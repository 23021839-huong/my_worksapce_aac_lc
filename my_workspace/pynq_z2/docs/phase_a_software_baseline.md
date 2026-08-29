# Giai đoạn A: build và chạy FDK-AAC software trên Cortex-A9

## 1. Mục tiêu của lượt chạy đầu tiên

Lượt chạy này tạo **software oracle trên chính PYNQ-Z2**, chưa dùng bitstream
hay Programmable Logic. Cấu hình baseline được khóa như sau:

| Thuộc tính | Giá trị |
|---|---|
| CPU | ARM Cortex-A9 của PYNQ-Z2 |
| Backend | FDK-AAC software nguyên bản |
| Audio Object Type | AAC-LC (`AOT=2`) |
| Input | Mono, signed PCM16 WAV, 48 kHz |
| Frame length | 1024 mẫu/kênh |
| Transport | ADTS |
| Bitrate đầu tiên | 64.000 bit/s CBR |
| Afterburner | Bật (`1`) |

Corpus đầu tiên là `input/abc_votay.wav`. Header của file trong repository đã
được kiểm tra: PCM không nén, mono, 48 kHz, 16 bit, dài khoảng 9,64 giây.

Kết thúc bước này phải có:

- binary `aac-enc` là ELF ARM 32-bit và chạy được trên board;
- `test-encode-decode` báo `0 failures`;
- file `software_baseline.aac` không rỗng;
- nếu có FFmpeg, `ffprobe` nhận AAC-LC/48 kHz/mono và file được giải mã lại;
- manifest, log thời gian và SHA-256 của các artifact.

## 2. Chép đúng working tree sang board

Board và máy phát triển phải dùng cùng source revision. Vì working tree hiện tại
có tài liệu và RTL riêng trong `my_workspace`, ưu tiên chép toàn bộ thư mục bằng
VS Code Remote SSH hoặc `scp`, thay vì clone một revision khác từ Internet.

Ví dụ từ PowerShell trên máy phát triển:

```powershell
scp -r "D:\A-lab\AAC\fdk-aac" <user>@<PYNQ_IP>:/home/<user>/
ssh <user>@<PYNQ_IP>
```

Thay `<user>` và `<PYNQ_IP>` theo image/card mạng đang dùng. Sau khi SSH:

```bash
cd ~/fdk-aac
```

Nếu thư mục đã tồn tại trên board, đồng bộ thay đổi có chủ đích; không chép đè
artifact của một lượt đo cũ mà chưa lưu checksum/log.

## 3. Xác nhận đúng target và cài công cụ

Chạy trên PYNQ-Z2:

```bash
uname -m
getconf LONG_BIT
gcc -dumpmachine 2>/dev/null || true
cat /etc/os-release
```

Kết quả target mong đợi là `armv7l`, 32 bit. Tên target GCC native thường chứa
`arm-linux-gnueabihf`, nhưng phải tin kết quả thực tế của image thay vì hardcode
ABI.

Cài bộ công cụ tối thiểu:

```bash
sudo apt update
sudo apt install -y build-essential cmake python3 time
```

Khuyến nghị cài thêm FFmpeg để kiểm tra ADTS độc lập và tạo WAV decode lại:

```bash
sudo apt install -y ffmpeg
```

Không thêm thủ công `-mfpu`, `-mfloat-abi` hoặc sysroot cho native build này.
Compiler mặc định của board đã khớp user space đang chạy; các flag ABI sai có
thể làm binary không chạy được. CMake của dự án yêu cầu phiên bản từ 3.5.1.

## 4. Cách chạy khuyến nghị

Từ root repository trên board:

```bash
bash my_workspace/pynq_z2/sw/scripts/run_phase_a_native.sh
```

Script sẽ:

1. từ chối chạy nếu máy không phải ARMv7;
2. xác nhận WAV là mono/48 kHz/PCM16;
3. configure Release và build static library FDK cùng hai chương trình mẫu;
4. chạy regression encode/decode có sẵn của repository;
5. encode AAC-LC ADTS 64 kb/s, kiểm tra syncword và đo thời gian;
6. dùng FFmpeg/FFprobe nếu đã cài;
7. lưu manifest và checksum vào thư mục timestamp.

Build Cortex-A9 có thể tốn RAM. Script mặc định dùng hai job; nếu bị OOM:

