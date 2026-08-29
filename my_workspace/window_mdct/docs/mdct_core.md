# Đặc tả khối `mdct_core`

## 1. Mục đích và phạm vi

Tài liệu này là hợp đồng chức năng, giao tiếp và số học của khối RTL
`mdct_core` dùng trong encoder AAC-LC. Các từ **phải**, **không được** và
**chỉ khi** biểu thị yêu cầu bắt buộc.

Khối có các thuộc tính cố định sau:

- profile: `FDK_AACLC_1024_Q31Q15_RAD2_V1`;
- chiều dài frame AAC: 1024 mẫu;
- đầu vào mỗi frame: snapshot 2048 mẫu PCM signed 16-bit;
- đầu ra mỗi frame: 1024 mantissa signed 32-bit và một exponent chung;
- hỗ trợ đủ `LONG`, `START`, `SHORT`, `STOP` và cửa sổ `SINE`, `KBD`;
- kết quả phải bit-exact với đường generic Q15-table của FDK-AAC.

Top-level được đặc tả là `rtl/mdct_core.vhd`. Block switch, ring buffer PCM,
psychoacoustic, quantizer, bitstream formatter, AAC-ELD và IMDCT nằm ngoài
phạm vi.

## 2. Tổng quan chức năng

Với mỗi frame, khối thực hiện:

```text
2048 PCM Q15
  -> analysis window + TDAC fold
  -> DCT-IV pre-rotation/packing
  -> forward FFT radix-2, 512 hoặc 64 điểm
  -> DCT-IV post-rotation/unpacking
  -> 1024 hệ số MDCT Q31 + exponent
```

Kiến trúc triển khai:

```text
                     +----------------+
 block metadata ---->|  mdct_control  |<---- done của từng engine
                     +-------+--------+
                             | geometry/start
                             v
 PCM RAM -> window/fold -> work RAM -> DCT4 pre -> FFT radix-2
                                                       |
 spectrum RAM <- DCT4 post <- FFT cache RAM <----------+
```

Các engine được tái sử dụng tuần tự. Frame `SHORT` chạy cùng datapath tám lần,
không nhân bản tám FFT.

## 3. Hằng số, mã hóa và kiểu dữ liệu

| Thuộc tính | Giá trị |
|---|---:|
| `MDCT_FRAME_LEN` | 1024 |
| `MDCT_SHORT_LEN` | 128 |
| `MDCT_SNAPSHOT_N` | 2048 |
| `MDCT_SHORT_COUNT` | 8 |
| PCM | `signed(15 downto 0)`, Q15 |
| Dữ liệu transform/output | `signed(31 downto 0)`, Q31 |
| Hệ số window/twiddle | `signed(15 downto 0)`, Q15 |
| Địa chỉ PCM | `unsigned(10 downto 0)`, miền hợp lệ `0..2047` |
| Địa chỉ spectrum | `unsigned(9 downto 0)`, miền hợp lệ `0..1023` |

Mã hóa `block_type` phải trùng `WINDOW_SEQUENCE` của FDK-AAC:

| `block_type` | Tên | Ý nghĩa |
|---|---|---|
| `00` | `LONG` | một DCT-IV 1024, hai slope dài |
| `01` | `START` | một DCT-IV 1024, chuyển sang slope ngắn bên phải |
| `10` | `SHORT` | tám DCT-IV 128 |
| `11` | `STOP` | một DCT-IV 1024, chuyển từ slope ngắn bên trái |

Mã hóa `right_shape`:

| `right_shape` | Cửa sổ |
|---|---|
| `0` | `SINE` |
| `1` | `KBD` |

## 4. Giao tiếp top-level

### 4.1 Generic

| Tên | Mặc định | Yêu cầu |
|---|---:|---|
| `ENABLE_TRACE` | `false` | `false` khi tổng hợp production; `true` khi regression cần quan sát ranh giới nội bộ |

### 4.2 Clock và reset

| Port | Chiều | Độ rộng | Mô tả |
|---|---|---:|---|
| `clk` | in | 1 | clock duy nhất, dữ liệu được lấy mẫu tại cạnh lên |
| `rst_n` | in | 1 | reset bất đồng bộ, tích cực mức thấp |

Khi `rst_n='0'`:

