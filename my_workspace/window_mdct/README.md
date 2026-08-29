# MDCT core AAC-LC bám FDK-AAC

Thư mục này chứa một MDCT forward hoàn chỉnh cho encoder AAC-LC `frameLength=1024`.
Datapath thực hiện đúng chuỗi của FDK-AAC:

```text
2048 PCM Q15
  -> window + TDAC fold
  -> DCT-IV pre-twiddle
  -> FFT phức radix-2 (512 hoặc 64 điểm)
  -> DCT-IV post-twiddle
  -> 1024 hệ số MDCT Q31 + exponent chung
```

RTL MDCT nằm tập trung trong `rtl/`; lõi FFT dùng lại trực tiếp từ
`fft_radix2_core/rtl/`. Source gốc của FDK-AAC không bị sửa.

## Trạng thái đã chốt

- Hỗ trợ đủ `LONG`, `START`, `SHORT`, `STOP` và cửa sổ `SINE`, `KBD`.
- LONG/START/STOP dùng một FFT-512; SHORT dùng tám FFT-64 thuần radix-2.
- PCM signed Q15, dữ liệu signed Q31, hệ số signed Q15.
- Truncation bằng dịch phải số học, wrap two's-complement 32 bit, không rounding
  và không saturation trong datapath biến đổi.
- Exponent output là `12` cho LONG/START/STOP và `9` cho SHORT.
- C++ và Python integer khớp nhau `0 LSB` trên 18 frame, 39 phép biến đổi con.
- RTL khớp golden `0 LSB` tại fold, pre-twiddle, từng stage FFT,
  post-twiddle và toàn bộ 1024 output.
- Reference radix-2 khớp hàm `mdct_block()` nguyên bản của FDK-AAC `0 LSB`
  trên chuỗi LONG/START/8-SHORT/STOP.
- NumPy float64 xác nhận dấu, gain và ordering; sai số RMS tương đối cực đại
  của MDCT fixed-point là `2.598566e-05` trên bộ regression hiện tại.

## Block switch nối vào đâu

`block_switch` nối vào phần điều khiển MDCT qua `block_type`, `right_shape` và
xung `start`; nó không nối thẳng vào FFT. Bộ đệm PCM phía trên phải cấp snapshot
2048 mẫu theo chỉ số `0..2047`. MDCT controller chọn hình học window/fold, chạy
một FFT-512 hoặc tám FFT-64 và giữ trạng thái cửa sổ trái giữa các frame.

## Cấu trúc cần giữ

```text
window_mdct/
├── rtl/                 RTL MDCT: control, datapath, ROM và RAM wrapper
├── fft_radix2_core/     FFT radix-2 Q31/Q15 dùng chung
├── ref/                 reference C++ và Python integer/NumPy
├── tb/                  testbench RTL, generator và harness FDK gốc
├── tools/               sinh ROM và golden vector
├── golden/radix2_q31_v1 bộ regression có version
├── synth/               constraint và Vivado batch flow
└── docs/                đặc tả, kiến trúc và hướng dẫn Linux
```

`build/`, cache Python, work library simulator và báo cáo tổng hợp là artefact
tạm, đã được `.gitignore` loại khỏi source tree.

## Tài liệu

1. [Kiến trúc và giao diện RTL](docs/mdct_core.md).
2. [Luồng thuật toán tương ứng FDK-AAC](docs/mdct_workflow.md).
3. [Chuẩn số và quyết định thiết kế cuối](docs/phattrien.md).
4. [Mô hình tham chiếu và chiến lược kiểm chứng](docs/window_mdct_reference_model.md).
5. [Chạy trên Linux bằng Questa, GHDL, C++ và NumPy](docs/huongdanchay.md).
6. [Đặc tả FFT radix-2 dùng chung](fft_radix2_core/docs/dactafftcor.md).
