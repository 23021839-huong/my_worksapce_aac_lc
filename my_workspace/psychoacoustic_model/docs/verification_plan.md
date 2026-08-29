# Kế hoạch kiểm chứng Psychoacoustic Model

## 1. Mục tiêu

Psychoacoustic Model có nhiều state, bảng cấu hình và phép toán fixed-point. Mục tiêu kiểm chứng không phải viết lại toàn bộ FDK bằng NumPy, mà là:

1. chứng minh output MDCT PL được Psy tiếp nhận đúng;
2. phát hiện điểm đầu tiên lệch giữa software backend và PL backend;
3. bảo đảm threshold/tool decisions/quantization không thay đổi ngoài dự kiến;
4. kiểm tra AAC end-to-end trên cùng Cortex-A9.

## 2. Oracle nên dùng

### Oracle production

Oracle chính là FDK-AAC software backend chạy trên cùng source revision và cùng kiến trúc ARM:

```text
FDKaacEnc_Transform_Real()
  + FDKaacEnc_psyMain()
  + FDKaacEnc_QCMain()
```

### Golden MDCT

Golden integer Python/C++ hiện tại tiếp tục dùng để xác nhận PL trả đúng spectrum/exponent trước khi Psy xử lý.

### NumPy

NumPy phù hợp cho các kiểm tra toán học cục bộ:

- tổng năng lượng theo SFB;
- mapping `sfbOffset`;
- quan sát spreading và threshold bằng đồ thị;
- so dấu/gain/order;
- minh họa masking.

NumPy không nên là oracle bit-exact cho toàn bộ Psy vì FDK dùng:

- fixed-point và `LdData` riêng;
- headroom/scale động;
- bảng cấu hình theo bitrate/sample rate;
- state pre-echo giữa frame;
- TNS/PNS/MS/IS;
- threshold adjustment phụ thuộc bit reservoir.

## 3. Các checkpoint cần dump

### Nhóm A — ngay sau MDCT

| Checkpoint | Dữ liệu |
|---|---|
| A1 | `frame_id`, channel ID |
| A2 | window sequence/shape |
| A3 | `mdctSpectrum[1024]` |
| A4 | `mdctSpectrum_e` |

Yêu cầu PL so software tại A3/A4: `0 LSB` và exponent bằng nhau.

### Nhóm B — spectrum/SFB ban đầu

| Checkpoint | Dữ liệu |
|---|---|
| B1 | `lowpassLine`, `sfbActive`, `sfbOffset[]` |
| B2 | spectrum sau low-pass/rescale |
| B3 | `mdctScale` sau `finalShift` |
| B4 | `sfbMaxScaleSpec[]` |
| B5 | `sfbEnergy[]`, `sfbEnergyLdData[]` |

### Nhóm C — TNS và masking

| Checkpoint | Dữ liệu |
|---|---|
| C1 | tonality theo SFB |
| C2 | TNS active/order/coefficients |
| C3 | spectrum sau TNS |
| C4 | energy tính lại sau TNS |
| C5 | threshold sau spreading |
| C6 | threshold sau PCM floor và pre-echo |
| C7 | state `sfbThresholdnm1[]`, `mdctScalenm1` |

### Nhóm D — output Psy

| Checkpoint | Dữ liệu |
|---|---|
| D1 | grouped `sfbOffsets[]`, `groupLen[]`, `groupingMask` |
| D2 | `sfbEnergyLdData[]` |
| D3 | `sfbThresholdLdData[]` |
| D4 | `sfbMinSnrLdData[]` |
| D5 | PNS `noiseNrg[]` |
| D6 | MS mask/IS book-scale nếu stereo |
| D7 | `PSY_OUT.mdctScale` |

### Nhóm E — QC và coding

| Checkpoint | Dữ liệu |
|---|---|
| E1 | PE và granted PE/bits |
| E2 | adjusted thresholds |
| E3 | `globalGain`, `scf[]` |
| E4 | `quantSpec[1024]` |
| E5 | `maxValueInSfb[]` |
| E6 | section/codebook map |
| E7 | spectral/scalefactor/side-info bit counts |
| E8 | total frame bits và bit reservoir state |

## 4. Trình tự kiểm chứng

### Test 1 — software trace trên PC