- controller và các valid/busy phải về trạng thái nghỉ;
- `pcm_ready`, `spec_rd_valid`, `busy` và `done` phải bằng `0`;
- history cửa sổ bị vô hiệu;
- nội dung các RAM không cần xóa và không được xem là dữ liệu hợp lệ;
- nguồn vào phải giữ `pcm_valid`, `start`, `spec_rd_en` ở `0`.

Khi sử dụng ở cấp hệ thống, `rst_n` phải được giữ thấp qua ít nhất một cạnh lên
`clk`. Giao dịch đầu tiên chỉ được phát sau khi `rst_n` đã lên `1`.

### 4.3 Cổng nạp snapshot PCM

| Port | Chiều | Độ rộng | Mô tả |
|---|---|---:|---|
| `pcm_valid` | in | 1 | yêu cầu nạp một mẫu |
| `pcm_ready` | out | 1 | mẫu và index hiện tại có thể được nhận |
| `pcm_index` | in | 11 | index tự nhiên của mẫu |
| `pcm_data` | in | 16 | mẫu PCM signed Q15 |

Một mẫu được nhận tại cạnh lên khi đồng thời
`pcm_valid='1' and pcm_ready='1'`. Nguồn phải nạp đúng 2048 giao dịch với
`pcm_index` liên tục từ `0` đến `2047`; không được lặp, bỏ hoặc đổi thứ tự
index.

`pcm_ready` bao gồm kiểm tra index đang chờ. Vì vậy `pcm_ready='0'` nếu:

- core không ở pha load;
- đã nhận đủ 2048 mẫu;
- `pcm_index` khác index kế tiếp;
- `pcm_index` chứa giá trị unknown;
- reset đang tích cực.

Nguồn được phép nạp một mẫu mỗi clock và phải giữ `pcm_valid`, `pcm_index`,
`pcm_data` ổn định cho tới khi giao dịch được nhận. Snapshot mới ghi đè snapshot
cũ theo index tương ứng.

### 4.4 Cổng lệnh frame và trạng thái

| Port | Chiều | Độ rộng | Mô tả |
|---|---|---:|---|
| `start` | in | 1 | xung lệnh đúng một clock |
| `block_type` | in | 2 | loại block của snapshot hiện tại |
| `right_shape` | in | 1 | shape cửa sổ bên phải do block switch chọn |
| `clear_history` | in | 1 | xóa context cửa sổ trước lệnh kế tiếp |
| `busy` | out | 1 | datapath đang xử lý transform |
| `done` | out | 1 | frame hoàn tất, xung đúng một clock |
| `mdct_exp` | out | 5 | exponent chung của 1024 mantissa output |

`start` chỉ hợp lệ khi:

1. core đã nhận đủ 2048 mẫu;
2. `pcm_valid='0'` trong cùng chu kỳ;
3. core chưa xử lý một frame khác;
4. `block_type` và `right_shape` đã ổn định trước cạnh nhận lệnh.

Core chốt metadata tại cạnh nhận `start`. `block_type` và `right_shape` được
phép đổi sau cạnh đó. Không được giữ `start` cao nhiều clock.

`busy` lên sau cạnh nhận `start`, giữ mức `1` trong toàn bộ các sub-transform
và về `0` trong chu kỳ `done`. `busy='0'` trong pha load nên không được dùng như
tín hiệu `start_ready`.

`done='1'` xác nhận cả 1024 giá trị spectrum đã được ghi xong. `mdct_exp` hợp lệ
chậm nhất trong chu kỳ `done` và phải được scheduler chốt cùng frame output.

`clear_history` có hiệu lực khi ở pha load. Có thể giữ nó cao một hoặc nhiều
clock trước `start`; nếu cùng cao với `start`, lệnh đó dùng hành vi cold-start.
Tín hiệu này chỉ xóa context cửa sổ, không xóa số mẫu PCM đã nạp. Muốn hủy một
snapshot đang nạp dở phải dùng `rst_n`.

### 4.5 Cổng đọc spectrum

| Port | Chiều | Độ rộng | Mô tả |
|---|---|---:|---|
| `spec_rd_en` | in | 1 | yêu cầu đọc đồng bộ |
| `spec_rd_index` | in | 10 | bin tự nhiên `0..1023` |
| `spec_rd_valid` | out | 1 | dữ liệu trả về hợp lệ |
| `spec_rd_data` | out | 32 | mantissa MDCT signed Q31 |

