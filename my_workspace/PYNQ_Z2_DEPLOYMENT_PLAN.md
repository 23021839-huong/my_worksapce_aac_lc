# Kế hoạch triển khai bộ mã hóa AAC-LC HW/SW codesign trên PYNQ-Z2

## 1. Mục đích tài liệu

Tài liệu này tổng hợp hiện trạng, kiến trúc đích, hợp đồng giao tiếp, các hạng
mục cần phát triển và kế hoạch kiểm chứng để triển khai mô hình hiện tại lên
kit PYNQ-Z2.

Mục tiêu hệ thống:

- bộ mã hóa âm thanh tuân theo AAC-LC;
- Block Switching chạy trên Processing System, tức hai lõi ARM Cortex-A9;
- Window/MDCT chạy trên Programmable Logic;
- các khối psychoacoustic, TNS, PNS, lượng tử hóa, Huffman và đóng gói ADTS
  tiếp tục chạy trên PS thông qua FDK-AAC;
- bản đầu tiên hỗ trợ mono, PCM16, frame AAC 1024 mẫu;
- kết quả MDCT phần cứng phải tương thích fixed-point với FDK-AAC;
- toàn bộ encoder phải chạy end-to-end trên PYNQ-Z2 và tạo được file AAC hợp
  lệ.

Tài liệu này là kế hoạch triển khai cấp hệ thống. Nó không thay thế đặc tả RTL
chi tiết của MDCT tại
[window_mdct/docs/mdct_core.md](window_mdct/docs/mdct_core.md) hoặc mô hình
Block Switching tại
[block_switch/docs/block_switch_reference_model.md](block_switch/docs/block_switch_reference_model.md).

---

## 2. Phạm vi phiên bản đầu tiên

Để tránh mở rộng phạm vi quá sớm, phiên bản đầu tiên nên khóa cấu hình:

| Thuộc tính | Cấu hình ban đầu |
|---|---|
| Audio Object Type | AAC-LC |
| Số kênh | Mono |
| Định dạng PCM | Signed PCM16 |
| Sample rate ưu tiên | 48 kHz |
| Frame length | 1024 mẫu/kênh |
| Transport | ADTS |
| Block Switching | PS Cortex-A9 |
| Window/MDCT | PL |
| Các khối AAC còn lại | PS Cortex-A9 |
| Clock PL mục tiêu | 100 MHz từ FCLK_CLK0 |
| Giao tiếp dữ liệu | AXI DMA |
| Giao tiếp điều khiển | AXI4-Lite |
| Hệ điều hành | Linux/PYNQ trên PS |

Sau khi mono 48 kHz chạy ổn định mới mở rộng lần lượt:

1. 44,1 kHz và 32 kHz;
2. nhiều bitrate;
3. stereo;
4. xử lý audio streaming thời gian thực;
5. đo công suất và tối ưu hiệu năng.

Các khối khác của AAC-LC không cần viết lại bằng RTL trong giai đoạn này.
FDK-AAC đã chứa toàn bộ psychoacoustic model, TNS, PNS, lượng tử hóa, bit
reservoir, Huffman và bitstream writer.

---

## 3. Đánh giá hiện trạng repository

### 3.1 Block Switching

Phần Block Switching hiện có:

- mã gốc FDK-AAC tại
  [../libAACenc/src/block_switch.cpp](../libAACenc/src/block_switch.cpp);
- reference model độc lập tại
  [block_switch/ref/ref_block_switch.cpp](block_switch/ref/ref_block_switch.cpp);
- harness đối chiếu DUT/reference;
- bộ kịch bản silence, sine, transient, castanets, crescendo, LFE và stereo;
- tài liệu hiện tại ghi nhận kết quả PASS, không lệch bit trên 292 frame và
  25 kịch bản.

Điểm cần lưu ý:

- reference model trong my_workspace dùng để kiểm chứng, không cần đưa vào
  production;
- production nên gọi trực tiếp FDKaacEnc_BlockSwitching của FDK-AAC trên ARM;
- phải chạy lại regression trên đúng bản ARM Cortex-A9 vì FDK có các nhánh
  hằng số và toán học phụ thuộc kiến trúc;
- Block Switching có trạng thái xuyên frame, do đó phải khởi tạo đúng một lần
  cho mỗi stream và mỗi kênh.

### 3.2 Window/MDCT

Phần Window/MDCT hiện có:

- RTL hoàn chỉnh cho LONG, START, SHORT và STOP;
- hỗ trợ cửa sổ SINE và KBD;
- LONG/START/STOP dùng FFT-512;
- SHORT dùng tám FFT-64;
- PCM signed Q15, dữ liệu Q31 và hệ số Q15;
- output gồm 1024 mantissa Q31 và một exponent chung;
- exponent bằng 12 với block dài và bằng 9 với SHORT;
- C++ model, Python integer model, NumPy oracle và RTL testbench;
- golden vector có version;
- tài liệu hiện tại ghi nhận kết quả 0 LSB tại fold, pre-rotation, từng stage
  FFT, post-rotation và toàn bộ spectrum.

Top RTL hiện tại là:

- [window_mdct/rtl/mdct_core.vhd](window_mdct/rtl/mdct_core.vhd);
- FFT dùng chung tại
  [window_mdct/fft_radix2_core/rtl](window_mdct/fft_radix2_core/rtl).

Giao diện hiện tại chỉ là giao diện RTL nội bộ:

- pcm_valid, pcm_ready, pcm_index, pcm_data;
- start, block_type, right_shape, clear_history;
- busy, done, mdct_exp;
- spec_rd_en, spec_rd_index, spec_rd_valid, spec_rd_data.

Nó chưa có AXI4-Lite, AXI4-Stream hoặc AXI DMA.

### 3.3 Input pipeline

Phần INPUT hiện có:

- mô phỏng đúng psyInputBuffer 2048 mẫu của FDK;
- sinh input 1024 mẫu cho Block Switching;
- sinh snapshot 2048 mẫu cho MDCT;
- mô phỏng đúng offset 1600;
- mô phỏng đúng việc chèn 448 mẫu trước MDCT và 576 mẫu sau MDCT;
- có ánh xạ frame và chỉ số mẫu tuyệt đối.