1. build FDK với trace compile-time chỉ dùng cho test;
2. encode corpus mono 48 kHz;
3. dump checkpoint A–E;
4. khóa source revision, config và checksum input;
5. xác nhận output AAC decode được.

### Test 2 — software trace trên Cortex-A9

1. build cùng source revision trên PYNQ-Z2;
2. encode cùng input/config;
3. so các checkpoint với PC;
4. nếu có lệch kiến trúc, dùng ARM trace làm baseline tích hợp board.

### Test 3 — PL MDCT độc lập

1. lấy A1–A4 từ software ARM;
2. gửi 2048 PCM + metadata vào PL;
3. so 1024 spectrum bin và exponent;
4. chưa tích hợp FDK cho đến khi toàn bộ corpus khớp.

### Test 4 — FDK với PL backend

1. chạy cùng input hai lần: software backend và PL backend;
2. dump checkpoint A–E;
3. tìm checkpoint đầu tiên lệch;
4. nếu A khớp nhưng B lệch, lỗi nằm ở buffer/scale/điểm resume trong Psy;
5. nếu B–D khớp nhưng E lệch, kiểm tra state QC/bit reservoir/config;
6. so AAC output và decode result.

## 5. Corpus tối thiểu

| Signal | Mục đích |
|---|---|
| Silence | zero spectrum, threshold floor, PNS silence behavior |
| Sine một tone | tonal band, SFB mapping, Huffman sparsity |
| Multi-tone | spreading và nhiều SFB hoạt động |
| White/pink noise | tonality thấp và PNS |
| Impulse | transient, SHORT switching và pre-echo |
| Castanets/clap | chuỗi LONG–START–SHORT–STOP |
| Crescendo | threshold/state thay đổi chậm |
| Near-full-scale PCM | headroom, clipping và overflow |
| Rất nhỏ gần zero | PCM threshold floor và fixed-point precision |

Với stereo sau này cần thêm:

- L = R để kiểm tra MS;
- L = −R để kiểm tra Side;
- tín hiệu lệch mức giữa L/R;
- noise tương quan/không tương quan;
- transient chỉ xuất hiện một channel.

## 6. Nguyên tắc triển khai trace

- trace phải bật bằng macro compile-time và tắt trong production;
- không thay đổi thứ tự phép toán fixed-point;
- không gọi hàm float chỉ để log trong đường xử lý chính;
- dump bit pattern nguyên gốc kèm scale, không chỉ dump số thực đã đổi;
- mỗi record phải có frame ID, channel, window/group và SFB/bin index;
- ghi manifest chứa sample rate, bitrate, AOT, channel mode và source revision;
- không dùng cùng implementation để vừa sinh expected vừa kiểm DUT.

## 7. Tiêu chí PASS

### MDCT boundary

- 1024/1024 mantissa khớp 0 LSB;
- exponent khớp;
- block/window history khớp.

### Psy/QC deterministic path

Khi cùng architecture, build flags và source revision:

- SFB layout khớp;
- spectrum sau low-pass/TNS khớp;
- energy/threshold bit pattern khớp;
- tool decisions khớp;
- PE, gains, scalefactors và quantSpec khớp;
- section/codebook và bit count khớp.

### End-to-end

- bitstream hợp lệ;
- decoder giải mã toàn bộ frame;
- không có DMA/cache/protocol error;
- không miss deadline 1024/48 kHz;
- chất lượng và bitrate không suy giảm ngoài tiêu chí dự án.

## 8. Chẩn đoán theo điểm lệch đầu tiên

| Điểm lệch đầu tiên | Khả năng lỗi cao |
|---|---|
| PCM snapshot | Input pipeline/overlap/look-ahead |
| MDCT spectrum | RTL, DMA ordering, endian hoặc window state |
| Exponent | Scale contract PL/HAL |
| Low-pass spectrum | Resume sai vị trí trong `psy_main` hoặc config khác |
| SFB energy | Exponent/headroom/SFB offset |
| TNS spectrum | TNS state hoặc input energy khác |
| Threshold | pre-echo state/config/scale |
| PNS/MS decision | tonality/energy/threshold hoặc stereo sync |
| PE | PSY_OUT interface hoặc adj-threshold state |
| QuantSpec | gain/scalefactor/quantizer config |
| Bit count | section/codebook/syntax flags |
| AAC bytes | transport header/fill/alignment/bit reservoir |

