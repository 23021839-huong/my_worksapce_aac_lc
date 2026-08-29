# Chạy mô phỏng và verify FFT core trên SSH Linux

Tài liệu này hướng dẫn chạy `fft_radix2_core` bằng Questa/ModelSim ở chế độ
batch trên máy Linux qua SSH, sau đó verify bit-exact giữa RTL, C++, Python
integer, NumPy và thuật toán FDK.

Toàn bộ lệnh phải chạy từ thư mục gốc của core, vì VHDL testbench mở vector bằng
đường dẫn tương đối `vectors/...`:

```bash
cd /duong-dan-toi-fdk-aac/my_workspace/window_mdct/fft_radix2_core
pwd
```

Kết quả `pwd` phải kết thúc bằng:

```text
my_workspace/window_mdct/fft_radix2_core
```

## Cấu trúc sau khi làm sạch

Chỉ giữ một flow Linux và một nguồn tạo golden duy nhất:

```text
fft_radix2_core/
├── rtl/       source VHDL có thể tổng hợp
├── tb/        testbench VHDL-2008
├── ref/       C++/Python model và comparator
├── tools/     sinh twiddle, sinh golden, so tương đương FDK
├── vectors/   bộ golden chuẩn bắt buộc cho testbench
├── synth/     XDC và Vivado runtime flow
└── docs/      đặc tả, kiến trúc và hướng dẫn Linux
```

Các file chuẩn cần giữ trong `vectors/` là:

```text
input_fft.txt
output_fft.txt
output_fft_stage.txt
manifest.csv
contract.meta
```

Các file `output_fft_cpp*`, `output_fft_rtl.txt`, `output_fft_numpy*` và
`compare_*.csv` chỉ là kết quả của một lần chạy. Chúng được `.gitignore` bỏ qua
và có thể tái tạo bằng các bước bên dưới. `build/`, Questa `work`, WLF, log,
binary và Python cache cũng không phải source.

`gen_fft_vectors.py` là công cụ duy nhất được phép tạo bộ golden. C++ reference
chỉ đọc `input_fft.txt` và sinh output mang hậu tố `_cpp`; điều này ngăn C++ vô
tình ghi đè `output_fft.txt` hoặc `output_fft_stage.txt`.

## 1. Chuẩn bị môi trường SSH Linux

### 1.1. Công cụ bắt buộc

```bash
command -v vlib
command -v vmap
command -v vcom
command -v vsim
command -v g++
command -v python3

vsim -version
g++ --version
python3 --version
```

Nếu Questa chưa có trong `PATH`, source script môi trường do quản trị viên máy
chủ cung cấp. Ví dụ minh họa, đường dẫn thực tế tùy server:

```bash
source /opt/mentor/questa/questasim/questa_env.sh
```

License Questa thường được server cấu hình sẵn qua `LM_LICENSE_FILE` hoặc
`MGLS_LICENSE_FILE`. Chỉ kiểm tra biến có tồn tại, không ghi giá trị license vào
repository hay log công khai:

```bash
test -n "${LM_LICENSE_FILE:-}${MGLS_LICENSE_FILE:-}" \
  && echo "Questa license environment: configured" \
  || echo "Questa license environment: check with server administrator"
```

Mô phỏng batch không cần `ssh -X`/`ssh -Y`. Chỉ cần X forwarding khi muốn mở
GUI hoặc waveform trực tiếp trên server.

### 1.2. Python và NumPy

Các bước sinh vector và so bit-exact chỉ cần Python chuẩn. NumPy chỉ cần cho
phép đối chiếu float64 độc lập ở bước 7.2.

Kiểm tra NumPy:

```bash
python3 -c 'import numpy; print(numpy.__version__)'
```

Nếu server cho phép tạo virtual environment và cài từ package mirror:

```bash
python3 -m venv build/venv
source build/venv/bin/activate
python -m pip install --upgrade pip
python -m pip install numpy
```

Không cần cài NumPy nếu chỉ chạy cổng bit-exact RTL–C++–Python.