Trong hệ thống production không cần tạo lại logic ring buffer này trong PL ở
phiên bản đầu. FDK-AAC trên PS đã duy trì psyInputBuffer và có thể gửi trực
tiếp snapshot 2048 mẫu cho PL.

### 3.4 Những phần còn thiếu

Hiện repository chưa có:

- flow synthesis cho đúng chip PYNQ-Z2;
- báo cáo utilization/timing trên Zynq-7020;
- AXI wrapper cho MDCT;
- Vivado block design chứa Zynq PS và AXI DMA;
- bitstream, HWH hoặc XSA cho PYNQ-Z2;
- HAL/driver MDCT phía Linux;
- DMA buffer và cache-coherency handling;
- backend PL được móc vào FDK-AAC;
- ứng dụng encoder chạy trên PYNQ;
- kiểm thử hardware-in-the-loop;
- kiểm thử AAC end-to-end;
- đo latency, CPU load và throughput trên board.

### 3.5 Sai khác target FPGA hiện tại

Hai script synthesis hiện tại đang dùng part:

~~~text
xc7a35tcpg236-1
~~~

Đây là Artix-7 và không phải FPGA của PYNQ-Z2. Part cần dùng cho PYNQ-Z2 là:

~~~text
xc7z020clg400-1
~~~

Do đó kết quả mô phỏng hiện tại chứng minh chức năng số học, nhưng chưa chứng
minh core:

- fit tài nguyên trên XC7Z020;
- đạt timing 100 MHz trên XC7Z020;
- suy luận BRAM/DSP đúng trên board đích;
- hoạt động đúng sau place và route.

---

## 4. Kiến trúc hệ thống đích

### 4.1 Luồng xử lý

~~~mermaid
flowchart LR
    PCM["PCM16 input"] --> BUF["FDK psyInputBuffer<br/>PS"]
    PCM --> BS["Block Switching<br/>PS Cortex-A9"]
    BS --> META["block_type<br/>window_shape"]
    BUF --> DMAI["AXI DMA MM2S<br/>2048 PCM"]
    META --> CTRL["AXI4-Lite<br/>control registers"]
    DMAI --> WRAP["MDCT AXI wrapper<br/>PL"]
    CTRL --> WRAP
    WRAP --> CORE["mdct_core<br/>PL"]
    CORE --> DMAO["AXI DMA S2MM<br/>1024 Q31"]
    DMAO --> PSY["Psychoacoustic/TNS/PNS<br/>PS"]
    PSY --> QC["Quantization/Huffman<br/>PS"]
    QC --> AAC["ADTS AAC output"]
~~~

### 4.2 Phân chia HW/SW

| Khối | Vị trí | Lý do |
|---|---|---|
| WAV/PCM input | PS | I/O hệ điều hành |
| Buffer overlap/look-ahead | PS | FDK đã quản lý đúng |
| Block Switching | PS | Ít tính toán, nhiều state/control |
| Window/fold | PL | Một phần của datapath MDCT |
| DCT-IV pre/post | PL | Datapath fixed-point |
| FFT-64/512 | PL | Khối tính toán chính |
| Psychoacoustic | PS | Logic phức tạp, nhiều nhánh |
| TNS/PNS/MS | PS | Đã có trong FDK |
| Quantization/Huffman | PS | Đã có trong FDK |
| ADTS writer | PS | Bitstream control |

### 4.3 Quyết định về buffer

Phiên bản đầu gửi đầy đủ 2048 PCM cho mỗi frame.

Không nên thêm ring buffer vào PL ngay vì:

- FDK đã có buffer đúng;
- tránh việc PS và PL duy trì hai bản state overlap khác nhau;
- dễ so sánh từng frame với software;
- lưu lượng dữ liệu rất nhỏ;
- đơn giản hóa reset, seek và flush.

Có thể tối ưu sau bằng cách chỉ gửi 1024 mẫu mới và duy trì ring buffer trong
PL, nhưng chỉ nên làm sau khi phiên bản 2048 mẫu đã được nghiệm thu bit-exact.

---

## 5. Hợp đồng dữ liệu PS–PL

### 5.1 Dữ liệu input

Mỗi transaction MDCT nhận:

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| frame_id | uint32 | ID tăng dần của frame |
| block_type | 2 bit | LONG/START/SHORT/STOP |
| right_shape | 1 bit | SINE/KBD |
| clear_history | 1 bit | Xóa context cửa sổ khi bắt đầu stream |
| pcm | 2048 × int16 | Snapshot psyInputBuffer |

Ánh xạ block_type phải giữ đúng enum FDK:

| Giá trị | Block |
|---:|---|
| 0 | LONG |
| 1 | START |
| 2 | SHORT |
| 3 | STOP |

Ánh xạ window shape:

| Giá trị | Shape |
|---:|---|
| 0 | SINE |
| 1 | KBD |

### 5.2 Đóng gói AXI4-Stream input

Khuyến nghị stream rộng 32 bit:

~~~text
word[i][15:0]  = pcm[2*i]
word[i][31:16] = pcm[2*i+1]
i = 0..1023
~~~

Mỗi frame input gồm:

- 1024 word AXI4-Stream;
- TLAST bằng 1 tại word 1023;
- wrapper tách mỗi word thành hai giao dịch PCM16 cho mdct_core;
- wrapper tự sinh pcm_index từ 0 đến 2047.

Metadata không nên trộn vào payload PCM ở phiên bản đầu. PS ghi metadata qua
AXI4-Lite trước khi khởi động DMA MM2S.

### 5.3 Dữ liệu output

Mỗi frame trả:

| Trường | Kiểu | Ý nghĩa |
|---|---|---|
| frame_id | uint32 | Phải khớp frame input |
| spectrum | 1024 × int32 | Mantissa MDCT Q31 |
| mdct_exp | uint5 | Exponent chung |
| error_flags | uint32 | Trạng thái protocol/hardware |
| cycle_count | uint32 | Số clock xử lý |

AXI4-Stream output:

- rộng 32 bit;
- một word cho mỗi bin;
- thứ tự bin tự nhiên 0 đến 1023;
- TLAST bằng 1 tại bin 1023.

