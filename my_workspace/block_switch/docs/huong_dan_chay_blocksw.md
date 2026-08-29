# Hướng dẫn chạy Block Switching Reference Model bằng VS Code trên Windows

Tài liệu này hướng dẫn tự build và chạy Reference Model Block Switching trong
terminal PowerShell của VS Code với input:

```text
input\abc_votay.wav
```

Golden output được ghi vào:

```text
my_workspace\block_switch\out\abc_votay_blockswitch\
```

Quy trình chính chỉ chạy Reference Model độc lập. Mục 7 là bước tùy chọn để
đối chiếu bit-exact giữa Reference Model và DUT thật của FDK-AAC.

## 1. Mở đúng workspace trong VS Code

Trong VS Code:

1. Chọn **File → Open Folder**.
2. Mở thư mục gốc `fdk-aac`.
3. Chọn **Terminal → New Terminal**.
4. Chọn profile **PowerShell** nếu terminal hiện tại không phải PowerShell.

Từ thư mục gốc repository, vào thư mục Block Switching:

```powershell
Set-Location .\my_workspace\block_switch
Get-Location
```

Đường dẫn phải kết thúc bằng:

```text
\fdk-aac\my_workspace\block_switch
```

Kiểm tra source và input bắt buộc:

```powershell
$requiredFiles = @(
  '.\ref\ref_block_switch.cpp',
  '.\ref\ref_block_switch.h',
  '.\tools\dump_wav_block_switch.cpp',
  '..\..\input\abc_votay.wav'
)

$requiredFiles | ForEach-Object {
  if (Test-Path -LiteralPath $_ -PathType Leaf) {
    "OK: $_"
  } else {
    "MISSING: $_"
  }
}
```

Chỉ tiếp tục khi cả bốn file đều báo `OK`.

## 2. Kiểm tra compiler và input WAV

### 2.1 Kiểm tra `g++`

```powershell
Get-Command g++
g++ --version
```

VS Code và extension C/C++ không tự cung cấp compiler. Nếu PowerShell báo
`g++ is not recognized`, cần cài hoặc cấu hình MinGW/MSYS2 toolchain và thêm thư
mục chứa `g++.exe` vào biến `PATH`, sau đó đóng và mở lại VS Code.

### 2.2 Kiểm tra file WAV

```powershell
Get-Item '..\..\input\abc_votay.wav' |
  Select-Object FullName, Length, LastWriteTime
```

Nếu đã có `ffprobe` trong `PATH`, xem metadata âm thanh:

```powershell
ffprobe -v error -select_streams a:0 `
  -show_entries stream=codec_name,sample_rate,channels,bits_per_sample `
  -of default=noprint_wrappers=1 `
  '..\..\input\abc_votay.wav'
```

Reference dump utility nhận trực tiếp WAV thỏa các điều kiện:

- PCM không nén;
- mono, một kênh;
- signed 16-bit little-endian.

File `abc_votay.wav` hiện tại đã được kiểm tra và có thông số:

```text
codec       = PCM
sample rate = 48000 Hz
channels    = 1
sample size = 16 bit
samples     = 462848
AAC frames  = 452 frame x 1024 mẫu
tail        = 0 mẫu
```

Vì vậy file này được đưa trực tiếp vào Reference Model, không cần chuyển đổi.

Nếu sau này dùng WAV stereo, float hoặc codec nén, có thể chuyển bằng `ffmpeg`:

```powershell
New-Item -ItemType Directory -Force '.\out' | Out-Null

ffmpeg -hide_banner -loglevel error -y `
  -i '..\..\input\abc_votay.wav' `
  -ac 1 -ar 48000 -c:a pcm_s16le `
  '.\out\abc_votay_pcm16_mono.wav'
```

Khi đó, thay input trong lệnh chạy bằng
`.\out\abc_votay_pcm16_mono.wav`.

## 3. Build chương trình dump Reference Model

