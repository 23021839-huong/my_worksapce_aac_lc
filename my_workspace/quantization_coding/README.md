# Quantization and Coding AAC-LC trong FDK-AAC

## 1. Mục đích

Thư mục này tổng hợp phần Quantization and Coding của bộ mã hóa AAC-LC trong FDK-AAC và đánh giá các ứng viên có thể triển khai RTL trên Programmable Logic của PYNQ-Z2.

Phạm vi bắt đầu sau khi Psychoacoustic Model đã tạo `PSY_OUT`:

```text
PSY_OUT
  -> Perceptual Entropy và phân bổ bit
  -> điều chỉnh masking threshold
  -> ước lượng global gain/scalefactor
  -> lượng tử hóa spectrum
  -> chọn Huffman codebook và section
  -> đếm bit, lặp điều chỉnh gain nếu cần
  -> ghi AAC bitstream
```

Tài liệu bám theo source thật, không mô tả một encoder AAC lý tưởng hóa. Các source chính:

- [`aacenc.cpp`](../../libAACenc/src/aacenc.cpp): thứ tự xử lý một frame;
- [`qc_main.cpp`](../../libAACenc/src/qc_main.cpp): rate control và quantization loop;
- [`adj_thr.cpp`](../../libAACenc/src/adj_thr.cpp): PE, bit allocation và threshold adjustment;
- [`sf_estim.cpp`](../../libAACenc/src/sf_estim.cpp): ước lượng/cải thiện scalefactor;
- [`quantize.cpp`](../../libAACenc/src/quantize.cpp): quantize, inverse quantize và distortion;
- [`dyn_bits.cpp`](../../libAACenc/src/dyn_bits.cpp): sectioning và dynamic bit count;
- [`bit_cnt.cpp`](../../libAACenc/src/bit_cnt.cpp): bit cost/codeword của Huffman codebook;
- [`bitenc.cpp`](../../libAACenc/src/bitenc.cpp): ghi syntax và bitstream.

## 2. Kết luận phân chia PS/PL

### Nên ưu tiên khảo sát làm RTL

1. **Quantize spectral lines** — ứng viên tốt nhất.
2. **Max magnitude theo SFB** — nên ghép cùng quantizer.
3. **Distortion/quantized energy theo SFB** — ứng viên giai đoạn sau để hỗ trợ scalefactor search.
4. **Huffman bit-cost theo SFB/codebook** — có thể tăng tốc sau profiling, nhưng chỉ trả bảng cost; giữ section merging trên PS.

### Nên giữ trên Cortex-A9

- PE và bit allocation;
- threshold adjustment và bit reservoir;
- toàn bộ vòng điều khiển `QCMain()`;
- scalefactor search cấp cao;
- greedy section merging;
- lựa chọn cuối giữa PNS/IS/zero/spectral codebook;
- variable-length Huffman emission;
- AAC syntax và ADTS bitstream writer.

Lý do tổng quát:

```text
PL phù hợp:
  vòng lặp đều theo 1024 spectral lines
  fixed-point arithmetic
  ROM lookup
  reduction theo SFB

PS phù hợp:
  vòng lặp số lần không cố định
  nhánh phụ thuộc dữ liệu/bit reservoir
  cấu trúc section có độ dài thay đổi
  ghi bit biến độ dài và nhiều syntax mode
```

## 3. Không nên offload ngay trước khi profiling

Đề xuất PL trong tài liệu là đánh giá từ cấu trúc thuật toán/source, chưa phải bằng chứng speedup trên board. Trước khi viết RTL phải đo trên Cortex-A9 ít nhất:

- thời gian `FDKaacEnc_EstimateScaleFactors()`;
- thời gian `FDKaacEnc_QuantizeSpectrum()`;
- thời gian `FDKaacEnc_calcMaxValueInSfb()`;
- thời gian `FDKaacEnc_dynBitCount()`;
- số vòng lặp quantization trung bình/tối đa;
- phần trăm thời gian của QC trong toàn encoder;
- chi phí DMA/cache nếu offload.

Nếu quantization/coding chỉ chiếm phần nhỏ sau khi MDCT đã offload, thêm accelerator có thể không đem lại speedup hệ thống đáng kể.

## 4. Tài liệu trong thư mục

- [quantization_coding_pipeline.md](docs/quantization_coding_pipeline.md): luồng hoạt động chi tiết từ `PSY_OUT` tới bitstream.
- [rtl_partition_analysis.md](docs/rtl_partition_analysis.md): đánh giá từng khối cho PS/PL, lý do và dẫn chứng source.
- [accelerator_interface_and_verification.md](docs/accelerator_interface_and_verification.md): kiến trúc accelerator đề xuất, giao diện dữ liệu và kế hoạch kiểm chứng.

## 5. Quan hệ với MDCT/Psy hiện tại

Không thể nối trực tiếp output MDCT PL vào Quantizer PL trong kiến trúc hiện tại:

```text
MDCT PL
  -> PS Psychoacoustic Model
       low-pass/rescale
       TNS có thể sửa spectrum
       MS/IS có thể sửa spectrum stereo
       threshold/scalefactor calculation
  -> Quantizer
```

Quantizer phải nhận **spectrum sau Psy/TNS/stereo processing** cùng `globalGain`, `scf[]` và `sfbOffsets[]`. Vì vậy một accelerator QC riêng vẫn cần PS gửi lại spectrum sau Psy sang PL, trừ khi trong tương lai đưa thêm phần Psy lên PL.

## 6. Phạm vi phiên bản đầu đề xuất

Nếu profiling chứng minh cần tăng tốc, phiên bản RTL đầu nên khóa:

| Thuộc tính | Giá trị |
|---|---|
| AOT | AAC-LC |
| Channel | Mono trước |
| Frame length | 1024 |
| Spectrum | signed Q31/FIXP_DBL |
| Quantized output | signed 16-bit/SHORT |
| SFB layout | runtime qua `sfbOffsets[]` |
| Modes | LONG/START/SHORT/STOP grouped layout |
| Dead-zone quantizer | runtime flag |
| Chức năng | quantize + max magnitude/SFB |
| Control/rate loop | PS |
| Bitstream | PS |

Mọi phép toán phải khớp FDK bit-exact. Không thay LUT `3/4`, cách normalize, shift, offset `k`, wrap hoặc cast chỉ để thuận tiện cho RTL.