### 5.4 Exponent

MDCT output không chỉ là 1024 số Q31. Backend phải trả cả exponent:

| Block | mdct_exp |
|---|---:|
| LONG | 12 |
| START | 12 |
| SHORT | 9 |
| STOP | 12 |

FDK phải gán exponent này vào mdctSpectrum_e/mdctScale. Không được tự dịch
spectrum về một Q-format khác nếu mục tiêu vẫn là bit-exact với FDK.

### 5.5 State cửa sổ

Core hiện tại giữ right shape và right slope của transform trước để làm
left-context cho transform kế tiếp.

Quy tắc phiên bản mono:

- clear_history bằng 1 cho frame đầu stream;
- clear_history bằng 0 cho các frame liên tục;
- không xen hai stream trên cùng core;
- không đổi backend software/PL giữa stream;
- khi DMA hoặc core lỗi, kết thúc stream hoặc reset rồi bắt đầu stream mới;
- không âm thầm fallback sang software giữa stream vì state software và PL có
  thể đã lệch nhau.

Với stereo, core hiện chỉ có một context nên chưa đủ. Phải chọn một trong ba
phương án:

1. hai MDCT core, một core mỗi kênh;
2. thêm save/restore context cho từng kênh;
3. sửa core thành stateless, để PS truyền rõ left/right shape và slope.

Phiên bản đầu nên giới hạn mono.

---

## 6. Thiết kế AXI wrapper

### 6.1 Chức năng

Wrapper cần:

- slave AXI4-Lite cho control/status;
- slave AXI4-Stream cho PCM;
- master AXI4-Stream cho spectrum;
- bridge giữa stream và giao thức native của mdct_core;
- quản lý frame_id và metadata;
- phát hiện protocol error;
- bộ đếm latency;
- interrupt error/done nếu cần.

### 6.2 Register map đề xuất

| Offset | Tên | R/W | Nội dung |
|---:|---|---|---|
| 0x00 | IP_ID | R | Magic nhận dạng MDCT IP |
| 0x04 | IP_VERSION | R | Version giao diện/numerical contract |
| 0x08 | CONTROL | R/W | soft_reset, abort, irq_enable |
| 0x0C | STATUS | R | ready, loading, computing, outputting, done, error |
| 0x10 | FRAME_ID_IN | R/W | ID frame do PS ghi |
| 0x14 | FRAME_CONFIG | R/W | block_type, right_shape, clear_history |
| 0x18 | FRAME_ID_OUT | R | ID frame đã hoàn thành |
| 0x1C | MDCT_EXP | R | Exponent output |
| 0x20 | ERROR_FLAGS | R/W1C | Các lỗi protocol |
| 0x24 | CYCLE_COUNT | R | Clock từ nhận frame đến hoàn tất |
| 0x28 | IRQ_STATUS | R/W1C | Trạng thái interrupt |

CONTROL đề xuất:

| Bit | Tên | Ý nghĩa |
|---:|---|---|
| 0 | SOFT_RESET | Xóa FSM và transaction hiện tại |
| 1 | ABORT | Hủy frame đang chạy |
| 2 | IRQ_ENABLE | Cho phép interrupt |

STATUS đề xuất:

| Bit | Tên |
|---:|---|
| 0 | READY |
| 1 | LOADING |
| 2 | COMPUTING |
| 3 | OUTPUTTING |
| 4 | DONE |
| 5 | ERROR |
| 6 | HISTORY_VALID |

ERROR_FLAGS tối thiểu:

| Bit | Lỗi |
|---:|---|
| 0 | INPUT_EARLY_TLAST |
| 1 | INPUT_MISSING_TLAST |
| 2 | INPUT_EXTRA_WORD |
| 3 | START_WHILE_BUSY |
| 4 | CORE_PROTOCOL_ERROR |
| 5 | OUTPUT_BACKPRESSURE_OVERFLOW |
| 6 | ABORTED |
| 7 | FRAME_ID_MISMATCH |

### 6.3 FSM đề xuất

~~~text
IDLE
  -> LOAD_INPUT
  -> START_CORE
  -> WAIT_CORE
  -> READ_SPECTRUM
  -> OUTPUT_STREAM
  -> DONE
  -> IDLE
~~~

Nếu đọc spectrum và phát stream cùng lúc:

- spec_rd_data có latency một clock;
- wrapper phải giữ index/tag tương ứng;
- cần skid buffer hoặc FIFO ít nhất hai entry;
- chỉ phát yêu cầu đọc khi còn chỗ chứa response;
- phải xử lý đúng m_axis_tready deassert;
- không để core frame tiếp theo ghi đè spectrum trước TLAST output.

### 6.4 Trình tự transaction

PS thực hiện theo thứ tự:

1. kiểm tra STATUS.READY;
2. ghi FRAME_ID_IN;
3. ghi FRAME_CONFIG;
4. chuẩn bị buffer input/output;
5. flush cache input;
6. arm DMA S2MM trước để sẵn sàng nhận output;
7. start DMA MM2S để gửi input;
8. wrapper nhận TLAST và tự phát start cho mdct_core;
9. chờ DMA S2MM done hoặc interrupt;
10. invalidate cache output;
11. đọc FRAME_ID_OUT, MDCT_EXP và ERROR_FLAGS;
12. kiểm tra frame_id và error;
13. chuyển spectrum và exponent cho FDK.

Không nên để CPU phát start thủ công sau DMA input vì dễ có race giữa DMA done
và thời điểm wrapper đã nạp đủ 2048 PCM. TLAST input nên là sự kiện khởi động
core.

---

## 7. Vivado project cho PYNQ-Z2

### 7.1 Part và clock

~~~tcl
set part_name xc7z020clg400-1
~~~

Clock production:

- FCLK_CLK0 từ Zynq PS;
- tần số 100 MHz;
- reset qua Processor System Reset;
- toàn bộ DMA, wrapper và mdct_core dùng cùng clock ở phiên bản đầu;
- chưa cần clock-domain crossing.

### 7.2 Block design

Các IP cần có:

