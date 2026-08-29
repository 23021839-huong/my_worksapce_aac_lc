# Block Switching — doc + harness test

Workspace tách riêng để tìm hiểu khối `libAACenc/src/block_switch.cpp`.
Không sửa gì trong source gốc của fdk-aac.

```
my_workspace/block_switch/
├── README.md                            ← file này
├── docs/
│   ├── block_switch_workflow.md         ← TÀI LIỆU: input/output/workflow chi tiết
│   ├── block_switch_reference_model.md  ← TÀI LIỆU: reference model + kiểm chứng
│   └── huong_dan_chay_blocksw.md        ← hướng dẫn chạy trên dữ liệu WAV/RAW
├── ref/
│   └── ref_block_switch.{h,cpp}         ← REFERENCE MODEL độc lập
├── tb/
│   ├── test_block_switch.cpp            ← harness QUAN SÁT
│   └── verify_block_switch.cpp          ← harness ĐỐI CHIẾU DUT vs REF
├── tools/
│   └── dump_wav_block_switch.cpp        ← đọc WAV/RAW và xuất control từng frame
└── out/                                 ← artefact tự sinh, không phải source
    ├── block_switch_trace.csv           ← trace của harness quan sát
    ├── verify_trace.csv                 ← trace của harness đối chiếu
    └── test_signal.raw                  ← PCM 16-bit mono mẫu (cho --pcm)
```

Hai harness phục vụ hai mục đích khác nhau:

| | `tb/test_block_switch.cpp` | `tb/verify_block_switch.cpp` |
|---|---|---|
| Kiểu | quan sát | đối chiếu |
| Trả lời | "khối này làm gì?" | "ta hiểu đúng khối này không?" |
| So sánh với | (không) | reference model độc lập, bit-exact |
| Exit code | luôn 0 | 0 = PASS, 1 = FAIL |

## Build & chạy — harness đối chiếu (reference model)

Đứng tại thư mục `my_workspace/block_switch`:

```bash
g++ -O2 -std=c++14 -Wall -Wextra -D_USE_MATH_DEFINES \
  -I../../libAACenc/src -I../../libFDK/include -I../../libSYS/include \
  ../../libAACenc/src/block_switch.cpp \
  ../../libSYS/src/genericStds.cpp \
  ref/ref_block_switch.cpp tb/verify_block_switch.cpp \
  -o verify_block_switch

./verify_block_switch --verbose
./verify_block_switch --pcm out/test_signal.raw
```

Kết quả hiện tại: **PASS — 0 lệch bit-exact trên 292 frame / 25 kịch bản**.
Chi tiết: [docs/block_switch_reference_model.md](docs/block_switch_reference_model.md).

## Build & chạy — harness quan sát

```bash
g++ -O2 -std=c++14 -D_USE_MATH_DEFINES \
  -I../../libAACenc/src -I../../libFDK/include -I../../libSYS/include \
  ../../libAACenc/src/block_switch.cpp \
  ../../libSYS/src/genericStds.cpp \
  tb/test_block_switch.cpp -o block_switch_test

./block_switch_test
./block_switch_test --no-csv
./block_switch_test --csv out/block_switch_trace.csv
```

Chỉ cần đúng **2 file nguồn của thư viện**: `block_switch.cpp` (khối cần test) và
`genericStds.cpp` (cho `FDKmemclear`/`FDKmemcpy`). Toàn bộ toán fixed-point
(`fMult`, `fPow2Div2`, `fixMax`…) là inline trong header nên không phải link `libFDK`.

## Harness làm gì

Mỗi kịch bản: sinh PCM 16-bit → gọi `FDKaacEnc_InitBlockSwitching()` một lần → gọi
`FDKaacEnc_BlockSwitching()` cho từng frame → in toàn bộ output của
`BLOCK_SWITCHING_CONTROL`.

| Kịch bản | Tín hiệu | Kỳ vọng |
|---|---|---|
| `silence` | im lặng | luôn `LONG` (dưới `minAttackNrg`) |
| `steady_sine` | sine 1 kHz −6 dBFS | `LONG` ổn định (trừ 1 attack giả ở frame 0 do cold start) |
| `single_attack` | nền nhỏ + 1 transient tại mẫu 3584 | `LONG → START → SHORT → STOP → LONG`, `attackIndex = 4` |
| `castanets` | transient lặp lại chu kỳ 700 mẫu | attack **rải rác** (frame 0, 6, 7…): mức nền `accWindowNrg` bị chính các cú đánh đẩy lên cao nên không phải cú nào cũng vượt 10× |
| `slow_crescendo` | biên độ tăng tuyến tính | **không** attack (tỉ lệ < 10×), trừ frame 0 do cold start |

Chạy ở 3 cấu hình: **AAC-LC** (1024 mẫu, 8 sub-window), **AAC-LD** (512 mẫu, 4 sub-window),
và **LFE** (bỏ qua phân tích). Cuối cùng có 2 kịch bản stereo cho
`FDKaacEnc_SyncBlockSwitching()` với `commonWindow = 1` và `= 0`.

Riêng `single_attack` in thêm bảng năng lượng từng sub-window kèm bar ASCII (thang log, 6 dB/ký tự)
để nhìn thấy bậc nhảy năng lượng tại điểm attack.

## Đọc CSV

`out/block_switch_trace.csv`, mỗi dòng = 1 frame:

```
case, mode, frame, attack, attackIndex, seqId, seq, shape, noOfGroups,
groupLen0..3, accWindowNrg, nrg0..nrg7, nrgF0..nrgF7
```

`nrg*`/`nrgF*` là `windowNrg[THIS_WINDOW][w]` / `windowNrgF[THIS_WINDOW][w]` ở dạng số nguyên Q31
(chia cho 2³¹ để ra giá trị thực). Ở chế độ LD chỉ 4 cột đầu có nghĩa.

## Muốn thử tín hiệu của mình

Thêm một hàm sinh tín hiệu trong `tb/test_block_switch.cpp` và đăng ký vào mảng `signalCases[]`:

```c
static void genMyCase(INT_PCM *buf, int n, int sampleRate) { /* ... */ }

static const SIGNAL_CASE signalCases[] = {
    ...
    {"my_case", "mô tả", genMyCase},
};
```

Để nạp file WAV thật, đọc PCM 16-bit mono vào buffer rồi gọi `runMonoCase()` với buffer đó
(có thể dùng `wavreader.c` ở thư mục gốc repo).