```bash
BUILD_JOBS=1 bash my_workspace/pynq_z2/sw/scripts/run_phase_a_native.sh
```

Để chọn input hoặc thư mục artifact khác:

```bash
bash my_workspace/pynq_z2/sw/scripts/run_phase_a_native.sh \
  /path/to/mono_48k_pcm16.wav \
  /path/to/phase_a_artifacts
```

Không dùng `ALLOW_NON_ARM=1` để nghiệm thu giai đoạn A. Tùy chọn đó chỉ dành
cho dry-run script trên máy phát triển và kết quả không phải ARM oracle.

## 5. Các lệnh tương đương để chạy thủ công

Nếu cần debug từng bước:

```bash
mkdir -p build-pynq-a9
cd build-pynq-a9
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_PROGRAMS=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build . -- -j2
cd ..
```

Kiểm tra binary đúng kiến trúc:

```bash
file build-pynq-a9/aac-enc
readelf -h build-pynq-a9/aac-enc | grep -E 'Class|Machine'
```

Chạy self-test của thư viện:

```bash
./build-pynq-a9/test-encode-decode input/abc_votay.wav
```

Process phải trả exit code 0 và log phải chứa `0 failures`. Test này thử nhiều
mode để phát hiện lỗi build thư viện; command nghiệm thu cấu hình đích vẫn là
AAC-LC ADTS bên dưới:

```bash
/usr/bin/time -v ./build-pynq-a9/aac-enc \
  -t 2 -r 64000 -a 1 \
  input/abc_votay.wav software_baseline.aac
```

Ý nghĩa tham số:

- `-t 2`: AAC-LC;
- `-r 64000`: CBR 64 kb/s;
- `-a 1`: bật afterburner;
- không truyền `-v`, nên không bật VBR;
- chương trình `aac-enc` trong source đã đặt `TT_MP4_ADTS`.

Kiểm tra và giải mã độc lập:

```bash
ffprobe -v error \
  -show_entries stream=codec_name,profile,sample_rate,channels \
  -of default=noprint_wrappers=1 software_baseline.aac

ffmpeg -v error -y -i software_baseline.aac \
  -c:a pcm_s16le software_baseline_decoded.wav

sha256sum input/abc_votay.wav \
  software_baseline.aac software_baseline_decoded.wav
```

## 6. Artifact và tiêu chí PASS

Script tạo cấu trúc:

```text
my_workspace/pynq_z2/artifacts/phase_a/<UTC timestamp>/
├── manifest.txt
├── test-encode-decode.log
├── encode.time.txt
├── software_baseline.aac
├── software_baseline_decoded.wav   # khi có FFmpeg
├── ffprobe.txt                     # khi có FFprobe
├── adts_header.txt
├── input.sha256
└── SHA256SUMS
```

Lượt chạy được xem là PASS khi:

- `manifest.txt` ghi `machine=armv7l` và đúng source revision;
- `test-encode-decode` trả exit code 0 và log chứa `0 failures`;
- `software_baseline.aac` có kích thước lớn hơn 0;
- `adts_header.txt` ghi `adts_syncword=PASS`;
- `ffprobe.txt` ghi `codec_name=aac`, `profile=LC`, 48.000 Hz và một kênh;
- FFmpeg giải mã xong với exit code 0;
- `sha256sum -c SHA256SUMS` trả về toàn bộ `OK`.

Không kỳ vọng WAV decode giống PCM input theo từng sample vì AAC là codec lossy.
Checksum dùng để cố định và tái tạo đúng một baseline, không phải để chứng minh
âm thanh lossless.

## 7. Bước tiếp theo trong giai đoạn A

Sau lượt baseline đầu tiên:

1. giữ lại toàn bộ thư mục artifact và ghi source revision vào báo cáo;
2. bổ sung trace block sequence, window shape, PCM snapshot, spectrum Q31 và
   exponent từ chính binary ARM;
3. chạy regression Block Switching, INPUT, FFT và MDCT hiện có;
4. chỉ chuyển sang giai đoạn B khi software end-to-end và golden ARM đã được
   version hóa.

Script hiện tại hoàn thành phần build/run/smoke test. Việc instrument FDK để
dump trace số học là bước kế tiếp và phải được làm tách biệt để vẫn giữ một
binary software nguyên bản làm đối chứng.