- ZYNQ7 Processing System;
- AXI DMA với MM2S và S2MM;
- AXI Interconnect hoặc SmartConnect phù hợp phiên bản Vivado;
- Processor System Reset;
- AXI Interrupt Controller hoặc xlconcat nếu dùng interrupt;
- MDCT AXI wrapper custom IP.

Kết nối đề xuất:

~~~text
PS M_AXI_GP0
  -> AXI interconnect
  -> MDCT wrapper S_AXI_CONTROL
  -> AXI DMA S_AXI_LITE

AXI DMA M_AXI_MM2S
  -> PS S_AXI_HP0

AXI DMA M_AXI_S2MM
  -> PS S_AXI_HP0

AXI DMA M_AXIS_MM2S
  -> MDCT wrapper S_AXIS_PCM

MDCT wrapper M_AXIS_SPEC
  -> AXI DMA S_AXIS_S2MM

FCLK_CLK0
  -> mọi AXI clock và mdct_core clock

DMA interrupt / wrapper error interrupt
  -> PS IRQ_F2P
~~~

### 7.3 Constraint

Không dùng nguyên xi block-level XDC standalone khi core đã nằm trong block
design.

Cần:

- để Vivado nhận clock từ FCLK_CLK0;
- kiểm tra không có unconstrained path;
- false path chỉ cho reset asynchronous nếu thực sự asynchronous;
- kiểm tra CDC nếu sau này dùng nhiều clock;
- kiểm tra timing cả AXI wrapper và MDCT, không chỉ mdct_core;
- đặt ENABLE_TRACE=false cho bản production.

### 7.4 Flow build tái lập

Nên có các Tcl script:

~~~text
my_workspace/pynq_z2/hw/tcl/create_project.tcl
my_workspace/pynq_z2/hw/tcl/package_mdct_ip.tcl
my_workspace/pynq_z2/hw/tcl/create_block_design.tcl
my_workspace/pynq_z2/hw/tcl/build_bitstream.tcl
my_workspace/pynq_z2/hw/tcl/export_overlay.tcl
~~~

Flow mong muốn:

~~~powershell
vivado -mode batch -source hw/tcl/create_project.tcl
vivado -mode batch -source hw/tcl/build_bitstream.tcl
~~~

Output cần lưu:

~~~text
overlay/aac_mdct.bit
overlay/aac_mdct.hwh
reports/utilization_post_synth.rpt
reports/utilization_post_route.rpt
reports/timing_post_route.rpt
reports/dsp_post_route.rpt
reports/methodology_post_route.rpt
~~~

Không nên commit toàn bộ thư mục Vivado project, cache hoặc run directory.
Nên commit source RTL, Tcl, XDC, register map và các báo cáo nghiệm thu.

### 7.5 Tiêu chí nghiệm thu phần cứng

- synth_design PASS cho xc7z020clg400-1;
- place_design và route_design PASS;
- WNS không âm tại 100 MHz;
- không có unconstrained critical path;
- không có critical warning ảnh hưởng chức năng;
- tài nguyên LUT/FF/BRAM/DSP nằm trong XC7Z020;
- RAM chính được suy luận thành BRAM, không bung toàn bộ thành register/LUTRAM;
- trace logic bị loại khi ENABLE_TRACE=false;
- bitstream và HWH được sinh tự động bằng Tcl.

---

## 8. Phần mềm trên Cortex-A9

### 8.1 Baseline software

Trước khi dùng PL cần build và chạy một bản FDK-AAC software thuần trên
PYNQ-Z2.

Baseline phải:

- build trực tiếp trên board hoặc cross-compile cho đúng root filesystem;
- tạo được file AAC-LC ADTS từ WAV test;
- giải mã lại thành công;
- lưu checksum output và log cấu hình encoder;
- đo thời gian encode tổng và thời gian MDCT software;
- dùng cùng compiler flags cho hai bản software và PL.

Nếu cross-compile, các tùy chọn Cortex-A9/NEON và hard-float phải khớp toolchain
và root filesystem thực tế. Không hardcode compiler flags khi chưa xác nhận ABI
của image PYNQ.

### 8.2 HAL MDCT PL

API đề xuất:

~~~cpp
struct MdctPlResult {
  int32_t spectrum[1024];
  uint32_t frame_id;
  uint32_t mdct_exp;
  uint32_t cycle_count;
  uint32_t error_flags;
};

int mdct_pl_open(MdctPlContext* ctx);
int mdct_pl_reset(MdctPlContext* ctx);
int mdct_pl_process(
    MdctPlContext* ctx,
    const int16_t pcm[2048],
    uint32_t frame_id,
    uint32_t block_type,
    uint32_t right_shape,
    bool clear_history,
    MdctPlResult* result);
void mdct_pl_close(MdctPlContext* ctx);
~~~

HAL phải tách khỏi FDK để có thể kiểm thử độc lập.

Trách nhiệm của HAL:

- load overlay hoặc kiểm tra overlay đã load;
- mmap AXI-Lite registers;
- cấp phát buffer DMA contiguous;
- flush/invalidate cache đúng lúc;
- khởi động DMA theo đúng thứ tự;
- timeout và reset khi lỗi;
- kiểm tra TLAST/length;
- kiểm tra frame_id;
- trả spectrum và exponent;
- ghi log đủ để debug transaction lỗi.

### 8.3 Backend abstraction trong FDK

Nên bổ sung abstraction thay vì ghi code DMA trực tiếp vào transform.cpp:

~~~cpp
enum MdctBackend {
  MDCT_BACKEND_SOFTWARE,
  MDCT_BACKEND_PL
};

int aac_mdct_process(
    MdctBackend backend,
    const INT_PCM* time_data,
    FIXP_DBL* mdct_data,
    int block_type,
    int window_shape,
    int* mdct_data_exp);
~~~

Hai binary hoặc hai mode runtime:

~~~text
fdk-aac software backend
fdk-aac PL backend
~~~

Software backend giữ nguyên FDKaacEnc_Transform_Real hiện tại.

PL backend:

1. lấy đúng psyInputBuffer 2048 mẫu;
2. lấy lastWindowSequence từ Block Switching;
3. lấy windowShape;
4. gọi mdct_pl_process;
5. copy 1024 Q31 về mdctSpectrum;
6. gán mdctSpectrum_e;
7. cập nhật prevWindowShape như software;
8. trả lỗi nếu DMA/core lỗi.

