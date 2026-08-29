# Mô hình tham chiếu và kiểm chứng MDCT

## 1. Mục tiêu

Bộ reference chứng minh ba thuộc tính khác nhau:

1. VHDL, C++ và Python integer thực hiện cùng hợp đồng fixed-point, yêu cầu
   `0 LSB` ở mọi ranh giới.
2. Hợp đồng pure radix-2 cho kết quả giống `mdct_block()`/`dct_IV()` nguyên bản
   của FDK-AAC, yêu cầu `0 LSB` end-to-end.
3. NumPy float64 độc lập xác nhận chiều FFT, gain, ordering và sai số lượng tử.

Không dùng chính RTL để sinh expected output và không dùng NumPy làm oracle
bit-exact.

## 2. Các hiện thực độc lập

| File | Vai trò |
|---|---|
| `ref/fixed_mdct_radix2.h/.cpp` | statement model C++ Q15/Q31, không gọi transform FDK |
| `ref/mdct_radix2_model.py` | model integer Python, parser ROM literal và NumPy oracle |
| `tools/gen_mdct_vectors.py` | self-test, sinh golden versioned và checksum |
| `tb/gen_mdct_golden.cpp` | sinh vector C++ hoặc kiểm trực tiếp bộ golden Python |
| `tb/verify_mdct_fdk.cpp` | so pure-radix2 reference với `mdct_block()` gốc |
| `tb/tb_mdct_core.vhd` | regression self-checking toàn RTL |

C++ fixed model chỉ dùng literal ROM từ `FDK_tools_rom.cpp`; nó tự viết lại
fold, pre, radix-2 FFT và post. Harness FDK là executable khác, link trực tiếp
`mdct.cpp`, `dct.cpp` và `fft_rad2.cpp`, nhờ vậy phép so không tự tham chiếu.

## 3. Bộ golden có version

`golden/radix2_q31_v1/` là dữ liệu nguồn kiểm chứng, không phải artefact build:

| File | Nội dung |
|---|---|
| `contract.meta` | profile, width, scale mask, exponent, table stride và seed |
| `cases.csv` | ba nhóm tín hiệu kiểm thử |
| `manifest.csv` | frame, block, shape, sub-transform và offset |
| `pcm_in_s16.txt` | snapshot PCM signed16 |
| `fold_q31.txt` | output window/fold |
| `pre_q31.txt` | complex input FFT |
| `fft_stage_q31.txt` | cặp output của mọi butterfly ở mọi stage |
| `post_q31.txt` | output post-twiddle |
| `mdct_out_q31.txt` | 1024 hệ số cuối |
| `rom_window_q15.txt` | literal SINE/KBD dùng bởi fold |
| `rom_dct_q15.txt` | literal pre/post DCT-IV |
| `rom_fft_q15.txt` | literal FFT twiddle |
| `numpy_metrics.csv` | sai số so với oracle float64 theo sub-transform |
| `checksums.sha256` | fingerprint các file trong contract |

Generator chỉ ghi đè danh sách file đã biết khi có `--force`; nó không xoá tùy
ý nội dung khác trong thư mục output.

## 4. Phạm vi stimulus

Regression chuẩn có ba case, mỗi case sáu frame theo chuỗi:

```text
LONG -> LONG -> START -> SHORT -> STOP -> LONG
```

Shape SINE/KBD thay đổi có chủ ý để kiểm state trái/phải. PCM gồm zero/impulse,
tín hiệu định hướng và pseudo-random full-range với các giá trị `INT16_MIN`/
`INT16_MAX` ở biên vùng short. Tổng cộng:

- 18 frame;
- 39 sub-transform, do mỗi SHORT frame tạo tám transform;
- 18,432 hệ số MDCT cuối;
- 87,552 record FFT-stage trong golden hiện tại.

## 5. Tiêu chí PASS

### 5.1 Python self-test và NumPy

Python so FFT radix-2 tách stage với kernel FDK fused, rồi so fixed-point với
NumPy float64. Kết quả bộ chuẩn hiện tại:

```text
split pure-radix2 == fused FDK FFT: 0 LSB
max FFT relative RMS : 1.999848e-05
max MDCT relative RMS: 2.598566e-05
```

NumPy không cần khớp từng LSB vì nó không mô phỏng truncation sau từng phép
nhân. Nó phải xác nhận đúng dấu/ordering/gain và sai số RMS nhỏ.

### 5.2 C++ so Python

`gen_mdct_golden --verify-python` đọc trực tiếp golden và tính lại bằng C++:

```text
18 frames, 39 transforms, 92,160 scalar checks, 0 LSB
```

Các scalar checks gồm nhiều boundary, không chỉ output cuối.

### 5.3 RTL so golden

`tb_mdct_core` bật `ENABLE_TRACE=true` và kiểm:

- fold;
- pre complex;
- từng cặp butterfly của từng FFT stage;
- FFT natural-order output;
- post;
- spectrum cuối và exponent.

Mọi so sánh fixed-point yêu cầu tuyệt đối `0 LSB`.

### 5.4 Reference so FDK gốc

`verify_mdct_fdk` link source transform FDK nguyên bản, chỉ thu hẹp dispatcher
FFT còn hai kích thước 64/512 và gọi chính `dit_fft()`. Kết quả chuẩn:

```text
6144 bins, LONG/START/8SHORT/STOP, exponents 12/9, 0 LSB
```

## 6. ROM và khả năng tái lập

Python parser và C++ reference cùng kiểm profile có window table 16 bit. ROM
VHDL được tạo từ literal trong `libFDK/src/FDK_tools_rom.cpp`, không tính lại
bằng `sin()` trong lúc mô phỏng. `contract.meta`, ROM dump và SHA-256 giúp phát
hiện source FDK/table thay đổi.

Lệnh đầy đủ để tự chạy từng tầng nằm tại [huongdanchay.md](huongdanchay.md).
