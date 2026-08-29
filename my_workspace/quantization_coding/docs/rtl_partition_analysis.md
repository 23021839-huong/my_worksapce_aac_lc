# Phân tích phần Quantization and Coding nên đưa lên RTL/PL

## 1. Nguyên tắc đánh giá

Mỗi khối được đánh giá theo:

| Tiêu chí | Thuận lợi cho PL |
|---|---|
| Dữ liệu song song | Nhiều spectral line/SFB độc lập |
| Điều khiển | Loop cố định, ít nhánh dữ liệu |
| Số học | Fixed-point, shift, multiply, LUT |
| Bộ nhớ | Truy cập tuần tự, dung lượng phù hợp BRAM |
| Tái sử dụng | Được gọi nhiều lần trong QC loop |
| Giao diện | Input/output đủ lớn để bù DMA overhead |
| State | Ít state xuyên frame |

Các nhận định dưới đây dựa trên cấu trúc source. Chúng là dẫn chứng về **khả năng ánh xạ RTL**, không phải số đo speedup. Profiling ARM là điều kiện bắt buộc trước implementation.

## 2. Ma trận quyết định

| Khối | Đặc tính source | Khả năng PL | Khuyến nghị |
|---|---|---:|---|
| Quantize spectral lines | 1024 phần tử, cùng datapath normalize/LUT/multiply/shift | Cao | Ưu tiên 1 |
| Max magnitude/SFB | Reduction tuần tự/độc lập theo SFB | Rất cao | Ghép với quantizer |
| Inverse quantize + distortion | Cùng kiểu per-line LUT/multiply, được gọi lặp trong scalefactor search | Cao | Ưu tiên 2 sau profiling |
| Huffman bit-cost/SFB | LUT/add/sign/escape, song song theo codebook | Trung bình–cao | Ưu tiên 3, trả cost vector |
| Form factor/PE | Log-domain, nhiều SFB nhưng gắn threshold/state | Trung bình | Giữ PS ban đầu |
| Threshold adjustment | Nhiều state, CBR/VBR, avoid-hole, reservoir | Thấp | Giữ PS |
| Scalefactor search control | Nhiều vòng thử, nhánh và scan SFB | Thấp–trung bình | Giữ control trên PS; có thể offload primitive distortion |
| QC iteration controller | `do/while`, số vòng phụ thuộc dữ liệu và bit budget | Thấp | Giữ PS |
| Greedy section merge | Cấu trúc variable-length, tìm max merge gain lặp | Thấp | Giữ PS |
| Huffman code emission | Variable-length bit packing, syntax phụ thuộc tool | Thấp | Giữ PS |
| ADTS/AAC bitstream | Bit-serial, nhiều trường/nhánh, workload nhỏ | Rất thấp | Giữ PS |

## 3. Ứng viên 1 — Quantize Spectrum

### Dẫn chứng source

Trong [`quantize.cpp`](../../../libAACenc/src/quantize.cpp), `FDKaacEnc_QuantizeSpectrum()` duyệt group/SFB rồi gọi `FDKaacEnc_quantizeLines()` cho toàn bộ spectral line hoạt động.

Datapath mỗi line gồm:

```text
Q31 spectrum
  -> multiply với quantizer
  -> abs/sign
  -> leading-zero count
  -> normalize
  -> ROM 3/4 lookup
  -> exponent ROM/multiply
  -> variable shift
  -> dead-zone offset
  -> signed 16-bit output
```

Đây là cấu trúc phù hợp RTL vì:

- mỗi line dùng cùng một datapath;
- input/output tuần tự;
- ROM có kích thước cố định và có thể suy luận BRAM/distributed ROM;
- CLZ, shifter, multiplier và saturation/cast có thể pipeline;
- SFB chỉ thay `gain = globalGain - scf` tại ranh giới band;
- tối đa 1024 line mỗi channel.

### Lợi ích tiềm năng

- giảm workload phi tuyến/lượng tử trên Cortex-A9;
- cùng datapath có thể dùng lại nhiều lần khi QC đổi global gain;
- đầu ra `quantSpec` cần cho cả bit counting và bitstream;
- có thể tính `maxValueInSfb` trong cùng pass mà gần như không tăng băng thông.

### Rủi ro

- phải bit-exact với thứ tự normalize/lookup/shift/cast FDK;
- gain có thể khác theo SFB;
- QC có thể gọi quantizer nhiều iteration;
- DMA round-trip mỗi iteration có thể lớn hơn thời gian ARM nếu thiết kế không giữ dữ liệu trong PL;
- dead-zone mode là runtime option.

### Kết luận

Đây là ứng viên RTL tốt nhất, nhưng chỉ triển khai sau khi profile xác nhận `QuantizeSpectrum`/distortion primitives chiếm tỷ lệ đáng kể.

## 4. Ứng viên ghép — Max magnitude per SFB

### Dẫn chứng source