### 8.4 Vị trí tích hợp

Điểm móc phù hợp là lời gọi FDKaacEnc_Transform_Real trong:

- [../libAACenc/src/psy_main.cpp](../libAACenc/src/psy_main.cpp);
- implementation hiện tại tại
  [../libAACenc/src/transform.cpp](../libAACenc/src/transform.cpp).

Nên giới hạn thay đổi:

- không sửa thuật toán Block Switching;
- không sửa psychoacoustic/QC/bitstream;
- không đổi layout mdctSpectrum;
- không đổi lowpassLine handling;
- không đổi scale semantics;
- cho phép build lại backend software để regression.

### 8.5 Cache coherence

Đây là lỗi phổ biến khi DMA trên Zynq.

Trước MM2S:

- CPU ghi buffer input;
- flush data cache cho vùng buffer input;
- memory barrier;
- start DMA.

Sau S2MM:

- chờ DMA completion;
- invalidate data cache cho buffer output;
- memory barrier;
- CPU mới đọc spectrum.

Nếu dùng buffer allocator của PYNQ, phải dùng đúng API đồng bộ cache của phiên
bản đang chạy. Nếu dùng C userspace độc lập, cần một cơ chế DMA-safe buffer như
driver/kernel API, UIO kết hợp allocator phù hợp hoặc buffer contiguous được
hệ thống cung cấp. Không dùng địa chỉ virtual thông thường làm địa chỉ DMA.

### 8.6 Polling và interrupt

Giai đoạn bring-up:

- dùng polling có timeout;
- dễ debug register và transaction;
- log STATUS/ERROR_FLAGS khi timeout.

Giai đoạn hoàn thiện:

- dùng interrupt DMA S2MM completion;
- interrupt riêng cho wrapper error nếu cần;
- vẫn giữ watchdog timeout;
- handler chỉ báo completion, không làm xử lý AAC nặng trong interrupt.

### 8.7 Xử lý lỗi

Không fallback âm thầm từ PL sang software giữa stream.

Khi có lỗi:

1. ghi log frame_id, config, DMA status, wrapper status;
2. abort DMA;
3. soft-reset wrapper/core;
4. đánh dấu stream hiện tại lỗi;
5. bắt đầu stream mới với clear_history nếu muốn tiếp tục.

Fallback giữa stream có thể làm sai window history và tạo output không còn
bit-exact.

---

## 9. Kiểm chứng trên board

### 9.1 Nguyên tắc oracle

Phải có ba oracle độc lập:

1. golden vector đã khóa trong repository;
2. C++ fixed-point MDCT reference;
3. FDK-AAC software chạy trên chính Cortex-A9.

So sánh full encoder phải dùng cùng một ARM build, chỉ thay MDCT backend. Không
nên lấy bitstream x86 làm chuẩn duy nhất vì FDK có nhánh tối ưu kiến trúc.

### 9.2 Kiểm thử cấp AXI wrapper

Testbench wrapper cần kiểm tra:

- một frame LONG bình thường;
- SHORT đủ tám transform;
- backpressure input;
- backpressure output;
- TLAST sớm;
- thiếu TLAST;
- thừa word;
- reset khi IDLE;
- reset giữa LOAD;
- abort giữa COMPUTE;
- output tready giữ thấp nhiều clock;
- frame_id liên tiếp;
- clear_history;
- không rơi hoặc lặp spectrum word.

### 9.3 Hardware-in-the-loop MDCT

Mỗi frame kiểm tra:

~~~text
input PCM[2048]
block_type
right_shape
output spectrum[1024]
mdct_exp
frame_id
error_flags
~~~

Tiêu chí:

- 1024/1024 bin khớp 0 LSB;
- exponent khớp;
- không lỗi AXI/DMA;
- đúng thứ tự output;
- đúng chuỗi state cửa sổ.

### 9.4 Bộ tín hiệu tối thiểu

| Nhóm | Tín hiệu |
|---|---|
| Mức cơ bản | all-zero, DC, sine 1 kHz |
| Biên | +32767, -32768, alternating max/min |
| Transient | single impulse ở nhiều vị trí |
| Block switch | castanets, clap, attack lặp |
| Sequence | LONG→START→SHORT→STOP→LONG |
| Shape | SINE và KBD |
| Thực tế | abc_votay.wav và một corpus WAV đa dạng |
| Startup | frame 0 có 1600 zero prefix |
| Tail | frame không đầy, padding và flush |
| Reset | reset giữa hai stream |

### 9.5 So sánh encoder end-to-end

Chạy hai encoder trên cùng board:

~~~text
encoder_sw input.wav output_sw.aac
encoder_pl input.wav output_pl.aac
~~~

So sánh:

1. cấu hình encoder;
2. số frame;
3. block_type/window_shape từng frame;
4. MDCT mantissa/exponent từng frame trong debug build;
5. checksum bitstream;
6. so sánh byte;
7. giải mã cả hai file;
8. kiểm tra lỗi decoder;
9. so sánh PCM decode.

Mục tiêu cao nhất:

~~~text
output_sw.aac và output_pl.aac byte-identical
~~~

Nếu không byte-identical:

- tìm frame AAC đầu tiên khác;
- tìm kênh và spectrum bin đầu tiên khác;
- kiểm tra exponent trước;
- kiểm tra block_type/window_shape;
- kiểm tra endian và cache;
- kiểm tra ARM numerical path;
- không đánh giá chỉ bằng việc file vẫn nghe được.

### 9.6 Conformance

Tối thiểu:

- FDK decoder giải mã được toàn bộ file;
- FFmpeg giải mã được, nếu có trong môi trường;
- không có malformed ADTS frame;
- sample rate, channel count và duration đúng;
- không có lỗi buffer/bit reservoir.

Nếu có bộ AAC conformance test hợp pháp, bổ sung sau khi bit-exact regression
đã PASS.

---

## 10. Ngân sách hiệu năng

### 10.1 Chu kỳ frame

Với AAC-LC frame 1024:

| Sample rate | Thời gian một frame |
|---:|---:|
| 48 kHz | 21,333 ms |
| 44,1 kHz | 23,220 ms |
| 32 kHz | 32,000 ms |

