# Quyết định thiết kế cuối cho MDCT core AAC-LC

## 1. Kết luận

Profile chính thức là `FDK_AACLC_1024_Q31Q15_RAD2_V1`. Phương án prototype
Q1.23 đã loại bỏ. Cùng một hợp đồng được dùng cho VHDL RTL, C++ fixed-point và
Python integer/NumPy.

| Hạng mục | Trạng thái cuối |
|---|---|
| Chuẩn số và exponent | Đã khóa |
| FFT radix-2 64/512 | Hoàn tất, stage và output `0 LSB` |
| Window ROM + TDAC fold | Hoàn tất RTL/reference |
| DCT-IV pre/post | Hoàn tất RTL/reference |
| Controller LONG/START/SHORT/STOP | Hoàn tất |
| State SINE/KBD giữa frame | Hoàn tất |
| Golden vector từng ranh giới | Hoàn tất, có version/checksum |
| Test RTL end-to-end | PASS `0 LSB` |
| So với `mdct_block()` FDK nguyên bản | PASS `0 LSB` |
| NumPy float64 oracle | PASS tiêu chí RMS |
| GHDL synthesis-elaboration/RAM inference | PASS |
| Vivado post-route | Có flow; cần chạy trên máy có Vivado để lấy số thật |

## 2. Ý nghĩa của bốn việc “chốt chuẩn số”

### 2.1 Chọn Q31 tương thích FDK thay vì Q1.23

Q-format quyết định độ rộng RAM, multiplier, vị trí binary point, ngưỡng
overflow và cách diễn giải output. Chọn Q31/Q15 làm cho RTL dùng trực tiếp ROM
FDK và so được từng bit với code encoder. Q1.23 chỉ thích hợp làm prototype độc
lập; ghép nó vào FDK sẽ cần đổi scale ở nhiều ranh giới và mất bit-exact.

Thông số cuối:

- input PCM signed 16-bit Q15;
- dữ liệu transform signed 32-bit Q31;
- hệ số window/twiddle signed 16-bit Q15;
- output 1024 mantissa Q31 cùng exponent 12 hoặc 9.

### 2.2 Chốt chiều FFT và pre/post-twiddle

Sai dấu FFT, sai ordering hoặc sai table stride đều cho phổ có vẻ “hợp lý”
nhưng không tương thích encoder. Profile khóa:

- forward FFT với kernel `exp(-j*2*pi*k/N)`;
- natural-order complex input/output;
- pure radix-2 DIT, `N=512` cho long và `N=64` cho short;
- pre ROM theo `SineWindow1024/128`;
- post ROM `SineTable1024` stride 2/16;
- hệ số `sqrt(1/2)` Q15 là `0x5A82`.

### 2.3 Chốt truncation, wrap và exponent

Fixed-point không chỉ là công thức đại số. Hai biểu thức toán học tương đương có
thể lệch nhiều LSB nếu đổi vị trí truncate. Mỗi tích được truncate trước
add/sub như generic FDK path; dịch phải là arithmetic; không rounding; add/sub
wrap two's-complement 32 bit; không saturation trong transform.

Exponent tích lũy là:

```text
fold +2 -> pre +2 -> FFT +(8 hoặc 5) -> post +0
```

Do đó LONG/START/STOP trả 12, SHORT trả 9.

### 2.4 Golden riêng cho từng ranh giới

Golden boundary làm ba việc: định vị stage lỗi, bảo vệ ordering/address
generator, và ngăn một thay đổi “tối ưu” vô tình đổi numerical contract.
`golden/radix2_q31_v1/` lưu PCM, fold, pre, mọi FFT stage, post, final, ROM,
manifest, checksum và metrics NumPy. Tên profile/version phải đổi nếu chủ ý đổi
hợp đồng số.

## 3. Quyết định kiến trúc RTL

Kiến trúc được chọn là iterative multi-engine với RAM rõ ràng:

- controller tách khỏi datapath;
- PCM/work/FFT-cache/spectrum có wrapper RAM riêng;
- window/fold, pre, FFT và post là bốn engine độc lập;
- FFT dùng address generator theo stage và hai bank ping-pong;
- butterfly nhân các nhánh thực song song, pipeline một butterfly/clock;
- SHORT tái sử dụng cùng datapath tám lần thay vì nhân tám core;
- trace synthesis-time generic phục vụ kiểm chứng nhưng bị loại trong production.

Kiến trúc này ưu tiên khả năng suy luận BRAM/DSP, timing rõ, tái sử dụng FFT và
diện tích hợp lý. Một FFT fully-unrolled không cần thiết cho thời hạn một frame
AAC-LC và làm tài nguyên tăng quá mạnh.

## 4. Kết nối BlockSwitch và buffer

BlockSwitch nối vào `mdct_core.block_type` và `mdct_core.right_shape`. Xung
`start` phải đi cùng quyết định ứng với đúng snapshot. Buffer cấp 2048 mẫu liên
tục qua `pcm_*`; không chuyển PCM qua chính BlockSwitch.

```text
analysis PCM ring buffer ---- pcm_valid/index/data ---> mdct_core
BlockSwitch ----------------- block_type/right_shape --> mdct_core
frame scheduler ------------- start/clear_history ----> mdct_core
mdct_core ------------------- spectrum + exponent ----> AAC quantizer path
```

## 5. Điều kiện để thay đổi profile

Một thay đổi chạm vào twiddle, width, rounding, saturation, scale mask, FFT
direction, ordering hoặc exponent phải thực hiện đồng thời:

1. đặt profile/version mới;
2. cập nhật C++ và Python model trước;
3. tái sinh toàn bộ ROM/golden/checksum;
4. chạy NumPy oracle;
5. chạy C++↔Python, RTL boundary và original-FDK equivalence;
6. đo lại synthesis/timing.

Không chấp nhận chỉ sửa RTL cho đến khi output “gần đúng”. Mục tiêu của nhánh
này là tương thích bit-exact với FDK-AAC generic Q15-table path.

## 6. Việc tiếp theo ở cấp hệ thống

MDCT transform core đã hoàn chỉnh. Phần còn lại để chạy encoder trên phần cứng
là công việc tích hợp cấp hệ thống, không phải bổ sung thuật toán vào core:

1. viết wrapper ring-buffer/BlockSwitch theo giao thức tại `mdct_core.md`;
2. nối spectrum và exponent vào psychoacoustic/quantizer;
3. chạy Vivado synthesis/place/route cho FPGA đích và khóa timing/resource;
4. regression nhiều stream WAV/AAC ở cấp encoder, gồm reset, flush và chuyển
   chuỗi LONG→START→SHORT→STOP→LONG.
