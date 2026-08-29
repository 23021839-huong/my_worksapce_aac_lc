# Luồng MDCT AAC-LC tương ứng FDK-AAC

## 1. Phạm vi và source chuẩn

Thiết kế bám đường encoder AAC-LC `frameLength=1024`:

```text
FDKaacEnc_Transform_Real()
  -> mdct_block()
  -> dct_IV()
  -> dit_fft()
```

Các source FDK dùng làm chuẩn là:

| Source | Vai trò |
|---|---|
| `libAACenc/src/transform.cpp` | chọn tham số transform từ block type |
| `libFDK/src/mdct.cpp` | window, TDAC fold và state giữa frame |
| `libFDK/src/dct.cpp` | DCT-IV bằng pre/FFT/post |
| `libFDK/src/fft_rad2.cpp` | kernel FFT radix-2 DIT |
| `libFDK/src/FDK_tools_rom.cpp` | SINE/KBD và các bảng sin/cos Q15 |

Phạm vi không gồm AAC-ELD, decoder/IMDCT, block switch, psychoacoustic,
quantizer hay bitstream formatter.

## 2. MDCT không cần FFT 2048 điểm

FDK không tạo trước một vector 2048 mẫu đã nhân cửa sổ. Windowing được gộp với
TDAC fold để giảm 2048 mẫu thành 1024 scalar, sau đó DCT-IV 1024 được phân tích
thành FFT phức 512 điểm:

```text
PCM[0..2047]
  -> window + fold: 2048 -> 1024
  -> pre-rotate/pack: 1024 real -> 512 complex
  -> forward FFT-512
  -> post-rotate/unpack: 512 complex -> 1024 MDCT
```

Với short sequence, chuỗi trên chạy tám lần ở độ dài 128/64. Vì vậy core FFT
chỉ cần hai mode 64 và 512, đều là lũy thừa hai thuần radix-2.

## 3. Hình học theo block type

| Block type | Right slope | Số DCT-IV | Chiều DCT-IV | FFT | Snapshot base |
|---|---:|---:|---:|---:|---|
| LONG | 1024 | 1 | 1024 | 512 | `0` |
| START | 128 | 1 | 1024 | 512 | `0` |
| SHORT | 128 | 8 | 128 | 64 | `448 + 128*q` |
| STOP | 1024 | 1 | 1024 | 512 | `0` |

`q=0..7`. Output short sub-transform `q` được đặt tại `128*q`.

Left slope không chỉ phụ thuộc block hiện tại. Nó là right slope/shape đã lưu
từ phép biến đổi trước. Ở cold start, right slope hiện tại được cài làm left
slope. Trong một SHORT frame, state này được cập nhật sau từng short transform,
không chờ hết cả tám transform.

## 4. Window và TDAC fold

Xét một transform cục bộ dài `L`, đặt:

```text
H  = L/2
fl = chiều left slope
fr = chiều right slope
nl = (L-fl)/2
nr = (L-fr)/2
```

Với `x[]` là `2L` mẫu bắt đầu tại input base của transform, FDK tạo vector
`fold[0..L-1]` bằng bốn miền:

```text
fold[H+i]        = -(x[L-1-i] << 15)                         i=0..nl-1
fold[H+nl+i]     = x[nl+i]*wL.im - x[L-nl-i-1]*wL.re         i=0..fl/2-1
fold[H-1-i]      = -(x[L+i] << 15)                           i=0..nr-1
fold[H-nr-i-1]   = -(x[L+nr+i]*wR.re
                      + x[2L-nr-i-1]*wR.im)                  i=0..fr/2-1
```

Mỗi tích Q15×Q15 và mỗi cộng/trừ giữ đúng thứ tự của FDK. Các miền được ghi vào
địa chỉ cuối ngay khi tính, nên không cần buffer windowed 2048 mẫu riêng.
`mdct_window_fold.vhd` đọc song song hai mẫu PCM cho mỗi cặp và phát vector Q31
theo địa chỉ tự nhiên vào work RAM/pre engine.

## 5. DCT-IV bằng FFT radix-2

### 5.1 Pre-twiddle

`mdct_dct4_pre` đọc cặp ở hai đầu vector folded, dùng literal
`SineWindow1024` hoặc `SineWindow128`, thực hiện complex multiply theo đúng thứ
tự truncate của `dct_IV()` rồi pack thành 512 hoặc 64 số phức. Pre-twiddle đóng
góp hai bit vào exponent tích lũy.

### 5.2 FFT

`fft_radix2_core` nhận complex input theo natural order và chạy forward DIT:

- FFT-64: 6 stage, stage 2 không scale, exponent tăng 5;
- FFT-512: 9 stage, stage 2 không scale, exponent tăng 8;
- stage còn lại scale 1 bit theo mask `101111` hoặc `101111111`;
- twiddle mang dấu forward âm;
- hai bank RAM ping-pong giữ I/O natural order.

Không dùng radix-3, radix-4 hay mixed-radix trong profile này.

### 5.3 Post-twiddle

`mdct_dct4_post` cache toàn bộ FFT output rồi unpack/in-place rotate giống
`dct_IV()`. Bảng là `SineTable1024` với bước 2 cho `L=1024`, bước 16 cho
`L=128`; trường hợp góc `pi/4` dùng Q15 `0x5A82`. Post không tăng exponent.

## 6. Ngân sách exponent

| Ranh giới | LONG/START/STOP | SHORT |
|---|---:|---:|
| Sau window/fold | 2 | 2 |
| Sau pre-twiddle | 4 | 4 |
| Sau FFT | 12 | 9 |
| Sau post/final | 12 | 9 |

Output là cặp `(mantissa Q31, exponent)`. Không được tự dịch mantissa về một
Q-format khác rồi bỏ exponent vì các khối AAC phía sau cần cùng quy ước với
FDK-AAC.

## 7. Vai trò của BlockSwitch

BlockSwitch cung cấp metadata, còn PCM snapshot do buffer cấp:

```text
BlockSwitch -- block_type/right_shape --> mdct_control
PCM buffer  -- 2048 mẫu Q15 -----------> mdct_pcm_memory
mdct_control -- size/start -------------> fold/pre/FFT/post
```

Nó không cấp twiddle, địa chỉ butterfly hay `size_mode` trực tiếp cho FFT.
Controller MDCT là nơi dịch quyết định cấp frame thành hình học transform.

## 8. Các ranh giới phải giữ golden

Mọi thay đổi tối ưu RTL phải tiếp tục so bit-exact tại:

1. PCM snapshot đã nhận.
2. Folded Q31.
3. Complex pre-twiddle.
4. Kết quả của từng FFT stage và FFT output.
5. Scalar post-twiddle.
6. 1024 hệ số cuối và exponent.

Nếu chỉ so output cuối, lỗi ordering ở một stage có thể bị che bởi một lỗi bù
trừ ở stage sau. Bộ golden hiện tại cố ý giữ tất cả các ranh giới trên.