### 10.2 Latency MDCT RTL hiện tại

Theo đặc tả core:

- nạp snapshot: 2048 clock;
- compute block dài tối đa: khoảng 8504 clock;
- đọc 1024 output: khoảng 1025 clock kể cả response đầu;
- tổng lý thuyết xấp xỉ 11577 clock;
- tại 100 MHz tương đương khoảng 115,77 microsecond, chưa tính DMA/software.

Đây là khoảng cách lớn so với deadline 21,333 ms tại 48 kHz.

### 10.3 Băng thông

Mono 48 kHz:

~~~text
input  = 4096 byte/frame
output = 4096 byte/frame
frame rate = 48000/1024 = 46,875 frame/s
total = 8192 * 46,875 = 384000 byte/s
~~~

Stereo nếu dùng hai core hoặc time-multiplex:

~~~text
khoảng 768000 byte/s
~~~

Băng thông rất thấp so với AXI HP/DMA. Điểm cần tối ưu thực tế là:

- chi phí setup DMA;
- cache maintenance;
- số lần context switch/interrupt;
- việc PS chờ đồng bộ PL;
- tổng CPU time của toàn encoder.

### 10.4 Chỉ số cần đo

- core cycle_count;
- DMA MM2S latency;
- core compute latency;
- DMA S2MM latency;
- tổng mdct_pl_process latency;
- thời gian Block Switching;
- thời gian toàn psychoacoustic;
- thời gian encode/frame;
- CPU utilization;
- frame deadline miss;
- tốc độ encode so với realtime;
- tốc độ backend software so với PL.

Điều kiện tối thiểu là không miss deadline. Mục tiêu kỹ thuật nên là tổng thời
gian PL transaction nhỏ hơn đáng kể một frame và có đủ margin cho toàn bộ các
khối FDK còn lại.

---

## 11. Cấu trúc thư mục đề xuất

Các file dưới đây là cấu trúc cần tạo trong quá trình triển khai:

~~~text
my_workspace/
├── PYNQ_Z2_DEPLOYMENT_PLAN.md
├── block_switch/
├── INPUT/
├── window_mdct/
└── pynq_z2/
    ├── README.md
    ├── hw/
    │   ├── rtl/
    │   │   ├── mdct_axi_wrapper.vhd
    │   │   ├── mdct_axis_input.vhd
    │   │   ├── mdct_axis_output.vhd
    │   │   └── mdct_axi_regs.vhd
    │   ├── tb/
    │   │   └── tb_mdct_axi_wrapper.vhd
    │   ├── tcl/
    │   │   ├── package_mdct_ip.tcl
    │   │   ├── create_project.tcl
    │   │   ├── create_block_design.tcl
    │   │   ├── build_bitstream.tcl
    │   │   └── export_overlay.tcl
    │   ├── constraints/
    │   └── reports/
    ├── overlay/
    │   ├── aac_mdct.bit
    │   └── aac_mdct.hwh
    ├── sw/
    │   ├── hal/
    │   │   ├── mdct_pl_hal.h
    │   │   └── mdct_pl_hal.cpp
    │   ├── backend/
    │   │   ├── aac_mdct_backend.h
    │   │   └── aac_mdct_backend.cpp
    │   ├── app/
    │   │   └── aac_encoder_pynq.cpp
    │   └── tests/
    │       ├── test_mdct_pl.cpp
    │       ├── compare_mdct_dump.py
    │       └── run_encoder_regression.py
    └── scripts/
        ├── deploy_overlay.ps1
        ├── run_board_smoke_test.sh
        └── collect_reports.sh
~~~

Không nhất thiết tách wrapper thành đúng bốn file như trên. Có thể bắt đầu bằng
một file rồi refactor, nhưng register map và numerical contract phải được khóa
từ đầu.

---

## 12. Kế hoạch triển khai theo mốc

### Mốc 0 — Khóa baseline

Nhiệm vụ:

- [ ] Khóa mono, PCM16, 48 kHz, AAC-LC, ADTS.
- [ ] Chọn một hoặc hai bitrate ban đầu.
- [ ] Build FDK-AAC software trên Cortex-A9.
- [ ] Encode abc_votay.wav.
- [ ] Lưu command line, log, checksum và thời gian chạy.
- [ ] Dump Block Switching và MDCT software từng frame.
- [ ] Xác nhận flush/tail của encoder đầy đủ.

Artefact:

- binary encoder software;
- output AAC baseline;
- checksum;
- trace block type/window shape;
- trace spectrum/exponent;
- benchmark baseline.

Tiêu chí hoàn thành:

- AAC software chạy end-to-end trên board;
- decoder giải mã được;
- kết quả có thể tái tạo.

### Mốc 1 — Port MDCT RTL sang XC7Z020

Nhiệm vụ:

- [ ] Tạo script synthesis cho xc7z020clg400-1.
- [ ] Giữ clock 100 MHz.
- [ ] Chạy synthesis.
- [ ] Chạy place/route.
- [ ] Kiểm tra RAM inference.
- [ ] Kiểm tra DSP inference.
- [ ] Kiểm tra WNS và unconstrained paths.
- [ ] Lưu báo cáo.

Artefact:

- Tcl target PYNQ-Z2;
- utilization report;
- timing report;
- routed checkpoint tùy chọn.

Tiêu chí hoàn thành:

- WNS không âm;
- fit tài nguyên;
- không có critical warning chức năng.

### Mốc 2 — AXI wrapper và testbench

Nhiệm vụ:

- [ ] Chốt register map.
- [ ] Viết AXI4-Lite slave.
- [ ] Viết AXI4-Stream input.
- [ ] Tách mỗi word thành hai PCM16.
- [ ] Sinh pcm_index.
- [ ] Tự start core từ TLAST.
- [ ] Viết output engine.
- [ ] Thêm skid buffer/FIFO cho backpressure.
- [ ] Thêm error flags.
- [ ] Thêm frame_id.
- [ ] Thêm cycle counter.
- [ ] Chạy simulation protocol.
- [ ] Chạy golden regression qua wrapper.

Artefact:

- wrapper RTL;
- wrapper testbench;
- register map;
- simulation logs.