Sau mỗi lần quantize, [`qc_main.cpp`](../../../libAACenc/src/qc_main.cpp) gọi `FDKaacEnc_calcMaxValueInSfb()` để tìm maximum absolute quantized value từng SFB và kiểm tra `MAX_QUANT`.

### Ánh xạ RTL

Trong lúc quantizer phát mỗi `quantSpec[k]`:

```text
abs_q = abs(quantSpec[k])
max_sfb = max(max_sfb, abs_q)
```

Tại ranh giới `sfbOffset[next]`, ghi `maxValueInSfb[sfb]` rồi reset accumulator.

### Lý do nên fuse

- không cần đọc lại toàn bộ 1024 output;
- không cần accelerator/transaction riêng;
- output chỉ tối đa `MAX_GROUPED_SFB` giá trị;
- giúp PS biết ngay có vi phạm `MAX_QUANT`.

## 5. Ứng viên 2 — Inverse Quantization và SFB Distortion

### Dẫn chứng source

[`sf_estim.cpp`](../../../libAACenc/src/sf_estim.cpp) gọi nhiều lần:

- `FDKaacEnc_calcSfbDist()`;
- `FDKaacEnc_calcSfbQuantEnergyAndDist()`.

Các hàm trong [`quantize.cpp`](../../../libAACenc/src/quantize.cpp) thực hiện quantize, inverse quantize với LUT `4/3`, rồi tích lũy sai số/năng lượng theo line.

Khi Afterburner/inverse-quantization được bật, scalefactor search là analysis-by-synthesis và thử nhiều giá trị scale. Đây có thể là workload lớn hơn một lần quantize cuối.

### Cách phân chia hợp lý

Giữ trên PS:

- chọn SFB/scalefactor cần thử;
- so distortion với threshold;
- assimilate single/multiple scalefactors;
- quyết định accept/reject.

Đưa lên PL primitive:

```text
RUN_QDIST(sfb_start, sfb_width, gain)
  -> quantized values tùy chọn
  -> quantized energy
  -> distortion
```

### Rủi ro

- nhiều lệnh SFB nhỏ làm AXI-Lite overhead lớn;
- cần giữ spectrum trong BRAM để không DMA lại mỗi lần thử;
- output distortion phải giữ đúng fixed-point/log scale;
- scratch/quantSpec semantics của Afterburner phải được mô phỏng đúng.

### Kết luận

Chỉ làm sau quantizer cơ bản và sau profiling có Afterburner bật/tắt. Không nên port toàn bộ `sf_estim.cpp` sang RTL.

## 6. Ứng viên 3 — Huffman bit-cost theo SFB

### Dẫn chứng source

Trong [`dyn_bits.cpp`](../../../libAACenc/src/dyn_bits.cpp), `FDKaacEnc_buildBitLookUp()` gọi `FDKaacEnc_bitCount()` cho mỗi SFB để tạo:

```text
bitLookUp[sfb][codebook]
```

[`bit_cnt.cpp`](../../../libAACenc/src/bit_cnt.cpp) đếm codebook 1–11 bằng:

- nhóm quantized values theo cặp/bộ bốn;
- table lookup code lengths;
- sign-bit count;
- escape-bit count;
- accumulation.

Các cost codebook cho cùng SFB có thể tính song song hoặc time-multiplex trên một datapath LUT/add.

### Ranh giới nên offload

PL chỉ nên trả:

```text
cost[sfb][0..11/escape]
```

PS tiếp tục:

- áp PNS/IS codebook override;
- chọn best book;
- Stage 1/Stage 2 section merging;
- scalefactor/PNS bit count;
- quyết định tổng budget.

### Vì sao không đưa section merging cùng lúc?

`FDKaacEnc_gmStage2()` dùng vòng `while`:

```text
tìm merge gain lớn nhất
  -> merge hai section
  -> sửa linked/index state
  -> cập nhật cost lân cận
  -> lặp đến khi không còn gain dương
```

Số iteration và pattern memory thay đổi theo frame, ít phù hợp pipeline RTL và workload chỉ trên tối đa vài chục SFB.

### Kết luận

Bit-cost kernel là ứng viên trung hạn nếu profiling cho thấy `dynBitCount/bitCount` đáng kể. Không phải ưu tiên trước quantizer.

## 7. Khối nên giữ trên PS — Threshold/PE/Bit Reservoir

### Dẫn chứng source

[`adj_thr.cpp`](../../../libAACenc/src/adj_thr.cpp) có nhiều hàm/state:

- adapt min-SNR;
- avoid-hole initialization/reset;
- CBR/VBR threshold formulas;
- threshold correction;
- allow-more-holes fallback;
- PE correction qua frame;
- bit-spend/save theo reservoir fill level.

[`qc_main.cpp`](../../../libAACenc/src/qc_main.cpp) còn phân phối bit giữa element, extension/static bits, padding và crash recovery.

### Lý do giữ PS

