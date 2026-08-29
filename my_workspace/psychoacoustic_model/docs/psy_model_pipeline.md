# Luồng hoạt động chi tiết của Psychoacoustic Model

## 1. Vị trí thật trong FDK-AAC

Entry point của một frame là `FDKaacEnc_EncodeFrame()` trong `libAACenc/src/aacenc.cpp`. Với mỗi channel element SCE/CPE/LFE, hàm gọi:

```cpp
FDKaacEnc_psyMain(...);
FDKaacEnc_QCMainPrepare(...);
```

Sau khi tất cả element đã được phân tích, encoder gọi `FDKaacEnc_QCMain()` để phân bổ bit, lượng tử hóa và đếm bit, rồi gọi bitstream writer.

Đối với AAC-LC mono:

```text
1 SCE
  -> 1 channel
  -> 1 psyInputBuffer[2048]
  -> 1 spectrum[1024]
  -> một tập energy/threshold theo SFB
```

Đối với stereo:

```text
1 CPE
  -> 2 channel
  -> Psy xử lý L/R
  -> có thể chuyển một số SFB sang M/S hoặc Intensity Stereo
```

## 2. Bước 0 — cấu hình Psychoacoustic Model

Khi khởi tạo encoder, `FDKaacEnc_psyMainInit()` gọi `FDKaacEnc_InitPsyConfiguration()` cho long và short window. Cấu hình phụ thuộc vào:

- Audio Object Type;
- bitrate và bitrate mỗi channel;
- sample rate;
- frame/granule length;
- mono/stereo;
- bật/tắt MS, IS, PNS, TNS;
- low-pass bandwidth;
- LC/LD/ELD filterbank.

Các bảng/giá trị chính gồm:

- `sfbOffset[]`: biên các scalefactor band;
- `sfbActive`: số band được xử lý;
- `lowpassLine`: MDCT line cao nhất được giữ;
- `sfbMaskLowFactor[]`, `sfbMaskHighFactor[]`: hệ số spreading masking;
- `sfbMinSnrLdData[]`: giới hạn SNR tối thiểu;
- `sfbPcmQuantThreshold[]`: sàn threshold do lượng tử PCM đầu vào;
- `clipEnergy`: giới hạn threshold theo mức tín hiệu;
- tham số TNS/PNS.

Không nên hard-code số SFB trong accelerator hoặc HAL. SFB layout là quyết định của cấu hình encoder và có thể thay đổi theo sample rate, block dài/ngắn và profile.

## 3. Bước 1 — Block Switching và chọn hình học cửa sổ

`FDKaacEnc_psyMain()` trước tiên chạy Block Switching trên 1024 mẫu mới:

```text
LONG / START / SHORT / STOP
```

Với stereo, kết quả hai channel được đồng bộ bằng `FDKaacEnc_SyncBlockSwitching()` để có common window hợp lệ.

Block type quyết định:

- một MDCT-1024 cho LONG/START/STOP;
- tám MDCT-128 cho SHORT;
- SFB configuration long hay short;
- số window và cách group short window;
- pre-echo, TNS và stereo processing phía sau.

## 4. Bước 2 — Window/MDCT

FDK gọi:

```cpp
FDKaacEnc_Transform_Real(
    psyInputBuffer,
    mdctSpectrum,
    windowSequence,
    windowShape,
    lastWindowShape,
    mdctPersistentState,
    frameLength,
    &mdctSpectrum_e,
    filterbank);
```

Kết quả gồm:

```text
mdctSpectrum[1024] : FIXP_DBL/Q31 mantissa
mdctSpectrum_e     : exponent/scale dùng chung
```

Trong thiết kế PYNQ-Z2, đây là điểm được thay bằng MDCT PL. Mọi bước từ mục 5 trở đi vẫn dùng code FDK trên ARM.

## 5. Bước 3 — Low-pass và cập nhật input history