Một yêu cầu được lấy mẫu khi `spec_rd_en='1'` tại cạnh lên. Sau đúng một clock,
`spec_rd_valid='1'` và `spec_rd_data` tương ứng với `spec_rd_index` đã lấy mẫu.
Cổng hỗ trợ một yêu cầu mỗi clock, do đó có thể đọc liên tục 1024 bin trong
1024 clock sau khi pipeline đọc đã được khởi động.

Scheduler chỉ được xem spectrum là hợp lệ sau `done` và phải đọc/chốt đủ output
trước khi phát `start` cho frame mới. Spectrum RAM chỉ có một bank; post engine
của frame kế tiếp sẽ ghi đè dữ liệu cũ. `spec_rd_en` phải bằng `0` khi reset.

### 4.6 Cổng trace tùy chọn

Các cổng trace không thuộc giao tiếp dữ liệu production:

| Nhóm | Các port | Giao dịch được quan sát |
|---|---|---|
| sub-transform | `trace_sub_index[2:0]` | chỉ số `q=0..7` hiện hành |
| fold | `trace_fold_valid/index/data` | scalar Q31 sau window/fold |
| pre | `trace_pre_valid/index/re/im` | complex Q31 đã được FFT nhận |
| FFT output | `trace_fft_valid/index/re/im` | complex Q31 đọc từ FFT |
| post | `trace_post_valid/index/data` | scalar Q31 đã ghi spectrum |
| FFT stage | `trace_fft_stage_*` | hai kết quả sau mỗi butterfly |

Khi `ENABLE_TRACE=false`, mọi output trace phải bằng `0` và logic trace được
phép loại bỏ khi tổng hợp. Khi `true`, `trace_*_valid` chỉ báo giao dịch đã được
chấp nhận, không chỉ báo dữ liệu đang được trình bày nhưng bị backpressure.

## 5. Hình học transform và thứ tự output

| Block | Số DCT-IV | `L` | FFT | Input base | Output base | Right slope | `mdct_exp` |
|---|---:|---:|---:|---:|---:|---:|---:|
| LONG | 1 | 1024 | 512 | 0 | 0 | 1024 | 12 |
| START | 1 | 1024 | 512 | 0 | 0 | 128 | 12 |
| SHORT | 8 | 128 | 64 | `448 + 128*q` | `128*q` | 128 | 9 |
| STOP | 1 | 1024 | 512 | 0 | 0 | 1024 | 12 |

Với `SHORT`, `q=0..7` và các sub-transform phải chạy theo thứ tự tăng dần.
Output của sub-transform `q` nằm tại:

```text
spec[128*q .. 128*q+127]
```

Với ba loại block dài, output nằm tại `spec[0..1023]`. Trong mọi mode,
`spec_rd_index` là thứ tự tự nhiên cuối cùng; wrapper ngoài không được
bit-reverse hoặc hoán vị lại.

## 6. State cửa sổ giữa các frame

Core lưu `right_shape` và chiều right slope của transform trước làm shape/slope
bên trái cho transform kế tiếp.

- right slope là 128 với `START` và `SHORT`;
- right slope là 1024 với `LONG` và `STOP`;
- khi history chưa hợp lệ hoặc `clear_history='1'`, left shape/slope phải bằng
  right shape/slope của transform hiện tại;
- sau mỗi transform, right shape/slope hiện tại trở thành history;
- trong frame `SHORT`, history phải cập nhật sau từng sub-transform, vì vậy
  sub-transform `q>0` dùng slope/shape ngắn của sub-transform trước;
- history hiện tại chỉ chứa một context kênh.

Core không kiểm tra chuỗi chuyển block AAC. Scheduler phải cấp chuỗi hợp lệ,
điển hình:

```text
LONG -> START -> SHORT -> STOP -> LONG
```

Nếu time-multiplex nhiều kênh trên một core, wrapper phải tự save/restore
history hoặc reset history giữa các kênh. Cách đơn giản là dùng một core cho
mỗi kênh.

## 7. Thuật toán bắt buộc

### 7.1 Window và TDAC fold

Xét một transform cục bộ dài `L`, đặt:

```text
H  = L/2
fl = chiều left slope
fr = chiều right slope
nl = (L-fl)/2
nr = (L-fr)/2
```

Với `x[]` là `2L` mẫu bắt đầu tại input base, vector `fold[0..L-1]` phải tương
đương bốn miền của `mdct_block()`:

