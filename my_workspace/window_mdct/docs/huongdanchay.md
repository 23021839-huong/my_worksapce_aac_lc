# Chạy và kiểm chứng MDCT core

Chạy các lệnh dưới đây trên Linux, từ thư mục gốc của repository `fdk-aac`.
Cần có Python 3, NumPy và Questa (`vlib`, `vcom`, `vsim`) trong `PATH`.

## 1. Sinh vector kiểm thử

```bash
python3 my_workspace/window_mdct/tools/gen_mdct_vectors.py \
  --out my_workspace/window_mdct/build/vectors \
  --force
```

Script này đã tự chạy self-test của Python reference model. Kết quả hợp lệ phải có:

```text
SELF-TEST PASS
PASS: generated mdct-r2-fdkq15-v1 vectors
```

## 2. Compile RTL

Chỉ cần tạo thư viện `work` ở lần chạy đầu tiên:

```bash
vlib work
```

Compile source VHDL-2008 theo đúng thứ tự phụ thuộc:

```bash
vcom -2008 \
  my_workspace/window_mdct/fft_radix2_core/rtl/fft_radix2_pkg.vhd \
  my_workspace/window_mdct/rtl/mdct_pkg.vhd \
  my_workspace/window_mdct/fft_radix2_core/rtl/fft_radix2_twiddle_rom.vhd \
  my_workspace/window_mdct/fft_radix2_core/rtl/fft_radix2_addr_gen.vhd \
  my_workspace/window_mdct/fft_radix2_core/rtl/fft_radix2_memory.vhd \
  my_workspace/window_mdct/fft_radix2_core/rtl/fft_radix2_butterfly.vhd \
  my_workspace/window_mdct/fft_radix2_core/rtl/fft_radix2_control.vhd \
  my_workspace/window_mdct/fft_radix2_core/rtl/fft_radix2_core.vhd \
  my_workspace/window_mdct/rtl/mdct_window_rom.vhd \
  my_workspace/window_mdct/rtl/mdct_rotation_rom.vhd \
  my_workspace/window_mdct/rtl/mdct_pcm_memory.vhd \
  my_workspace/window_mdct/rtl/mdct_work_memory.vhd \
  my_workspace/window_mdct/rtl/mdct_fft_cache_memory.vhd \
  my_workspace/window_mdct/rtl/mdct_spectrum_memory.vhd \
  my_workspace/window_mdct/rtl/mdct_window_fold.vhd \
  my_workspace/window_mdct/rtl/mdct_dct4_pre.vhd \
  my_workspace/window_mdct/rtl/mdct_dct4_post.vhd \
  my_workspace/window_mdct/rtl/mdct_control.vhd \
  my_workspace/window_mdct/rtl/mdct_core.vhd \
  my_workspace/window_mdct/tb/tb_mdct_core.vhd
```

## 3. Chạy mô phỏng

```bash
vsim -c \
  -gGOLDEN_DIR="my_workspace/window_mdct/build/vectors" \
  -gRTL_OUT_FILE="my_workspace/window_mdct/build/vectors/rtl_mdct_out_q31.txt" \
  work.tb_mdct_core \
  -do 'onerror {quit -code 1}; run -all; quit -f'
```

Testbench tự dừng với lỗi nếu kết quả không khớp. Khi thành công, Questa in ra:

```text
PASS: mdct_core matches fold/pre/every FFT stage/post/final at 0 LSB
```

## 4. So toàn bộ input/output RTL với NumPy

Testbench ghi toàn bộ output RTL vào `build/vectors/rtl_mdct_out_q31.txt`. Sau
khi mô phỏng PASS, chạy:

```bash
python3 my_workspace/window_mdct/tools/compare_rtl_numpy.py
```

Kết quả nằm trong `my_workspace/window_mdct/build/rtl_numpy_compare/`:

- `all_inputs.csv`: toàn bộ 2048 input PCM của từng frame và sai số input
  (bằng 0 vì RTL và NumPy dùng chung vector);
- `all_output_errors.csv`: đủ 1024 output mỗi frame, gồm RTL, NumPy và sai số;
- `all_frame_errors.csv`: RMS, relative RMS, max error và SNR của từng frame.