Tiêu chí hoàn thành:

- spectrum và exponent vẫn khớp 0 LSB;
- PASS toàn bộ backpressure/reset/error tests.

### Mốc 3 — Vivado block design và overlay

Nhiệm vụ:

- [ ] Package custom IP.
- [ ] Tạo Zynq PS block.
- [ ] Bật GP0 và HP0.
- [ ] Thêm AXI DMA.
- [ ] Nối AXI-Lite control.
- [ ] Nối MM2S/S2MM stream.
- [ ] Nối clock/reset.
- [ ] Nối interrupt.
- [ ] Validate block design.
- [ ] Build bitstream.
- [ ] Export HWH.
- [ ] Chạy post-route timing cho toàn design.

Artefact:

- block design Tcl;
- bit;
- hwh;
- reports.

Tiêu chí hoàn thành:

- overlay load được trên PYNQ;
- AXI registers đọc đúng ID/version;
- DMA reset/idle bình thường.

### Mốc 4 — HAL và board smoke test

Nhiệm vụ:

- [ ] Viết mdct_pl_open/reset/process/close.
- [ ] Cấp phát DMA-safe buffers.
- [ ] Thêm cache synchronization.
- [ ] Dùng polling với timeout.
- [ ] Chạy một frame LONG.
- [ ] Chạy SHORT.
- [ ] Chạy chuỗi đầy đủ.
- [ ] So sánh 0 LSB với golden.
- [ ] Ghi latency.

Artefact:

- HAL C/C++;
- test_mdct_pl;
- board logs;
- comparison report.

Tiêu chí hoàn thành:

- tất cả golden frame khớp 0 LSB;
- exponent, frame_id và error flags đúng;
- không timeout.

### Mốc 5 — Tích hợp FDK-AAC

Nhiệm vụ:

- [ ] Tạo backend abstraction.
- [ ] Giữ software backend.
- [ ] Thêm PL backend.
- [ ] Truyền psyInputBuffer 2048 mẫu.
- [ ] Truyền block type/window shape.
- [ ] Nhận spectrum/exponent.
- [ ] Cập nhật prevWindowShape.
- [ ] Không thay các khối PS còn lại.
- [ ] Thêm trace debug từng frame.
- [ ] Xử lý lỗi có kiểm soát.

Artefact:

- FDK backend patch;
- encoder_pl binary;
- frame trace.

Tiêu chí hoàn thành:

- encoder PL chạy hết WAV;
- không lỗi DMA/core/FDK;
- số frame đúng.

### Mốc 6 — Nghiệm thu end-to-end

Nhiệm vụ:

- [ ] Chạy software và PL backend trên cùng ARM build.
- [ ] So block switching trace.
- [ ] So MDCT từng frame.
- [ ] So file AAC.
- [ ] Decode bằng nhiều decoder.
- [ ] Chạy corpus WAV.
- [ ] Chạy startup/tail/flush/reset tests.
- [ ] Đo CPU và latency.
- [ ] Lưu báo cáo cuối.

Tiêu chí hoàn thành:

- ưu tiên bitstream byte-identical;
- nếu không byte-identical, mọi sai khác phải được giải thích và vẫn phải đạt
  numerical/conformance contract đã khóa;
- không miss deadline realtime;
- build và test tái lập được.

### Mốc 7 — Mở rộng stereo

Chỉ bắt đầu sau Mốc 6.

Nhiệm vụ:

- [ ] Chọn hai core hoặc save/restore history.
- [ ] Deinterleave PCM L/R.
- [ ] Chạy FDKaacEnc_SyncBlockSwitching trên PS.
- [ ] Đảm bảo block type chung đúng cho CPE.
- [ ] Giữ context MDCT riêng từng kênh.
- [ ] Đo lại tài nguyên nếu dùng hai core.
- [ ] Chạy regression stereo và MS stereo.

---

## 13. Ma trận kiểm thử tổng hợp

| Test ID | Cấp | Input | Kỳ vọng |
|---|---|---|---|
| RTL-01 | mdct_core | LONG/SINE golden | 0 LSB |
| RTL-02 | mdct_core | START/KBD golden | 0 LSB |
| RTL-03 | mdct_core | 8 SHORT | 0 LSB |
| RTL-04 | mdct_core | STOP | 0 LSB |
| AXI-01 | wrapper | input liên tục | đủ 2048 mẫu |
| AXI-02 | wrapper | input backpressure | không mất mẫu |
| AXI-03 | wrapper | output backpressure | không mất bin |
| AXI-04 | wrapper | early TLAST | error flag |
| AXI-05 | wrapper | missing TLAST | timeout/error |
| AXI-06 | wrapper | reset giữa frame | phục hồi về IDLE |
| HIL-01 | board | golden LONG | 0 LSB |
| HIL-02 | board | golden SHORT | 0 LSB |
| HIL-03 | board | sequence đầy đủ | state đúng |
| HIL-04 | board | extreme PCM | wrap/truncate đúng |
| FDK-01 | encoder | silence | AAC hợp lệ |
| FDK-02 | encoder | sine | software = PL |
| FDK-03 | encoder | transient | block sequence đúng |
| FDK-04 | encoder | abc_votay.wav | encode/decode PASS |
| SYS-01 | system | nhiều file liên tiếp | clear_history đúng |
| SYS-02 | system | tail không đủ frame | flush đúng |
| PERF-01 | system | 48 kHz realtime | không miss deadline |

---

## 14. Rủi ro và biện pháp giảm thiểu

### 14.1 Target FPGA sai

Rủi ro:

- script hiện tại dùng Artix-7;
- báo cáo cũ không đại diện cho PYNQ-Z2.

Giảm thiểu:

- tạo flow XC7Z020 độc lập;
- nghiệm thu bằng post-route report.

### 14.2 Không bit-exact trên ARM

Rủi ro:

- FDK có nhánh ARM cho FFT/fixed-point;
- golden x86 có thể không đại diện hoàn toàn cho Cortex-A9.

Giảm thiểu:

- build software oracle trên Cortex-A9;
- so PL và software từng frame;
- khóa compiler flags và numerical profile.

### 14.3 Sai exponent