## 2. Quy tắc PASS của core

Profile kiểm chứng duy nhất là `FDK_AACLC_1024_Q31Q15_RAD2_V1`:

| Thuộc tính | Giá trị |
|---|---|
| Dữ liệu | signed Q1.31, 32 bit |
| Twiddle | signed Q1.15, 16 bit |
| FFT | forward radix-2 DIT, N=64 hoặc 512 |
| `scale_exp` | 5 cho FFT-64; 8 cho FFT-512 |
| Golden integer | phải khớp đúng 0 LSB |

Một lần chạy chỉ được xem là PASS khi đồng thời:

1. `vcom` và `vsim` trả exit code 0.
2. Log Questa có dòng `PASS: Q31 FFT64/FFT512 matched final and every stage trace`.
3. Không có `** Error:` hoặc `** Fatal:` trong log.
4. `compare_fft_outputs.py` báo 2.560 record, 0 mismatch, max error 0 LSB.
5. Testbench đã kiểm đủ 10.752 output butterfly theo từng stage.
6. Latency đúng 222 clock cho FFT-64 và 2.349 clock cho FFT-512.

NumPy float64 không phải cổng bit-exact; sai số vài nghìn LSB do Q15 và
truncation theo stage là bình thường.

## 3. Sinh lại twiddle và golden vector

Chạy từ thư mục core:

```bash
set -euo pipefail

python3 tools/gen_fdk_twiddle.py
python3 tools/gen_fft_vectors.py
```

`gen_fdk_twiddle.py` đọc `libFDK/src/FDK_tools_rom.cpp` và đồng bộ ba bảng:

- `rtl/fft_radix2_twiddle_rom.vhd`;
- `ref/fdk_sinetable512_q15.h`;
- `ref/fdk_sinetable512_q15.py`.

Hash mong đợi:

```text
95eb626e5d5a6f44fdbd00f5700bb5f8b1b8e9425d20d374412968090d595ad9
```

Bộ regression gồm 12 frame, tổng cộng 2.560 bin final và 10.752 hàng trace:

| File | Nội dung |
|---|---|
| `vectors/input_fft.txt` | input complex Q1.31 |
| `vectors/output_fft.txt` | golden integer final |
| `vectors/output_fft_stage.txt` | golden sau từng butterfly/stage |
| `vectors/manifest.csv` | case, mode, N, exponent và hash |
| `vectors/contract.meta` | hợp đồng số học máy đọc được |

Kiểm tra nhanh metadata trước khi mô phỏng:

```bash
cat vectors/contract.meta
head -n 5 vectors/manifest.csv
wc -l vectors/input_fft.txt vectors/output_fft.txt vectors/output_fft_stage.txt
```

Số dòng mong đợi lần lượt là 2.560, 2.560 và 10.752.

## 4. Build và verify mô hình C++ trước RTL

Tạo executable trong `build/`, không ghi binary vào `ref/`:

```bash
mkdir -p build

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  ref/fft_radix2_ref.cpp \
  -o build/fft_radix2_ref

./build/fft_radix2_ref \
  --input vectors/input_fft.txt \
  --output vectors/output_fft_cpp.txt \
  --trace vectors/output_fft_cpp_stage.txt
```

So final output C++ với golden Python integer:

```bash
python3 ref/compare_fft_outputs.py \
  --reference vectors/output_fft.txt \
  --rtl vectors/output_fft_cpp.txt \
  --report vectors/compare_cpp_python.csv \
  --data-w 32
```

Kết quả bắt buộc:

```text
Compared records : 2560
Mismatched bins  : 0
Max abs error RE : 0 LSB
Max abs error IM : 0 LSB
```

So trace theo từng stage, bỏ qua dòng trống/comment nếu có:

```bash
grep -Ev '^[[:space:]]*(#|$)' vectors/output_fft_stage.txt \
  > build/python_stage.normalized.txt
grep -Ev '^[[:space:]]*(#|$)' vectors/output_fft_cpp_stage.txt \
  > build/cpp_stage.normalized.txt

diff -u build/python_stage.normalized.txt build/cpp_stage.normalized.txt
```