Đứng tại `my_workspace\block_switch`, chạy:

```powershell
g++ -O2 -std=c++14 -Wall -Wextra `
  -I'.\ref' `
  .\ref\ref_block_switch.cpp `
  .\tools\dump_wav_block_switch.cpp `
  -o .\dump_wav_block_switch.exe

if ($LASTEXITCODE -ne 0) {
  throw "Build dump_wav_block_switch failed with exit code $LASTEXITCODE"
}
```

Lưu ý: dấu backtick `` ` `` phải là ký tự cuối cùng trên dòng, không đặt khoảng
trắng phía sau nó.

Chương trình được build từ:

- `ref/ref_block_switch.cpp`: Reference Model fixed-point độc lập;
- `tools/dump_wav_block_switch.cpp`: đọc WAV, chia frame và ghi output;
- không link `libAACenc\src\block_switch.cpp` trong bước này.

Kiểm tra executable:

```powershell
Get-Item '.\dump_wav_block_switch.exe' |
  Select-Object FullName, Length, LastWriteTime
```

## 4. Chạy với `input\abc_votay.wav`

Tạo thư mục output cha:

```powershell
New-Item -ItemType Directory -Force '.\out' | Out-Null
```

Chạy toàn bộ WAV và lưu log terminal:

```powershell
$dumpOutput = & '.\dump_wav_block_switch.exe' `
  --wav '..\..\input\abc_votay.wav' `
  --out '.\out\abc_votay_blockswitch' 2>&1

$dumpStatus = $LASTEXITCODE
$dumpOutput | Tee-Object -FilePath '.\out\dump_abc_votay.log'

"dump exit code = $dumpStatus"
if ($dumpStatus -ne 0) {
  throw "Reference Model failed with exit code $dumpStatus"
}
```

Kết quả thành công phải có dạng:

```text
OK: dumped 452 frame(s) to .\out\abc_votay_blockswitch
dump exit code = 0
```

Lệnh trên ghi đè nội dung năm file output nếu chúng đã tồn tại, nhưng không xóa
các file khác trong thư mục `out`.

Muốn chạy thử 10 frame đầu:

```powershell
& '.\dump_wav_block_switch.exe' `
  --wav '..\..\input\abc_votay.wav' `
  --out '.\out\abc_votay_first10' `
  --frames 10

"exit code = $LASTEXITCODE"
```

## 5. Các output được tạo

```text
out\
├── dump_abc_votay.log
└── abc_votay_blockswitch\
    ├── input.txt
    ├── out_blocksw.txt
    ├── out_blocksw.csv
    ├── out_window_mdct_control.txt
    └── meta.txt
```

### `input.txt`

Dump toàn bộ PCM thực sự cấp vào Reference Model:

```text
frame sample_in_frame abs_sample pcm_s16
```

| Trường | Ý nghĩa |
|---|---|
| `frame` | Chỉ số frame AAC-LC, bắt đầu từ 0 |
| `sample_in_frame` | Vị trí `0..1023` trong frame |
| `abs_sample` | Vị trí tuyệt đối trong WAV |
| `pcm_s16` | Giá trị PCM signed 16-bit |

File này dùng làm stimulus khi cần đối chiếu C++ với RTL.

### `out_blocksw.txt`

Kết quả dễ đọc cho người, gồm:

- `attack`, `attackIndex`;
- chuỗi cửa sổ `LONG`, `START`, `SHORT`, `STOP`;
- shape `SINE`, `KBD`, `LOL`;
- `noOfGroups`, `groupLen[0..3]`;
- năng lượng thô, năng lượng sau high-pass và nền năng lượng tích lũy.

### `out_blocksw.csv`

Golden output chính của Block Switching Reference Model. Mỗi dòng tương ứng một
frame và có thể đọc bằng PowerShell, Python, Excel hoặc testbench tự động.