- chỉ xử lý tối đa vài chục SFB/element, không phải 1024 line;
- nhiều branch và state xuyên frame;
- phụ thuộc mode/bitrate/syntax;
- thay đổi nhỏ ảnh hưởng chất lượng và reservoir toàn stream;
- khó chứng minh lợi ích so với chi phí RTL/verification.

## 8. Khối nên giữ trên PS — Scalefactor Search Control

### Dẫn chứng source

[`sf_estim.cpp`](../../../libAACenc/src/sf_estim.cpp) có:

- scan SFB;
- nhiều scratch arrays;
- thử scalefactor tăng/giảm;
- vòng assimilate single/multiple SFB;
- gọi distortion kernel lặp;
- sửa zero bands và giới hạn `MAX_SCF_DELTA`.

### Lý do

Control phức tạp nhưng primitive bên trong có thể tăng tốc. Cách codesign hợp lý:

```text
ARM: thuật toán search
PL : quantize/inverse-quantize/distortion cho candidate
```

## 9. Khối nên giữ trên PS — QC iteration controller

### Dẫn chứng source

`FDKaacEnc_QCMain()` chứa nested `do/while`, flags theo subframe/element/channel và các điều kiện:

- `constraintsFulfilled`;
- `calculateQuant`;
- `decreaseBitConsumption`;
- `dynBitsOvershoot`;
- maximum iterations;
- crash recovery;
- min/max total bits.

### Lý do

Đây là state machine phần mềm cấp cao với số vòng dữ liệu phụ thuộc. Đưa toàn bộ lên PL sẽ kéo theo hầu hết cấu trúc QC, Psy metadata và bit reservoir, tạo giao diện lớn và verification rất khó.

## 10. Khối nên giữ trên PS — Sectioning/Huffman emission/Bitstream

### Sectioning

- số section thay đổi;
- greedy merge có loop dữ liệu phụ thuộc;
- chỉ có tối đa vài chục SFB;
- có codebook đặc biệt PNS/IS.

### Huffman emission

[`FDKaacEnc_codeValues()`](../../../libAACenc/src/bit_cnt.cpp) tạo codeword variable-length và sign/escape bits. [`bitenc.cpp`](../../../libAACenc/src/bitenc.cpp) ghép nhiều syntax field vào bitstream.

### Lý do giữ PS

- workload không lớn bằng per-line quantization;
- bit packing serial và nhiều nhánh;
- encoder đã phải chạy transport/ADTS trên PS;
- chi phí đưa variable-length fragments qua AXI có thể lớn;
- lỗi một bit làm hỏng toàn access unit.

## 11. Kiến trúc tăng tốc theo giai đoạn

### Giai đoạn 0 — profile

Không viết RTL trước khi có số liệu ARM.

### Giai đoạn 1 — `QMAX` accelerator

```text
input : spectrum + SFB offsets + globalGain + scf + dead-zone flag
output: quantSpec + maxValueInSfb + violation flag
```

PS giữ toàn bộ QC loop và `dynBitCount()`.

### Giai đoạn 2 — persistent spectrum + `QDIST`

PL giữ spectrum sau Psy trong BRAM. ARM gửi các candidate gain/scf, PL trả distortion/energy và chỉ trả quantSpec cuối khi accept.

### Giai đoạn 3 — codebook cost

PL tính `bitLookUp[sfb][book]`; ARM chạy greedy sectioning và quyết định budget.

### Không khuyến nghị trong phạm vi hiện tại

- full QC in PL;
- full Huffman/ADTS bitstream writer in PL;
- nối thẳng MDCT PL sang quantizer trước Psy;
- stereo song song trước khi mono bit-exact.

## 12. Dẫn chứng băng thông và overhead

Một pass mono tối thiểu:

```text
spectrum input   = 1024 × 4 byte = 4096 byte
quantSpec output = 1024 × 2 byte = 2048 byte
SFB metadata     = vài trăm byte
```

Ở 48 kHz:

```text
frame rate = 48000 / 1024 = 46.875 frame/s
```

Một pass chỉ khoảng 288 kB/s cho hai mảng chính, rất nhỏ so với AXI HP. Vấn đề không phải bandwidth cực đại mà là:

- DMA setup;
- cache flush/invalidate;
- interrupt/polling;
- số pass lặp trong scalefactor/QC search.

Do đó thiết kế giữ spectrum trong PL qua nhiều iteration có tiềm năng hơn việc DMA toàn bộ spectrum qua lại cho từng candidate.

## 13. Tiêu chí ra quyết định cuối

Chỉ triển khai một candidate khi đồng thời đạt:

- chiếm tỷ lệ CPU đáng kể trong profile thực;
- đủ số lần gọi/frame để bù chi phí tích hợp;
- có hợp đồng fixed-point khóa được;
- có golden boundary rõ;
- fit tài nguyên sau MDCT trên XC7Z020;
- tổng latency bao gồm DMA/cache tốt hơn software;
- không làm tăng rủi ro bitstream/conformance quá mức.

