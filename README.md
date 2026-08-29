# My Workspace — AAC-LC HW/SW Codesign

Thư mục này chứa tài liệu, reference model, RTL và công cụ kiểm chứng phục vụ
triển khai bộ mã hóa AAC-LC trên PYNQ-Z2. Kiến trúc mục tiêu giữ Block
Switching, Psychoacoustic Model và Quantization/Coding trên Cortex-A9; khối
Window/MDCT được tăng tốc bằng Programmable Logic.

```text
PCM → INPUT → Block Switching → Window/MDCT → Psy Model → Quantization → AAC
          PS          PS             PL           PS            PS
```

## Cấu trúc

| Thư mục | Nội dung |
|---|---|
| [`INPUT/`](INPUT/) | Đọc WAV PCM16 và tạo input cho Block Switching/MDCT |
| [`block_switch/`](block_switch/) | Reference model và regression Block Switching |
| [`window_mdct/`](window_mdct/) | Reference model, golden vector và RTL MDCT/FFT |
| [`psychoacoustic_model/`](psychoacoustic_model/) | Luồng Psy, masking, TNS/PNS và giao diện với QC |
| [`quantization_coding/`](quantization_coding/) | Phân bổ bit, scalefactor, quantization và Huffman coding |
| [`pynq_z2/`](pynq_z2/) | Script, tài liệu build và profiling trên PYNQ-Z2 |

## Trạng thái hiện tại

- Block Switching PASS bit-exact trên 292 frame/25 kịch bản.
- FFT-64/FFT-512 và MDCT RTL khớp golden `0 LSB`.
- FDK-AAC đã encode thành công trên Cortex-A9; đã có kết quả profiling theo khối.
- Đã có flow Vivado nhắm `xc7z020clg400-1` ở 100 MHz.
- Chưa hoàn tất AXI/DMA wrapper, overlay, HAL và PL backend trong FDK-AAC.

## Tài liệu chính

1. [`z2.md`](z2.md) — kiến trúc và lộ trình HW/SW codesign.
2. [`PYNQ_Z2_DEPLOYMENT_PLAN.md`](PYNQ_Z2_DEPLOYMENT_PLAN.md) — kế hoạch triển khai và tiêu chí nghiệm thu.
3. README của từng module — cách build, chạy và kiểm chứng chi tiết.

Mục tiêu nghiệm thu cuối là MDCT chạy trên PL, khớp software golden `0 LSB`,
đạt timing 100 MHz và tạo được file AAC-LC hợp lệ từ encoder chạy trên PYNQ-Z2.