| Cột | Ý nghĩa |
|---|---|
| `frame` | Chỉ số frame AAC-LC |
| `attack` | `1` nếu phát hiện transient |
| `attackIndex` | Sub-window chứa transient, từ 0 đến 7 |
| `lastWindowSequence`, `seq` | ID và tên loại cửa sổ |
| `windowShape`, `shape` | ID và tên window shape |
| `noOfGroups`, `groupLen0..3` | Nhóm tám short window |
| `maxWindowNrg`, `accWindowNrg` | Năng lượng cực đại và nền tích lũy |
| `windowNrg0..7` | Năng lượng thô từng sub-window |
| `windowNrgF0..7` | Năng lượng sau bộ lọc high-pass |

### `out_window_mdct_control.txt`

Tín hiệu điều khiển chuyển sang khối Window/MDCT:

```text
frame blockType blockTypeName windowShape windowShapeName noOfGroups groupLen0 groupLen1 groupLen2 groupLen3
```

| Giá trị | Ý nghĩa |
|---|---|
| `blockType=0` | `LONG` |
| `blockType=1` | `START` |
| `blockType=2` | `SHORT` |
| `blockType=3` | `STOP` |
| `windowShape=0` | `SINE` |
| `windowShape=1` | `KBD` |
| `windowShape=2` | `LOL`, chủ yếu cho AAC-LD |

Window/MDCT sử dụng trực tiếp `blockType` và `windowShape`. Grouping được giữ lại
cho các khối psychoacoustic/quantization phía sau.

File này chỉ chứa tín hiệu **điều khiển**. PCM của Window/MDCT đi qua
`psyInputBuffer` và trễ 576 mẫu so với vùng PCM Block Switching vừa phân tích.
Không dùng trực tiếp `input.txt` làm PCM Window/MDCT nếu chưa mô phỏng bộ đệm và
đường trễ này.

### `meta.txt`

Metadata để tái tạo phép chạy:

- source WAV;
- sample rate, số kênh và bit depth;
- tổng số mẫu;
- số frame xử lý;
- số mẫu đuôi không đủ frame;
- loại Reference Model và nhánh hệ số fixed-point.

### `dump_abc_votay.log`

Log stdout/stderr của lần chạy. Log dùng để xác nhận chương trình đã hoàn tất;
trace từng frame nằm trong `out_blocksw.txt` và `out_blocksw.csv`.

## 6. Kiểm tra output trong PowerShell

### 6.1 Liệt kê và kiểm tra metadata

```powershell
Get-ChildItem '.\out\abc_votay_blockswitch' |
  Select-Object Name, Length, LastWriteTime

Get-Content '.\out\abc_votay_blockswitch\meta.txt'
```

Với input hiện tại, metadata phải chứa:

```text
sampleRate=48000
channels=1
bitsPerSample=16
granuleLength=1024
totalSamples=462848
framesDumped=452
usedSamples=462848
droppedTailSamples=0
model=RefBs fixed-point independent reference
```

### 6.2 Kiểm tra số frame

```powershell
$csv = Import-Csv '.\out\abc_votay_blockswitch\out_blocksw.csv'
$control = Get-Content '.\out\abc_votay_blockswitch\out_window_mdct_control.txt' |
  Where-Object { $_ -and -not $_.StartsWith('#') }

"CSV frames     = $($csv.Count)"
"Control frames = $($control.Count)"
```

Kết quả phải là:

```text
CSV frames     = 452
Control frames = 452
```

### 6.3 Xem đầu và cuối trace

```powershell
Get-Content '.\out\abc_votay_blockswitch\out_blocksw.txt' -TotalCount 8

$csv | Select-Object -First 5 |
  Format-Table frame, attack, attackIndex, seq, shape, noOfGroups

$csv | Select-Object -Last 5 |
  Format-Table frame, attack, attackIndex, seq, shape, noOfGroups
```

### 6.4 Thống kê loại cửa sổ và attack