`diff` không in gì và trả exit code 0 là PASS.

## 5. Tạo thư viện Questa riêng trong `build/`

Không tạo thư viện `work` ở root repository. Đoạn dưới tạo `modelsim.ini` và
physical library trong `build/questa`:

```bash
set -euo pipefail

mkdir -p build/questa
QUESTA_LIB="$(pwd)/build/questa/work"

(
  cd build/questa
  test -d work || vlib work
  vmap -c
  vmap work "$QUESTA_LIB"
)

export MODELSIM="$(pwd)/build/questa/modelsim.ini"
echo "MODELSIM=$MODELSIM"
vmap
```

`vmap` phải cho thấy logical library `work` trỏ tới
`.../fft_radix2_core/build/questa/work`.

Nếu muốn compile sạch mà không dùng `rm -rf`:

```bash
export MODELSIM="$(pwd)/build/questa/modelsim.ini"
vdel -all -lib work || true
vlib "$(pwd)/build/questa/work"
vmap work "$(pwd)/build/questa/work"
```

## 6. Compile VHDL-2008 bằng `vcom`

Thứ tự compile là bắt buộc: package trước, rồi ROM/address/memory/datapath/
control/top và cuối cùng là testbench.

```bash
set -euo pipefail
export MODELSIM="$(pwd)/build/questa/modelsim.ini"

vcom -2008 -work work \
  rtl/fft_radix2_pkg.vhd \
  rtl/fft_radix2_twiddle_rom.vhd \
  rtl/fft_radix2_addr_gen.vhd \
  rtl/fft_radix2_memory.vhd \
  rtl/fft_radix2_butterfly.vhd \
  rtl/fft_radix2_control.vhd \
  rtl/fft_radix2_core.vhd \
  tb/tb_fft_radix2_core.vhd \
  2>&1 | tee build/questa/vcom.log
```

Vì đã bật `set -o pipefail`, lỗi `vcom` không bị `tee` che mất. Kiểm tra nhanh:

```bash
test "${PIPESTATUS[0]}" -eq 0
! grep -E '\*\* (Error|Fatal):' build/questa/vcom.log
```

Không dùng `vlog` vì toàn bộ DUT/testbench là VHDL. Không thêm `-93` hoặc
`-2002`; thiết kế dùng VHDL-2008 và `std.env.stop`.

## 7. Chạy Questa batch qua SSH

### 7.1. Regression RTL bit-exact

```bash
set -o pipefail
export MODELSIM="$(pwd)/build/questa/modelsim.ini"

vsim -c work.tb_fft_radix2_core \
  -do 'run -all; quit -f' \
  2>&1 | tee build/questa/vsim.log

VSIM_RC=${PIPESTATUS[0]}
test "$VSIM_RC" -eq 0
grep -F 'PASS: Q31 FFT64/FFT512 matched final and every stage trace' \
  build/questa/vsim.log
! grep -E '\*\* (Error|Fatal):' build/questa/vsim.log
```

Testbench tự thực hiện các kiểm tra sau:

- nạp xen kẽ FFT-512 và FFT-64 qua nhiều frame;
- kiểm mọi output butterfly với `output_fft_stage.txt`;
- kiểm 2.560 bin final với `output_fft.txt`;
- kiểm `scale_exp=5/8`, `done`, `busy`, `rd_valid` và auto re-arm;
- kiểm latency 222/2.349 clock;
- ghi dữ liệu RTL thật vào `vectors/output_fft_rtl.txt`.

Sau Questa, chạy comparator độc lập:

```bash
python3 ref/compare_fft_outputs.py \
  --reference vectors/output_fft.txt \
  --rtl vectors/output_fft_rtl.txt \
  --report vectors/compare_fft_outputs.csv \
  --data-w 32
```

Exit code của comparator khác 0 nếu có mismatch.

### 7.2. Verify bằng Python integer và NumPy float64