Với mỗi window, các MDCT line từ `lowpassLine` trở lên được đặt bằng 0. Mục đích:

- giới hạn bandwidth theo bitrate/cấu hình;
- không tốn bit cho miền tần số encoder không định mã hóa;
- giảm số SFB hoạt động thực tế.

Sau đó `psyInputBuffer` được rotate/copy để chuẩn bị đúng overlap và look-ahead cho frame tiếp theo.

Đây là lý do PL backend không được tự ý quản lý một lịch sử khác với PS nếu chưa có hợp đồng state rõ ràng.

## 6. Bước 4 — chuẩn hóa spectrum và tính năng lượng theo SFB

### 6.1 Chia phổ thành scalefactor bands

Thay vì đánh giá riêng từng MDCT line, AAC gom các line thành SFB:

```text
SFB i = X[sfbOffset[i] ... sfbOffset[i+1]-1]
```

SFB mô phỏng gần đúng độ phân giải tần số không đồng đều của thính giác và cũng là đơn vị điều khiển scalefactor/Huffman section.

### 6.2 Headroom và block floating-point

FDK tính khả năng left-shift an toàn cho từng band bằng `FDKaacEnc_CalcSfbMaxScaleSpec()`. Sau đó chọn `finalShift` sao cho:

- spectrum có độ chính xác tốt nhất;
- phép tính bình phương/tổng năng lượng không overflow;
- threshold lượng tử PCM vẫn biểu diễn được;
- short block còn headroom khi group năng lượng.

Nếu spectrum được left-shift, `mdctScale` được giảm tương ứng để giá trị vật lý không thay đổi:

```text
mantissa mới = mantissa cũ × 2^shift
mdctScale mới = mdctScale cũ - shift
```

Vì vậy exponent từ PL là input cho Psy, nhưng Psy có thể tiếp tục đổi scale nội bộ sau đó. Không nên giả định `mdct_exp` của PL luôn bằng `PSY_OUT.mdctScale` cuối cùng.

### 6.3 Năng lượng band

Về mặt khái niệm:

```text
E[sfb] = tổng X[k]^2, với k thuộc SFB
```

Trong FDK, phép tính dùng fixed-point, headroom và dạng logarithmic `LdData` để tránh overflow và hỗ trợ các phép so/điều chỉnh threshold hiệu quả.

Các dạng dữ liệu liên quan:

- `sfbEnergy`: năng lượng tuyến tính;
- `sfbEnergyLdData`: năng lượng ở miền log nội bộ;
- `sfbMaxScaleSpec`: headroom cực đại của spectrum trong band;
- `sfbThreshold`: masking threshold đang được phát triển.

Threshold ban đầu được suy ra từ năng lượng band với một tỉ lệ cố định. Trong source, `C_RATIO` tương ứng xấp xỉ `10^(-29/10)` trước các scale nội bộ. Đây mới là điểm khởi đầu; threshold còn được spreading, giới hạn và pre-echo control ở các bước sau.

Nếu toàn bộ spectrum bằng 0, FDK đặt năng lượng/threshold về trạng thái silence đặc biệt và đưa `mdctScale` về 0 để tránh threshold bị dịch xuống 0 không mong muốn.

## 7. Bước 5 — tonality và TNS

### 7.1 Tonality

Với long window, `FDKaacEnc_CalculateFullTonality()` ước lượng một band giống:

- tín hiệu tonal: năng lượng tập trung vào một số line;
- tín hiệu noise-like: năng lượng phân tán/ngẫu nhiên hơn.

Thông tin tonality được dùng trong quyết định PNS và các đánh giá nâng cao. Tonal component thường nhạy với việc thay bằng nhiễu hơn noise-like component.

### 7.2 TNS detection

`FDKaacEnc_TnsDetect()` phân tích spectrum để quyết định có dùng Temporal Noise Shaping hay không và xác định tham số bộ lọc dự đoán.

