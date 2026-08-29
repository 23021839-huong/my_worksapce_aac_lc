# Đặc tả chính thức FFT core

Tài liệu này là hợp đồng dùng chung cho RTL, C++ và Python/NumPy. Nếu một thay
đổi làm khác bất kỳ dòng nào dưới đây thì phải đổi profile ID và tạo lại toàn
bộ golden vector.

## 1. Profile và phạm vi

- ID: `FDK_AACLC_1024_Q31Q15_RAD2_V1`.
- FDK snapshot đối chiếu: commit
  `35f9c13cb6df0c5d4e7ba958ef2d251c48b8d1d9`.
- Chỉ FFT complex forward radix-2 DIT, N bằng 64 hoặc 512.
- Mục đích: lõi FFT giữa pre-twiddle và post-twiddle của DCT-IV AAC-LC.
- Không bao gồm window/fold, pre/post-twiddle, block switching hoặc IMDCT.

## 2. Biểu diễn số

| Đại lượng | Kiểu |
|---|---|
| mẫu complex | hai số signed 32 bit Q1.31 |
| magnitude twiddle | signed 16 bit Q1.15, luôn không âm |
| phase | chỉ số `k=0..255` trên lưới FFT-512 |
| output exponent | unsigned 4 bit, giá trị 5 hoặc 8 |

Giá trị thực của một output là:

```text
complex_value = (Re + j·Im) × 2^scale_exp / 2^31
```

Mọi cộng/trừ vượt 32 bit lấy đúng 32 bit thấp rồi diễn giải two's-complement.
Không saturation. Dịch phải là arithmetic shift. Không cộng bias làm tròn.

Với một tích dữ liệu Q31 và hệ số Q15:

```text
mulDiv2(x, c) = wrap32((signed_32(x) × signed_16(c)) >> 16)
```

Dấu của cos/sin được áp dụng sau khi từng tích dương đã bị truncation. Thứ tự
này không được đổi vì có thể lệch LSB so với FDK.

## 3. Twiddle

FFT dùng:

```text
W512(k) = exp(-j·2πk/512),  k=0..255
```

FFT-64 dùng cùng ROM với stride phase lớn hơn. ROM chỉ chứa 65 cặp magnitude
trong octant đầu, rồi fold để sinh toàn miền cần thiết. Nguồn là
`SineTable512` trong `libFDK/src/FDK_tools_rom.cpp`, chuyển Q31 sang Q15 đúng
macro `FX_DBL2FXCONST_SGL`.

SHA-256 của 65 cặp `(cos_q15,sin_q15)` little-endian signed-16 là:

```text
95eb626e5d5a6f44fdbd00f5700bb5f8b1b8e9425d20d374412968090d595ad9
```

`W=1` và `W=-j` bắt buộc đi qua nhánh exact. Không được thay `1` bằng
`0x7FFF`, và tại `W=-j` phải dịch phải trước rồi mới đổi dấu.

## 4. Thuật toán radix-2 và scaling

Input natural được ghi bit-reversed. Với stage `s=1..log2(N)`:

```text
m       = 2^s
half    = m/2
tw_step = 2^(9-s)
addr_a  = group_base + j
addr_b  = addr_a + half
phase   = j × tw_step
```

Mỗi stage có N/2 butterfly. Output cuối ở natural order.

### Stage 1 — có scale

Để giữ đúng statement ordering của nửa đầu kernel radix-4 FDK:

```text
top = wrap32((A + B) >> 1)
bot = wrap32(top - B)
```

Không thay `bot` bằng `(A-B)>>1`; hai công thức toán học tương đương nhưng có
thể khác một LSB trong số học fixed-point.

### Stage 2 — không scale

Twiddle chỉ là `W=1` hoặc `W=-j`:

```text
top = wrap32(A + B×W)
bot = wrap32(A - B×W)
```

### Stage 3 trở đi — có scale

```text
U   = A >> 1
T   = complexMulDiv2(B, W)
top = wrap32(U + T)
bot = wrap32(U - T)
```

Scale masks và exponent:

| N | stages | mask từ stage 1 | `scale_exp` |
|---:|---:|---|---:|
| 64 | 6 | `101111` | 5 |
| 512 | 9 | `101111111` | 8 |

## 5. Hợp đồng giao tiếp RTL

### Load

- `load_ready=1` mới được phép phát `ld_en`.
- `ld_idx` phải liên tục từ 0 đến N−1, không lặp và không bỏ chỉ số.
- `size_mode` không được đổi trong frame.
- `start` không được trùng `ld_en` và chỉ hợp lệ sau đúng N mẫu.
- Core tự bit-reverse địa chỉ; nguồn không được bit-reverse trước.

### Run

- `busy=1` từ sau `start` đến khi ghi butterfly cuối.
- Không load, start hoặc đọc output khi `busy=1`.
- `done=1` đúng một clock và không chồng `busy`.
- Kết quả vẫn giữ trong result bank cho đến khi bắt đầu nạp frame sau.

### Read

- `rd_idx=0..N-1` là bin natural order.
- Port đọc đồng bộ: `rd_valid` trễ đúng một clock so với `rd_en`.
- `scale_exp` đi cùng toàn frame, không đi riêng từng bin.

### Trace

`ENABLE_TRACE=true` chỉ dùng khi mô phỏng. Mỗi `trace_valid` ghi hai output của
một butterfly sau P2 cùng stage và địa chỉ. Synthesis dùng `false` để loại toàn
bộ debug cone.

## 6. Định dạng vector

Input, mỗi hàng:

```text
mode frame index re_hex32 im_hex32
```

Output, mỗi hàng:

```text
mode frame index re_hex32 im_hex32 scale_exp
```

Stage trace, mỗi hàng:

```text
mode frame stage pair addr_a addr_b a_re a_im b_re b_im
```

`mode=0` là FFT-64, `mode=1` là FFT-512. Hex là bit pattern 32 bit, không có
tiền tố `0x`.

## 7. Tiêu chí chấp nhận

Một bản sửa chỉ được coi là hợp lệ khi đồng thời đạt:

1. C++ và Python integer khớp 0 LSB ở final output và mọi stage.
2. RTL khớp golden 0 LSB ở final output và mọi stage.
3. FFT-64/512 xen kẽ liên tục, `done`, `rd_valid`, latency và auto re-arm đúng.
4. `verify_fdk_equivalence.py` khớp cổng trực tiếp `dit_fft` 0 LSB.
5. GHDL analysis, elaboration và synthesis-elaboration không lỗi.
6. Khi có Vivado, implementation phải không còn unconstrained path và phải lấy
   số LUT/FF/DSP/BRAM/Fmax từ báo cáo post-route.

Sai số float NumPy không phải điều kiện bit-exact; nó chỉ bắt lỗi chiều FFT,
gain và ordering ở mức toán học.

## 8. Điểm nối vào MDCT

Input FFT là M số complex Q1.31 đã được khối DCT-IV pre-twiddle tạo với
headroom `/4`; M=512 cho MDCT-1024 và M=64 cho MDCT-128. `size_mode` xuất phát
từ block controller, không từ nội dung dữ liệu.

Output FFT cùng `scale_exp` đi thẳng sang DCT-IV post-twiddle. Exponent MDCT
phải cộng đúng phần của FFT: `+8` cho long hoặc `+5` cho short, ngoài các phần
scale của window/fold và pre-twiddle.