```text
fold[H+i]        = -(x[L-1-i] << 15)                         i=0..nl-1
fold[H+nl+i]     = x[nl+i]*wL.im - x[L-nl-i-1]*wL.re         i=0..fl/2-1
fold[H-1-i]      = -(x[L+i] << 15)                           i=0..nr-1
fold[H-nr-i-1]   = -(x[L+nr+i]*wR.re
                      + x[2L-nr-i-1]*wR.im)                  i=0..fr/2-1
```

Kết quả fold phải được ghi theo index tự nhiên và có exponent tích lũy bằng 2.

### 7.2 DCT-IV pre-rotation

Pre engine phải pack `L` scalar thành `M=L/2` số phức theo đúng rearrangement
của `dct_IV()` FDK, dùng `SineWindow1024` hoặc `SineWindow128`. Input FFT phải
ở thứ tự tự nhiên. Pre-rotation tăng exponent thêm 2.

### 7.3 FFT radix-2

FFT phải tuân theo:

- forward FFT với kernel `exp(-j*2*pi*k/N)`;
- pure radix-2 DIT;
- natural-order input và output;
- `N=512`, 9 stage cho block dài;
- `N=64`, 6 stage cho block ngắn;
- scale mask theo thứ tự stage là `101111111` cho FFT-512 và `101111` cho
  FFT-64;
- stage 2 không scale; các stage còn lại scale một bit;
- exponent FFT lần lượt là 8 và 5.

Input được lưu bit-reversed bên trong FFT; đây là chi tiết nội bộ và không làm
thay đổi hợp đồng natural-order ở các ranh giới.

### 7.4 DCT-IV post-rotation

Post engine phải dùng `SineTable1024` với stride vật lý 2 cho `L=1024`, stride
16 cho `L=128`, và hằng Q15 `sqrt(1/2)=0x5A82` ở hai mẫu giữa. Post-rotation
không tăng exponent và phải xuất scalar theo thứ tự FDK cuối cùng.

## 8. Hợp đồng số học bit-exact

Các quy tắc sau là một phần của chức năng, không phải tùy chọn triển khai:

- PCM, hệ số và dữ liệu lần lượt là signed Q15, signed Q15 và signed Q31;
- nhân Q15 x Q15 tạo trực tiếp kết quả 32-bit dùng ở biên fold;
- mỗi tích Q31 x Q15 phải được dịch phải số học và truncate riêng trước phép
  cộng/trừ phức bao quanh;
- không round-to-nearest;
- cộng, trừ và đổi dấu wrap modulo `2^32` theo two's-complement;
- không saturation trong toàn bộ transform;
- thứ tự truncate, add/sub, shift và negate phải giữ đúng đường generic FDK;
- ROM phải dùng literal/profile FDK đã khóa, không tái lượng tử hóa khi tổng hợp.

Ngân sách exponent:

| Ranh giới | Block dài | SHORT |
|---|---:|---:|
| Sau fold | 2 | 2 |
| Sau pre | 4 | 4 |
| Sau FFT | 12 | 9 |
| Sau post/output | 12 | 9 |

Output phải được chuyển tiếp dưới dạng cặp `(spec_rd_data, mdct_exp)`. Khối sau
không được bỏ exponent hoặc tự đổi Q-format nếu vẫn yêu cầu tương thích FDK.

## 9. Timing và thông lượng

### 9.1 Quy ước đo

Một chu kỳ được tính từ cạnh lên nhận `start` đến cạnh lên làm `done` lên `1`.
Các số dưới đây giả sử không reset và các kết nối nội bộ không bị backpressure;
đó cũng là cấu hình của `mdct_core` hiện tại.

Với một sub-transform chiều dài `L`, `M=L/2`, latency là:

```text
Csub = 6*L + Cfft + 11
Cfft = log2(M) * (M/2 + 5)
```

| Loại frame | Latency | Thời gian tại 100 MHz |
|---|---:|---:|
| LONG/START/STOP | 8.504 clock | 85,04 us |
| SHORT, đủ 8 sub-transform | 8.008 clock | 80,08 us |

Ngoài latency transform:

- nạp snapshot liên tục cần 2048 clock;
- cổng spectrum có thể nhận một yêu cầu đọc mỗi clock;
- đọc đủ 1024 bin cần 1024 yêu cầu liên tục, dữ liệu đầu tiên trả sau một clock;
- thiết kế không chồng lấp xử lý hai frame và không có double-buffer spectrum.

