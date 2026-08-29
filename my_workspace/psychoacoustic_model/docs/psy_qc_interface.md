# Hợp đồng dữ liệu MDCT → Psy → Quantization/Coding

## 1. Điểm nối trong source

Trong `FDKaacEnc_EncodeFrame()`, FDK đặt các pointer của `PSY_OUT_CHANNEL` trỏ trực tiếp vào vùng nhớ của `QC_OUT_CHANNEL`:

```text
psyOutChan->mdctSpectrum       -> qcOutChan->mdctSpectrum
psyOutChan->sfbSpreadEnergy    -> qcOutChan->sfbSpreadEnergy
psyOutChan->sfbEnergy          -> qcOutChan->sfbEnergy
psyOutChan->sfbEnergyLdData    -> qcOutChan->sfbEnergyLdData
psyOutChan->sfbMinSnrLdData    -> qcOutChan->sfbMinSnrLdData
psyOutChan->sfbThresholdLdData -> qcOutChan->sfbThresholdLdData
```

Điều này có hai hệ quả:

1. Psy và QC chia sẻ buffer để tránh copy dữ liệu lớn;
2. PL backend phải ghi spectrum vào đúng buffer mà phần Psy/QC đang sử dụng hoặc copy có kiểm soát vào đó trước khi tiếp tục.

## 2. Hợp đồng MDCT → Psy

### Input logic của MDCT

| Trường | Ý nghĩa |
|---|---|
| `psyInputBuffer[2048]` | Cửa sổ PCM có overlap/look-ahead |
| `lastWindowSequence` | LONG/START/SHORT/STOP đang xử lý |
| `windowShape` | SINE/KBD phía phải |
| `lastWindowShape` | State window phía trái |
| `mdctPers` | State persistent của MDCT software |
| `frameLength` | 1024 cho AAC-LC hiện tại |

### Output bắt buộc

| Trường | Kiểu/size | Ý nghĩa |
|---|---:|---|
| `mdctSpectrum` | `1024 × FIXP_DBL/Q31` | Mantissa spectral coefficient |
| `mdctSpectrum_e` | `INT` | Exponent/transform scale dùng chung |
| window state cập nhật | state | Bảo đảm frame kế tiếp dùng đúng left window |

Trong PL backend hiện tại:

```text
LONG/START/STOP -> mdct_exp = 12
SHORT           -> mdct_exp = 9
```

Nhưng sau khi Psy rescale hoặc TNS shift, `PSY_OUT_CHANNEL.mdctScale` có thể khác giá trị PL ban đầu.

## 3. Những xử lý phải diễn ra sau output PL

Không chuyển thẳng output PL vào quantizer. `psy_main.cpp` còn phải thực hiện:

1. zero spectrum phía trên `lowpassLine`;
2. kiểm tra silence;
3. tính headroom và rescale spectrum;
4. tính năng lượng SFB;
5. tonality/TNS và có thể thay đổi spectrum;
6. xây dựng masking threshold;
7. pre-echo control;
8. short grouping;
9. PNS/MS/IS;
10. tạo metadata `PSY_OUT`.

Nếu bỏ qua các bước này và gọi quantizer trực tiếp, encoder có thể vẫn sinh bitstream nhưng chất lượng, bitrate, syntax tool và scale sẽ sai.

## 4. `PSY_OUT_CHANNEL`

Theo `libAACenc/src/interface.h`, các trường chính gồm:

| Trường | Vai trò phía QC/coding |
|---|---|
| `sfbCnt` | Tổng số SFB sau grouping |
| `sfbPerGroup` | Số SFB trong một group |
| `maxSfbPerGroup` | Band cao nhất cần mã hóa |
| `lastWindowSequence` | Chọn syntax long/short |
| `windowShape` | Ghi ICS/window metadata |
| `groupingMask` | Mô tả cách group tám short window |
| `sfbOffsets[]` | Ánh xạ SFB sang spectral lines |
| `mdctScale` | Scale cuối của spectrum |
| `groupLen[]` | Độ dài từng short-window group |
| `tnsInfo` | Tham số TNS cần ghi bitstream |
| `noiseNrg[]` | Năng lượng PNS cần mã hóa |
| `isBook[]` | Intensity Stereo codebook |
| `isScale[]` | Intensity Stereo scale |
| `mdctSpectrum` | Spectrum đầu vào của quantizer |
| `sfbEnergy[]` | Năng lượng tuyến tính theo SFB |
| `sfbSpreadEnergy[]` | Năng lượng đã spreading |
| `sfbThresholdLdData[]` | Masking threshold miền log |
| `sfbMinSnrLdData[]` | Giới hạn chất lượng từng band |
| `sfbEnergyLdData[]` | Năng lượng miền log |

## 5. `PSY_OUT_ELEMENT`

Với mono/SCE, element có một channel. Với stereo/CPE, element có hai channel và thêm dữ liệu dùng chung:

| Trường | Ý nghĩa |
|---|---|
| `psyOutChannel[2]` | Output riêng từng channel |
| `commonWindow` | L/R dùng chung window information |
| `toolsInfo.msDigest` | Không dùng, dùng một phần hoặc toàn bộ MS |
| `toolsInfo.msMask[]` | Quyết định MS theo SFB |

## 6. Hợp đồng Psy → `QCMainPrepare`

`FDKaacEnc_QCMainPrepare()` đọc `PSY_OUT` để tạo:

- form factor theo SFB;
- PE data;
- static bit count dự kiến.

Các input tối quan trọng:

```text
mdctSpectrum
sfbEnergyLdData
sfbThresholdLdData
sfbMinSnrLdData
sfbSpreadEnergy
sfbOffsets/layout
tool decisions
```

Nếu chỉ dump spectrum để debug thì chưa đủ kiểm chứng Psy. Cần dump cả energy, threshold và SFB layout.

## 7. Hợp đồng `QCMain` → quantizer

Sau bit distribution và threshold adjustment, QC tạo cho mỗi channel:

| Trường | Ý nghĩa |
|---|---|
| `globalGain` | Gain lượng tử chung |
| `scf[]` | Scalefactor theo SFB |
| `sfbFormFactorLdData[]` | Hỗ trợ ước lượng distortion |
| `quantSpec[1024]` | Spectral coefficient sau lượng tử |
| `maxValueInSfb[]` | Chọn codebook/kiểm tra giới hạn |
| `sectionData` | Section/codebook và bit counts |

Quantizer dùng:

```text
mdctSpectrum
globalGain
scf[sfb]
sfbOffsets
```

để tạo `quantSpec`.

## 8. Hợp đồng Quantization → coding

`FDKaacEnc_dynBitCount()` nhận:

- `quantSpec`;
- `maxValueInSfb`;
- scalefactor;
- window/SFB grouping;
- PNS/IS flags;
- syntax flags.

Nó trả:

- codebook cho mỗi section;
- số SFB trong section;
- Huffman spectral bits;
- scalefactor bits;
- PNS energy bits;
- side-information bits;
- tổng dynamic bits.

Bitstream writer sau đó phải dùng chính `sectionData` đã được đếm. Nếu lựa chọn codebook hoặc dữ liệu thay đổi sau bước đếm bit, tổng frame bit sẽ không còn nhất quán.

## 9. Điểm móc PL backend đề xuất

Backend abstraction nên thay riêng transform:

```cpp
int transform_backend_run(
    const INT_PCM pcm[2048],
    FIXP_DBL spectrum[1024],
    int window_sequence,
    int window_shape,
    int *last_window_shape,
    int *mdct_spectrum_e);
```

Luồng trong `FDKaacEnc_psyMain()`:

```text
if backend == software:
    FDKaacEnc_Transform_Real(...)
else:
    mdct_pl_run(...)

// Không return khỏi Psy tại đây.
// Tiếp tục nguyên code low-pass, SFB energy, TNS, thresholds, PNS/MS...
```

### Điều kiện backend PL phải giữ

- output 1024 bin đúng thứ tự FDK;
- Q31 mantissa bit-exact;
- exponent đúng;
- SHORT output giữ đúng layout tám transform;
- window shape/history đúng giữa frame;
- không thay đổi `psyInputBuffer` ngoài hợp đồng;
- trả lỗi rõ ràng nếu DMA timeout/protocol error;
- không fallback giữa stream nếu state software và PL không đồng bộ.

## 10. Mono và stereo

### Mono

```text
1 channel
1 spectrum
1 mdctScale
không MS/IS
PNS/TNS vẫn có thể hoạt động
```

### Stereo

```text
2 channel spectrum
2 window/MDCT contexts
commonWindow
TNS synchronization
M/S energy và msMask
PNS channel-pair processing
Intensity Stereo decisions
```

Do Psy sửa spectrum khi dùng TNS/MS/IS, việc chạy hai MDCT channel song song trên PL không có nghĩa toàn bộ Psy stereo cũng song song độc lập. Sau MDCT vẫn có bước xử lý tương tác giữa L/R trên PS.

## 11. Source map

| Chức năng | Source chính |
|---|---|
| Top-level frame order | `libAACenc/src/aacenc.cpp` |
| Psy main | `libAACenc/src/psy_main.cpp` |
| Psy state/config | `psy_data.h`, `psy_configuration.h/.cpp` |
| Psy/QC interface | `interface.h` |
| SFB energy/spreading | `psy_main.cpp`, helper trong Psy source |
| Tonality | `tonality.cpp` |
| TNS | `aacenc_tns.cpp`, `tns_func.h` |
| PNS | `aacenc_pns.cpp`, `pnsparam.cpp`, `noisedet.cpp` |
| PE/threshold adjustment | `adj_thr.cpp`, `line_pe.cpp` |
| QC loop | `qc_main.cpp` |
| Quantization | `quantize.cpp` |
| Sectioning/bit counting | `dyn_bits.cpp`, `bit_cnt.cpp` |
| Bitstream coding | `bitenc.cpp` |