Rủi ro:

- spectrum có vẻ đúng nhưng mdct_exp sai;
- các khối psychoacoustic phía sau dùng sai scale.

Giảm thiểu:

- exponent là field bắt buộc;
- assertion theo block type;
- compare exponent ở mọi test.

### 14.4 Cache coherence

Rủi ro:

- DMA đọc dữ liệu cũ;
- CPU đọc output cũ;
- lỗi không ổn định và khó tái hiện.

Giảm thiểu:

- DMA-safe allocator;
- flush/invalidate đúng thứ tự;
- memory barrier;
- test pattern thay đổi mạnh giữa các frame.

### 14.5 Backpressure AXI

Rủi ro:

- mất/nhân đôi bin do spec read có latency;
- TLAST sai.

Giảm thiểu:

- skid buffer/FIFO;
- randomized backpressure testbench;
- counter input/output bắt buộc.

### 14.6 Window history lệch

Rủi ro:

- reset giữa stream;
- fallback backend;
- stereo time-multiplex sai context.

Giảm thiểu:

- mono trước;
- clear_history chỉ ở biên stream;
- không fallback giữa stream;
- frame trace block type/shape/history.

### 14.7 Không đạt timing/tài nguyên

Rủi ro:

- multiplier bung nhiều DSP/LUT;
- RAM không được infer BRAM;
- wrapper làm tăng critical path.

Giảm thiểu:

- xem report hierarchy;
- register AXI paths;
- tách control/datapath;
- giữ trace false;
- chỉ tối ưu sau khi có report thật.

### 14.8 DMA overhead lớn hơn lợi ích

Rủi ro:

- MDCT software ARM đã nhanh;
- setup DMA/cache làm tăng tổng latency;
- offload không cải thiện toàn encoder.

Giảm thiểu:

- đo software baseline;
- đo riêng từng đoạn;
- cân nhắc batching hoặc persistent DMA sau khi correctness hoàn tất;
- đánh giá mục tiêu dự án không chỉ bằng speedup mà còn bằng chứng minh
  HW/SW codesign và giảm CPU load.

### 14.9 Giấy phép

FDK-AAC có điều kiện license và thông báo liên quan bằng sáng chế AAC. Nếu dự
án được phân phối ngoài phạm vi nghiên cứu nội bộ, phải rà soát NOTICE, license
nguồn và nghĩa vụ đối với phiên bản đã sửa.

---

## 15. Definition of Done

Dự án chỉ được xem là triển khai thành công khi đáp ứng đồng thời:

### Chức năng

- [ ] Block Switching chạy trên Cortex-A9.
- [ ] MDCT chạy thật trên PL.
- [ ] FDK nhận spectrum và exponent từ PL.
- [ ] Encoder tạo AAC-LC ADTS hoàn chỉnh.
- [ ] Decoder giải mã không lỗi.

### Numerical correctness

- [ ] RTL core khớp golden 0 LSB.
- [ ] AXI wrapper không làm đổi dữ liệu.
- [ ] Board output khớp golden 0 LSB.
- [ ] Exponent đúng ở mọi block type.
- [ ] Chuỗi window history đúng.

### FPGA implementation

- [ ] Target xc7z020clg400-1.
- [ ] Post-route WNS không âm ở 100 MHz.
- [ ] Fit tài nguyên.
- [ ] Không có unconstrained critical path.
- [ ] Bit/HWH build tái lập bằng Tcl.

### Software

- [ ] Có software backend và PL backend.
- [ ] HAL xử lý DMA/cache/timeout.
- [ ] Không fallback giữa stream ngoài kiểm soát.
- [ ] Có log lỗi và frame_id.

### End-to-end

- [ ] Software và PL backend được so trên cùng ARM build.
- [ ] Corpus regression PASS.
- [ ] Startup, tail, flush và reset PASS.
- [ ] Không miss deadline realtime.
- [ ] Có báo cáo hiệu năng cuối.

### Tài liệu

- [ ] Register map được khóa.
- [ ] Hướng dẫn build Vivado.
- [ ] Hướng dẫn deploy overlay.
- [ ] Hướng dẫn build encoder.
- [ ] Hướng dẫn chạy regression.
- [ ] Lưu báo cáo timing/resource/test.

---

## 16. Những việc nên làm ngay

Thứ tự hành động ngắn hạn:

1. Build FDK-AAC software trên PYNQ-Z2 và tạo baseline AAC.
2. Tạo script synthesis mdct_core cho xc7z020clg400-1.
3. Lấy báo cáo timing/resource thật.
4. Khóa register map AXI wrapper.
5. Viết wrapper và testbench backpressure/TLAST.
6. Tạo Vivado block design với Zynq PS và AXI DMA.
7. Sinh bit/HWH và chạy golden frame trên board.
8. Chỉ sau khi hardware MDCT khớp 0 LSB mới sửa FDK backend.
9. Tích hợp encoder và so bitstream end-to-end.
10. Hoàn thành mono trước khi mở rộng stereo.

Điểm ưu tiên cao nhất hiện tại không phải sửa thuật toán Block Switching hoặc
MDCT, mà là:

- port RTL sang đúng XC7Z020;
- xây AXI/DMA wrapper;
- tạo overlay PYNQ;
- viết HAL;
- móc PL backend vào FDK;
- kiểm chứng end-to-end trên chính Cortex-A9.

---

## 17. Tài liệu liên quan trong repository

- [Kiến trúc encoder AAC-LC](../aac_encoder_architecture.md)
- [Block Switching README](block_switch/README.md)
- [Block Switching workflow](block_switch/docs/block_switch_workflow.md)
- [Block Switching reference model](block_switch/docs/block_switch_reference_model.md)
- [Input pipeline](INPUT/README.md)
- [Window/MDCT README](window_mdct/README.md)
- [MDCT core interface](window_mdct/docs/mdct_core.md)
- [MDCT workflow](window_mdct/docs/mdct_workflow.md)
- [MDCT development decisions](window_mdct/docs/phattrien.md)
- [MDCT reference model](window_mdct/docs/window_mdct_reference_model.md)
- [FFT radix-2 core](window_mdct/fft_radix2_core/docs/fft_radix2_core.md)