Nếu NumPy đã có:

```bash
python3 ref/compare_fft_numpy.py \
  --input vectors/input_fft.txt \
  --rtl vectors/output_fft_rtl.txt \
  --integer-output vectors/output_fft_numpy.txt \
  --stage-output vectors/output_fft_numpy_stage.txt \
  --float-output vectors/output_fft_numpy.csv \
  --report vectors/compare_fft_numpy.csv
```

Kết quả chuẩn của bộ vector hiện tại:

```text
Frames                 : 12
Integer mismatched bins: 0
Integer max error      : 0 LSB
Float RMS error        : khoảng 440.53 LSB
Float max error        : khoảng 5512.61 LSB
Float SNR              : khoảng 97.30 dB
```

Chỉ hai dòng integer là điều kiện PASS. Thông số float có thể thay đổi nếu bộ
input thay đổi.

### 7.3. Verify radix-2 thuần với thuật toán FDK

```bash
python3 tools/verify_fdk_equivalence.py --trials 64
```

Kết quả mong đợi:

```text
PASS: pure radix-2 equals direct FDK dit_fft port (36864 bins)
```

Kiểm tra này không dùng RTL; nó chứng minh vòng radix-2 của model tương đương
statement-by-statement với kernel radix-4/radix-2 trong FDK trên cùng số học.

## 8. Lệnh chạy đầy đủ để copy/paste

Đoạn dưới giả sử đã `cd` đúng thư mục core và Questa đã có trong `PATH`:

```bash
set -euo pipefail

python3 tools/gen_fdk_twiddle.py
python3 tools/gen_fft_vectors.py

mkdir -p build/questa
QUESTA_LIB="$(pwd)/build/questa/work"
(
  cd build/questa
  test -d work || vlib work
  vmap -c
  vmap work "$QUESTA_LIB"
)
export MODELSIM="$(pwd)/build/questa/modelsim.ini"

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  ref/fft_radix2_ref.cpp -o build/fft_radix2_ref
./build/fft_radix2_ref \
  --input vectors/input_fft.txt \
  --output vectors/output_fft_cpp.txt \
  --trace vectors/output_fft_cpp_stage.txt
python3 ref/compare_fft_outputs.py \
  --reference vectors/output_fft.txt \
  --rtl vectors/output_fft_cpp.txt \
  --report vectors/compare_cpp_python.csv

vcom -2008 -work work \
  rtl/fft_radix2_pkg.vhd \
  rtl/fft_radix2_twiddle_rom.vhd \
  rtl/fft_radix2_addr_gen.vhd \
  rtl/fft_radix2_memory.vhd \
  rtl/fft_radix2_butterfly.vhd \
  rtl/fft_radix2_control.vhd \
  rtl/fft_radix2_core.vhd \
  tb/tb_fft_radix2_core.vhd \
  2>&1 | tee build/questa/vcom.log

vsim -c work.tb_fft_radix2_core \
  -do 'run -all; quit -f' \
  2>&1 | tee build/questa/vsim.log

grep -F 'PASS: Q31 FFT64/FFT512 matched final and every stage trace' \
  build/questa/vsim.log
! grep -E '\*\* (Error|Fatal):' build/questa/vcom.log build/questa/vsim.log

python3 ref/compare_fft_outputs.py
python3 tools/verify_fdk_equivalence.py --trials 64

echo 'PASS: Questa + RTL/C++/Python + FDK verification completed'
```

Muốn thêm NumPy vào flow đầy đủ, chạy sau đoạn trên:

```bash
python3 ref/compare_fft_numpy.py
```

## 9. Ghi waveform trên server

Để giữ waveform mà không mở GUI:

```bash
export MODELSIM="$(pwd)/build/questa/modelsim.ini"

vsim -c -wlf build/questa/fft_radix2_core.wlf \
  work.tb_fft_radix2_core \
  -do 'log -r /*; run -all; quit -f' \
  2>&1 | tee build/questa/vsim_wave.log
```

Sau đó có thể tải WLF về máy local:

```bash
scp user@server:/duong-dan/fft_radix2_core/build/questa/fft_radix2_core.wlf .
```

Mở trên máy có Questa GUI:

```bash
vsim -view fft_radix2_core.wlf
```

Nếu server cho phép X forwarding, có thể dùng `ssh -Y` rồi mở `vsim`, nhưng WLF
batch thường ổn định và nhanh hơn trên kết nối mạng chậm.

## 10. Chạy lâu khi SSH có thể mất kết nối

Regression hiện chỉ mất vài giây, nhưng khi ghép MDCT hoặc chạy nhiều seed nên
dùng `tmux`:

```bash
tmux new -s fft_verify
# Chạy các lệnh trong session.
# Nhấn Ctrl-b rồi d để detach.
tmux attach -t fft_verify
```

Log đã được giữ tại `build/questa/vcom.log` và `build/questa/vsim.log`, nên có
thể kiểm lại mà không chạy lại mô phỏng.

## 11. Lỗi thường gặp

| Hiện tượng | Nguyên nhân thường gặp | Cách xử lý |
|---|---|---|
| `vsim: command not found` | Chưa source môi trường Questa | Source script của server rồi chạy `vsim -version` |
| `Unable to checkout a license` | License server/biến môi trường sai | Liên hệ quản trị viên; không sửa source RTL |
| `Library work not found` | `MODELSIM` hoặc `vmap` sai | Chạy lại mục 5 và kiểm output `vmap` |
| Package chưa được khai báo | Compile sai thứ tự | Package phải là file đầu tiên trong lệnh `vcom` |
| Không mở được `vectors/input_fft.txt` | Chạy `vsim` sai thư mục | `cd` về root `fft_radix2_core` trước khi chạy |
| `std.env` không tồn tại | Compile không dùng VHDL-2008 | Thêm `vcom -2008` |
| Log có PASS nhưng shell trả 0 khi trước đó có lỗi | `tee` che exit code | Dùng `set -o pipefail` như tài liệu |
| RTL final lệch nhưng stage đầu đúng | Sai thứ tự/đọc result | Kiểm bit-reverse load, bank cuối và `rd_valid` |
| Lệch từ stage 1 | Sai statement ordering | Phải dùng `top=(A+B)>>1`, `bottom=top-B` |
| Chỉ lệch phase 0 hoặc 128 | Mất bypass đặc biệt | Kiểm nhánh exact `W=1` và `W=-j` |
| Integer 0 LSB nhưng NumPy lệch | Lượng tử Q15 và truncation | Đây không phải lỗi nếu integer vẫn khớp 0 LSB |

## 12. GHDL và tổng hợp

GHDL có thể dùng làm simulator thứ hai trên Linux:

```bash
mkdir -p build/ghdl

ghdl -a --std=08 --workdir=build/ghdl \
  rtl/fft_radix2_pkg.vhd \
  rtl/fft_radix2_twiddle_rom.vhd \
  rtl/fft_radix2_addr_gen.vhd \
  rtl/fft_radix2_memory.vhd \
  rtl/fft_radix2_butterfly.vhd \
  rtl/fft_radix2_control.vhd \
  rtl/fft_radix2_core.vhd \
  tb/tb_fft_radix2_core.vhd

ghdl -e --std=08 --workdir=build/ghdl tb_fft_radix2_core
ghdl -r --std=08 --workdir=build/ghdl \
  tb_fft_radix2_core --assert-level=error
```

Kiểm tra synthesis-elaboration:

```bash
ghdl --synth --std=08 --workdir=build/ghdl fft_radix2_core \
  > build/fft_radix2_core_synth.vhd
```

GHDL 6.0 nhận hai RAM 512×64 bit. Số LUT/DSP/BRAM/Fmax thực phải lấy từ
Vivado post-route:

```bash
vivado -mode batch -source synth/vivado_fft_runtime.tcl
```

Các báo cáo được tạo trong `synth/`; chỉ dùng timing và utilization post-route
để kết luận phần cứng.