Constraint production hiện tại là 100 MHz (`clk=10 ns`, uncertainty 0,2 ns).
Timing chỉ được xem là đạt trên FPGA đích khi post-route WNS không âm và không
có path quan trọng bị unconstrained.

## 10. Hành vi ngoài hợp đồng và assertion

RTL có assertion mô phỏng cho các lỗi như:

- index PCM sai thứ tự, thừa hoặc unknown;
- `start` trước khi đủ snapshot;
- nạp PCM và `start` cùng chu kỳ;
- start một engine khi engine đang bận;
- mode/geometry đổi giữa transform;
- response RAM/ROM lệch tag;
- completion pulse xuất hiện sai trạng thái;
- scale exponent nội bộ sai profile.

Các assertion không thay thế kiểm tra ở wrapper và có thể bị loại khi tổng hợp.
Nếu vi phạm giao thức, output và khả năng tự phục hồi không được bảo đảm. Wrapper
phải ngăn lỗi; nếu cần phục hồi một transaction dở dang, phải reset core.

## 11. Yêu cầu tích hợp

Wrapper cấp hệ thống phải:

1. duy trì ring buffer và xuất đúng snapshot 2048 mẫu của frame hiện tại;
2. ghép `block_type`, `right_shape` với đúng snapshot;
3. chỉ phát `start` sau khi đủ 2048 giao dịch PCM;
4. latch `mdct_exp` tại `done`;
5. đọc đủ 1024 bin trước `start` của frame tiếp theo;
6. giữ đúng context history cho từng kênh;
7. không điều khiển `size_mode`, twiddle hoặc địa chỉ FFT trực tiếp.

Block switch chỉ cấp metadata:

```text
PCM ring buffer ---- pcm_valid/index/data ---> mdct_core
BlockSwitch -------- block_type/right_shape --> mdct_core
frame scheduler ---- start/clear_history -----> mdct_core
mdct_core ---------- spectrum + exponent ----> quantizer path
```

## 12. Tiêu chí nghiệm thu

Một thay đổi RTL chỉ đạt nếu thỏa đồng thời:

1. compile/elaborate toàn bộ `mdct_core` và FFT dùng chung;
2. khớp golden `0 LSB` tại fold, pre, từng stage FFT, FFT output, post và toàn
   bộ 1024 hệ số cuối;
3. exponent bằng 12 cho LONG/START/STOP và 9 cho SHORT;
4. C++ fixed model khớp Python integer model `0 LSB`;
5. reference radix-2 khớp `mdct_block()` FDK nguyên bản `0 LSB`;
6. NumPy float64 xác nhận dấu, gain và ordering trong ngưỡng đã khóa;
7. pass reset, history, chuỗi `LONG -> START -> SHORT -> STOP -> LONG`, cả SINE
   và KBD;
8. synthesis suy luận đúng các RAM chính và post-route đạt constraint FPGA đích.

Bộ regression hiện tại gồm 3 case, 18 frame và 39 sub-transform; kiểm tra mọi
ranh giới số học với sai số cho phép đúng bằng `0 LSB`.

## 13. Phân rã source

| Nhóm | File | Trách nhiệm |
|---|---|---|
| Top | `rtl/mdct_core.vhd` | nối control, datapath, memory và FFT adapter |
| Control | `rtl/mdct_control.vhd` | protocol, geometry, sequencing và window history |
| Fold | `rtl/mdct_window_fold.vhd` | window + TDAC fold |
| Pre/Post | `rtl/mdct_dct4_pre.vhd`, `rtl/mdct_dct4_post.vhd` | phân tích DCT-IV quanh FFT |
| ROM | `rtl/mdct_window_rom.vhd`, `rtl/mdct_rotation_rom.vhd` | hệ số Q15 đã khóa |
| RAM | `rtl/mdct_*_memory.vhd` | PCM, work, FFT cache và spectrum |
| FFT | `fft_radix2_core/rtl/` | FFT-64/512 dùng chung |
| Reference | `ref/fixed_mdct_radix2.*` | statement model fixed-point |
| Regression | `tb/tb_mdct_core.vhd` | kiểm tra boundary và output bit-exact |

Luồng thuật toán chi tiết nằm ở [mdct_workflow.md](mdct_workflow.md); mô hình
tham chiếu và golden contract nằm ở
[window_mdct_reference_model.md](window_mdct_reference_model.md).