```powershell
$csv |
  Group-Object seq |
  Sort-Object Name |
  Select-Object Name, Count |
  Format-Table -AutoSize

$attacks = $csv | Where-Object { $_.attack -eq '1' }
"attack frames = $($attacks.Count)"

$attacks |
  Select-Object -First 20 frame, attackIndex, seq, shape, accWindowNrg |
  Format-Table -AutoSize
```

### 6.5 Mở output trong VS Code

```powershell
code '.\out\abc_votay_blockswitch\meta.txt'
code '.\out\abc_votay_blockswitch\out_blocksw.csv'
code '.\out\abc_votay_blockswitch\out_window_mdct_control.txt'
```

Nếu `code` không có trong `PATH`, mở các file từ Explorer panel của VS Code.

## 7. Tùy chọn: kiểm tra Reference Model khớp DUT FDK-AAC

Golden output ở mục 4 được sinh hoàn toàn từ Reference Model. Bước này build
harness chứa cả DUT FDK-AAC và Reference Model rồi chạy cùng input để xác nhận
bit-exact.

### 7.1 Build harness kiểm chứng trực tiếp bằng `g++`

```powershell
g++ -O2 -std=c++14 -Wall -Wextra -D_USE_MATH_DEFINES `
  -I'..\..\libAACenc\src' `
  -I'..\..\libFDK\include' `
  -I'..\..\libSYS\include' `
  -I'.\ref' `
  '..\..\libAACenc\src\block_switch.cpp' `
  '..\..\libSYS\src\genericStds.cpp' `
  '.\ref\ref_block_switch.cpp' `
  '.\tb\verify_block_switch.cpp' `
  -o '.\verify_block_switch.exe'

if ($LASTEXITCODE -ne 0) {
  throw "Build verify_block_switch failed with exit code $LASTEXITCODE"
}
```

### 7.2 Chuyển WAV sang RAW cho `verify_block_switch`

`verify_block_switch --pcm` chỉ nhận PCM16 RAW, không nhận WAV:

```powershell
ffmpeg -hide_banner -loglevel error -y `
  -i '..\..\input\abc_votay.wav' `
  -ac 1 -ar 48000 -f s16le `
  '.\out\abc_votay.raw'

if ($LASTEXITCODE -ne 0) {
  throw "ffmpeg conversion failed with exit code $LASTEXITCODE"
}
```

### 7.3 Chạy DUT và Reference Model song song

```powershell
$verifyOutput = & '.\verify_block_switch.exe' `
  --pcm '.\out\abc_votay.raw' 2>&1

$verifyStatus = $LASTEXITCODE
$verifyOutput | Tee-Object -FilePath '.\out\verify_abc_votay.log'

"verify exit code = $verifyStatus"
if ($verifyStatus -ne 0) {
  throw "DUT/reference verification failed with exit code $verifyStatus"
}
```

Điều kiện chấp nhận:

```text
[A] bit-exact  lech   : 0
[C] property   loi    : 0
KET QUA: *** PASS ***
verify exit code = 0
```

Tìm riêng phần input thực trong log:

```powershell
Select-String `
  -Path '.\out\verify_abc_votay.log' `
  -Pattern 'PCM thuc te' `
  -Context 0,10

Get-Content '.\out\verify_abc_votay.log' -Tail 25
```

Lưu ý: `verify_block_switch --csv` chỉ ghi các scenario regression tích hợp,
không ghi frame của file `--pcm`. Golden trace của `abc_votay.wav` nằm trong
`out_blocksw.csv` do chương trình dump Reference Model tạo.

## 8. Quy trình PowerShell ngắn gọn để copy-paste

Đứng tại thư mục gốc `fdk-aac`:

```powershell
Set-Location '.\my_workspace\block_switch'

Get-Command g++ | Out-Null
if (-not (Test-Path '..\..\input\abc_votay.wav')) {
  throw 'Missing input\abc_votay.wav'
}

g++ -O2 -std=c++14 -Wall -Wextra `
  -I'.\ref' `
  '.\ref\ref_block_switch.cpp' `
  '.\tools\dump_wav_block_switch.cpp' `
  -o '.\dump_wav_block_switch.exe'

if ($LASTEXITCODE -ne 0) {
  throw "Build failed with exit code $LASTEXITCODE"
}

New-Item -ItemType Directory -Force '.\out' | Out-Null

$dumpOutput = & '.\dump_wav_block_switch.exe' `
  --wav '..\..\input\abc_votay.wav' `
  --out '.\out\abc_votay_blockswitch' 2>&1

$dumpStatus = $LASTEXITCODE
$dumpOutput | Tee-Object -FilePath '.\out\dump_abc_votay.log'
if ($dumpStatus -ne 0) {
  throw "Reference Model failed with exit code $dumpStatus"
}

$csv = Import-Csv '.\out\abc_votay_blockswitch\out_blocksw.csv'
$meta = Get-Content '.\out\abc_votay_blockswitch\meta.txt'

$meta
"frames = $($csv.Count)"
$csv | Group-Object seq | Select-Object Name, Count | Format-Table -AutoSize
```

Sau khi tự chạy, gửi lại kết quả của:

```powershell
Get-Content '.\out\abc_votay_blockswitch\meta.txt'

Import-Csv '.\out\abc_votay_blockswitch\out_blocksw.csv' |
  Group-Object seq |
  Select-Object Name, Count |
  Format-Table -AutoSize

Import-Csv '.\out\abc_votay_blockswitch\out_blocksw.csv' |
  Where-Object { $_.attack -eq '1' } |
  Select-Object -First 20 frame, attackIndex, seq, shape
```

## 9. Lỗi thường gặp

### `g++ is not recognized`

Kiểm tra:

```powershell
Get-Command g++ -ErrorAction SilentlyContinue
$env:Path -split ';'
```

Nếu không tìm thấy, compiler chưa được cài hoặc thư mục chứa `g++.exe` chưa có
trong `PATH`. Sau khi chỉnh `PATH`, phải mở terminal mới hoặc khởi động lại VS Code.

### PowerShell chờ nhập tiếp với dấu `>>`

Thường do thiếu backtick, backtick có khoảng trắng phía sau, thiếu dấu nháy hoặc
thiếu dấu ngoặc. Nhấn `Ctrl+C`, sau đó copy lại nguyên khối lệnh.

### `No such file or directory` khi build

Đang đứng sai thư mục:

```powershell
Set-Location '<duong-dan-repo>\fdk-aac\my_workspace\block_switch'
Get-Location
Get-ChildItem
```

### `ERROR: cannot read input (rc=-5)`

WAV không phải mono PCM16. Chuyển bằng lệnh `ffmpeg` ở mục 2 rồi chạy lại với
file đã chuyển.

### Output có ít hơn 452 frame

Kiểm tra metadata:

```powershell
Get-Content '.\out\abc_votay_blockswitch\meta.txt' |
  Select-String 'totalSamples|framesDumped|droppedTailSamples'
```

Reference Model chỉ xử lý frame đầy đủ 1024 mẫu. Với đúng file
`input\abc_votay.wav` hiện tại, kết quả phải là 452 frame và không có mẫu đuôi.

## 10. Dọn artifact build sau khi kiểm tra

Chỉ chạy khi đã lưu output cần thiết:

```powershell
$artifacts = @(
  '.\dump_wav_block_switch.exe',
  '.\verify_block_switch.exe',
  '.\out\abc_votay.raw'
)

$artifacts | ForEach-Object {
  if (Test-Path -LiteralPath $_ -PathType Leaf) {
    Remove-Item -LiteralPath $_ -Force
  }
}
```

Đoạn trên chỉ xóa executable và RAW trung gian. Nó giữ nguyên golden output trong
`out\abc_votay_blockswitch`.