TNS không thay MDCT bằng transform khác. Nó lọc spectrum trước lượng tử hóa để định hình nhiễu lượng tử theo thời gian sau khi decoder áp dụng inverse TNS.

TNS đặc biệt hữu ích khi năng lượng thay đổi nhanh theo thời gian, vì nhiễu lượng tử trải đều trong một window dài có thể gây pre-echo.

### 7.3 TNS encode và tính lại năng lượng

Nếu TNS được bật:

1. FDK đồng bộ quyết định TNS giữa hai channel khi cần;
2. điều chỉnh headroom spectrum;
3. `FDKaacEnc_TnsEncode()` lọc spectrum;
4. tính lại `sfbMaxScaleSpec` và năng lượng band;
5. cập nhật `mdctScale` nếu phải shift để tránh overflow;
6. lưu `tnsInfo` để bitstream writer mã hóa side information.

Do spectrum bị biến đổi bởi TNS, quantizer phía sau phải nhận spectrum sau TNS, không phải bản MDCT thô từ PL.

## 8. Bước 6 — xây dựng masking threshold

### 8.1 Giới hạn clipping

Threshold được giới hạn bởi `clipEnergy`. Mục tiêu là tránh cho phép nhiễu quá lớn ở vùng tín hiệu mạnh chỉ vì năng lượng band lớn.

### 8.2 Spreading function

`FDKaacEnc_SpreadingMax()` lan ảnh hưởng masking giữa các SFB lân cận:

```text
band có năng lượng mạnh
  -> che một phần band thấp hơn/cao hơn ở gần nó
```

Hai hướng dùng hệ số riêng:

- `sfbMaskLowFactor`;
- `sfbMaskHighFactor`.

Masking không đối xứng hoàn toàn theo tần số, nên không thể chỉ lấy trung bình đơn giản của các band lân cận.

### 8.3 Sàn do lượng tử PCM

FDK không yêu cầu threshold thấp hơn mức nhiễu vốn đã xuất hiện từ lượng tử PCM đầu vào. `sfbPcmQuantThreshold` tạo một sàn threshold phụ thuộc SFB và scale hiện tại.

### 8.4 Pre-echo control

`FDKaacEnc_PreEchoControl()` so threshold hiện tại với state của frame/window trước:

```text
sfbThresholdnm1[]
mdctScalenm1
calcPreEcho
```

Khi transient xuất hiện, threshold của frame hiện tại có thể tăng rất nhanh. Nếu quantizer ngay lập tức cho phép nhiễu lớn trên toàn cửa sổ, một phần nhiễu sẽ xuất hiện trước transient và nghe như tiếng “xì” đi trước cú đánh.

Pre-echo control giới hạn tốc độ tăng threshold và giữ lại một phần threshold trước đó. Khi chuyển START/STOP/SHORT, FDK reset hoặc điều chỉnh state để không so trực tiếp các hình học band/window không tương thích.

### 8.5 Spread energy

FDK tạo thêm `sfbSpreadEnergy`, là năng lượng đã spread qua các band. Dữ liệu này giúp phát hiện spectral hole và được dùng trong threshold/scalefactor adjustment phía QC.

## 9. Bước 7 — năng lượng M/S và grouping short window

### 9.1 Mid/Side energy

Với stereo, Psy tính năng lượng của:

```text
Mid  ~ L + R
Side ~ L - R
```

theo từng SFB. Nếu L và R tương quan mạnh, Side có thể nhỏ và mã hóa M/S tiết kiệm bit hơn mã hóa độc lập L/R.

Mono bỏ qua hoàn toàn bước này.

### 9.2 Group short data

SHORT gồm tám MDCT-128. FDK group các short window dựa trên `groupLen[]`:

- sắp xếp lại spectrum theo group;
- gom energy/threshold/spread energy;
- tạo `groupedSfbOffset[]`;
- xác định `maxSfbPerGroup`;
- tạo `groupingMask` cho bitstream.

Sau bước này, giao diện Psy → QC dùng một layout grouped thống nhất thay vì tám mảng short rời rạc.

## 10. Bước 8 — PNS, MS và Intensity Stereo

### 10.1 PNS detection

`FDKaacEnc_PnsDetect()` tìm các SFB noise-like phù hợp để thay bằng Perceptual Noise Substitution. Quyết định xem xét:

- tonality;
- năng lượng và threshold;
- band hoạt động;
- TNS state/prediction gain;
- bitrate và cấu hình PNS.

Nếu dùng PNS, encoder không mã hóa toàn bộ spectral coefficient của band. Nó mã hóa dấu hiệu PNS và mức năng lượng nhiễu; decoder tái tạo noise tương ứng.

### 10.2 Stereo tools

Với hai channel, FDK có thể chạy:

- `FDKaacEnc_IntensityStereoProcessing()`;
- `FDKaacEnc_MsStereoProcessing()`;
- tiền/hậu xử lý PNS theo channel pair.

Kết quả được lưu qua:

- `toolsInfo.msDigest`;
- `toolsInfo.msMask[]`;
- `isBook[]`;
- `isScale[]`;
- `noiseNrg[]`.

Trong bản mono đầu tiên, các cấu trúc vẫn tồn tại nhưng MS/IS không có tác dụng.

### 10.3 PNS coding preparation

`FDKaacEnc_CodePnsChannel()` chuyển quyết định PNS thành `noiseNrg[]` và cập nhật threshold cần thiết. `noiseNrg[]` sau đó được QC/bitstream writer dùng để chọn codebook PNS và ghi năng lượng noise.

## 11. Bước 9 — tạo `PSY_OUT`

Kết quả cuối của Psy cho mỗi channel gồm:

- spectrum đã low-pass/TNS/stereo processing;
- `mdctScale` cuối;
- `sfbEnergy` và `sfbEnergyLdData`;
- `sfbThresholdLdData`;
- `sfbMinSnrLdData`;
- `sfbSpreadEnergy`;
- `sfbOffsets`;
- `sfbCnt`, `sfbPerGroup`, `maxSfbPerGroup`;
- window sequence/shape và grouping;
- `tnsInfo`;
- PNS/IS data;
- MS mask cho channel pair.

Đây không chỉ là “một mảng threshold”. Nó là hợp đồng đầy đủ để QC có thể phân bổ bit, chọn scalefactor, lượng tử hóa và tạo syntax AAC.

## 12. Bước 10 — `QCMainPrepare`: từ Psy sang độ khó mã hóa

Ngay sau Psy, `FDKaacEnc_QCMainPrepare()` thực hiện:

1. `FDKaacEnc_CalcFormFactor()`;
2. `FDKaacEnc_peCalculation()`;
3. gọi bitstream writer ở chế độ đếm để ước lượng static bit demand.

### Form factor

Form factor mô tả phân bố biên độ spectral line trong SFB và hỗ trợ ước lượng distortion/scalefactor.

### Perceptual Entropy — PE

PE là ước lượng độ khó mã hóa cảm nhận của frame/element:

```text
energy cao so với threshold
  -> cần biểu diễn chính xác hơn
  -> PE cao
  -> cần nhiều bit hơn
```

PE không phải entropy Shannon chính xác của bitstream. Nó là đại lượng điều khiển để ánh xạ masking demand sang ngân sách bit.

### Static bit demand

AAC frame còn cần bit cho:

- ICS/window/grouping information;
- TNS;
- MS/IS/PNS signaling;
- section data;
- scalefactor metadata;
- transport/extension data.

FDK đếm trước các bit này để biết phần ngân sách còn lại cho spectral data.

## 13. Bước 11 — threshold adjustment và phân bổ bit

Trong `FDKaacEnc_QCMain()`:

1. điều hòa bit reservoir giữa các element/frame;
2. tính số dynamic bit khả dụng;
3. `FDKaacEnc_DistributeBits()` phân bổ bit dựa trên PE;
4. `FDKaacEnc_AdjustThresholds()` điều chỉnh masking threshold để phù hợp ngân sách.

Nếu bitrate thấp hơn nhu cầu ban đầu, threshold phải tăng có kiểm soát:

```text
threshold tăng
  -> cho phép distortion lớn hơn
  -> scalefactor thô hơn
  -> ít bit hơn
```

Quá trình vẫn bị giới hạn bởi min-SNR và các cơ chế tránh tạo spectral hole/artefact nghiêm trọng.

## 14. Bước 12 — scalefactor và quantization loop

### 14.1 Ước lượng scalefactor

`FDKaacEnc_EstimateScaleFactors()` biến energy/threshold/form factor thành:

- `globalGain`;
- `scf[sfb]`;
- các tham số cần cho quantizer.

Scalefactor điều khiển bước lượng tử riêng theo band, còn global gain đặt mức chung.

### 14.2 Lượng tử hóa phi tuyến

`FDKaacEnc_QuantizeSpectrum()` lượng tử hóa từng spectral line theo global gain và scalefactor. Về mặt khái niệm AAC dùng luật lũy thừa gần `|X|^(3/4)` trước khi đưa về integer quantized coefficient.

Output:

```text
quantSpec[1024] : SHORT/integer spectral values
```

### 14.3 Vòng phản hồi bit

FDK không lượng tử hóa đúng một lần. Nó lặp:

```text
ước lượng gain/scalefactor
  -> quantize
  -> kiểm tra max quantized value
  -> chọn section/codebook và đếm bit
  -> so với ngân sách
  -> chỉnh gain/threshold
  -> quantize lại nếu cần
```

Vòng lặp dừng khi:

- quantized values nằm trong giới hạn syntax;
- dynamic bits không vượt ngân sách;
- tổng frame bits phù hợp bit reservoir/rate control;
- đạt các ràng buộc tối thiểu của encoder;
- hoặc cơ chế recovery được kích hoạt sau số vòng lặp giới hạn.

## 15. Bước 13 — sectioning và Huffman coding

`FDKaacEnc_dynBitCount()`:

1. đếm số bit của từng Huffman codebook ứng viên cho mỗi SFB;
2. chọn codebook có chi phí thấp;
3. ghép các SFB kề nhau thành section;
4. cân bằng spectral bits và side-information bits;
5. đếm scalefactor, PNS và intensity bits;
6. trả `sectionData` và tổng dynamic bit demand.

Các codebook phổ AAC mã hóa cặp/bộ spectral coefficient với miền giá trị khác nhau. Codebook 0 đại diện band toàn zero; codebook đặc biệt biểu diễn PNS hoặc Intensity Stereo.

Sau khi quantization loop hội tụ, `FDKaacEnc_WriteBitstream()` ghi:

- element/ICS information;
- section data và codebook ID;
- scalefactors;
- pulse/TNS/gain control nếu dùng;
- spectral Huffman codewords;
- PNS/MS/IS side information;
- fill/extension/transport data.

## 16. Tóm tắt theo dữ liệu

```text
PCM16[1024 mẫu mới]
  + psyInputBuffer/history
  + block type/window shape

        Window/MDCT
              ↓
Q31 spectrum[1024] + exponent

        Low-pass + scale/headroom
              ↓
SFB energy + initial threshold

        Tonality + TNS
              ↓
TNS-filtered spectrum + updated energy

        Spreading + PCM floor + pre-echo
              ↓
masking threshold per SFB

        PNS/MS/IS + short grouping
              ↓
PSY_OUT

        PE + bit allocation + threshold adjustment
              ↓
global gain + scalefactors

        nonlinear quantization
              ↓
quantSpec[1024]

        codebook selection + sectioning + Huffman
              ↓
AAC raw_data_block / ADTS access unit
```

