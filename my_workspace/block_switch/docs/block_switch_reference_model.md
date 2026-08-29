# Reference Model & Kiểm chứng khối Block Switching

Tài liệu này mô tả **mô hình tham chiếu (reference model)** viết độc lập cho khối
block switching của FDK-AAC, và bộ harness đối chiếu nó với mã gốc.

Bổ sung cho [block_switch_workflow.md](block_switch_workflow.md) (mô tả thuật toán).
Ở đây trọng tâm là: *làm sao chứng minh ta đã hiểu đúng khối đó*.

---

## 1. Tại sao cần reference model

Harness `tb/test_block_switch.cpp` chỉ **quan sát** DUT: gọi API rồi in kết quả.
Nó cho thấy khối làm gì, nhưng không chứng minh được là ta hiểu đúng — nếu hiểu sai
thuật toán, bảng in ra vẫn "đẹp" như thường.

Reference model giải quyết đúng chỗ đó: viết lại thuật toán **từ đầu, không dùng một
dòng nào của fdk-aac**, rồi bắt hai bên chạy song song trên cùng dữ liệu và so sánh
**toàn bộ trạng thái, từng bit, từng frame**. Nếu hiểu sai một chi tiết, so sánh sẽ
lệch ngay.

Đây cũng chính là quy trình dùng khi viết RTL: model C là vàng, RTL là DUT.

---

## 2. Các file

```
my_workspace/block_switch/
├── ref/
│   ├── ref_block_switch.h      ← API reference model
│   └── ref_block_switch.cpp    ← REF: fixed-point bit-exact + float behavioral
├── tb/
│   ├── verify_block_switch.cpp ← harness đối chiếu DUT vs REF
│   └── test_block_switch.cpp   ← harness quan sát
├── tools/
│   └── dump_wav_block_switch.cpp ← đọc WAV/RAW và xuất kết quả từng frame
├── docs/
│   ├── block_switch_workflow.md          ← thuật toán
│   └── block_switch_reference_model.md   ← file này
└── out/                       ← artefact tự sinh, không phải source
    ├── verify_trace.csv        ← trace từng frame của lần verify
    └── test_signal.raw         ← PCM 16-bit mono để test đường --pcm
```

Build trực tiếp bằng `g++` từ thư mục `my_workspace/block_switch`:

```bash
g++ -O2 -std=c++14 -Wall -Wextra -D_USE_MATH_DEFINES \
  -I../../libAACenc/src -I../../libFDK/include -I../../libSYS/include \
  ../../libAACenc/src/block_switch.cpp \
  ../../libSYS/src/genericStds.cpp \
  ref/ref_block_switch.cpp tb/verify_block_switch.cpp \
  -o verify_block_switch

./verify_block_switch
./verify_block_switch --verbose
./verify_block_switch --pcm out/test_signal.raw
./verify_block_switch --csv out/verify.csv
```

Chỉ cần 4 file nguồn: `block_switch.cpp` (DUT), `genericStds.cpp`,
`ref/ref_block_switch.cpp`, `tb/verify_block_switch.cpp`.

---

## 3. Kiến trúc kiểm chứng — 3 lớp

```
                 PCM 16-bit (cùng một buffer)
                          │
        ┌─────────────────┼─────────────────┐
        ▼                 ▼                 ▼
   ┌─────────┐     ┌─────────────┐   ┌──────────────┐
   │  DUT    │     │ REF fixed   │   │ REF float    │
   │ FDKaac  │     │ RefBs_*     │   │ RefBsF_*     │
   │ Enc_    │     │ int32 Q31   │   │ double       │
   │ Block   │     │ bit-exact   │   │ behavioral   │
   │Switching│     └─────────────┘   └──────────────┘
   └─────────┘            │                 │
        │                 │                 │
        ├──── [A] so sánh TOÀN BỘ trạng thái, bit-exact
        │                                   │
        ├──── [B] so sánh QUYẾT ĐỊNH ───────┘
        │
        └──── [C] kiểm tra BẤT BIẾN (property check)
```

| Lớp | So sánh gì | Ảnh hưởng exit code |
|---|---|---|
| **[A] Bit-exact** | 43 trường của `BLOCK_SWITCHING_CONTROL`: `lastWindowSequence`, `windowShape`, `attack`, `attackIndex`, `lastattack`, `lastAttackIndex`, `noOfGroups`, `groupLen[4]`, `maxWindowNrg`, `accWindowNrg`, `iirStates[2]`, `windowNrg[2][8]`, `windowNrgF[2][8]` | **Có** |
| **[B] Decision** | attack / attackIndex / window sequence / shape / grouping so với mô hình float lý tưởng | Không (thông tin) |
| **[C] Property** | 7 bất biến mà DUT luôn phải thoả | **Có** |

### Bảy bất biến của lớp [C]

| # | Bất biến |
|---|---|
| P1 | Không bao giờ phát ra `WRONG_WINDOW` |
| P2 | Kênh LFE luôn cho `LONG_WINDOW` + `SINE_WINDOW` |
| P3 | Chuyển trạng thái hợp lệ theo ràng buộc overlap: `LONG/STOP → {LONG, START}`, `START/SHORT → {SHORT, STOP}` |
| P4 | `windowShape` khớp đúng bảng `blockType2windowShape[allowShortFrames][seq]` |
| P5 | `attackIndex ∈ [0, nBlockSwitchWindows)` |
| P6 | `noOfGroups = 4 ⟹ Σ groupLen = 8`; `noOfGroups = 1 ⟹ Σ groupLen = 1` |
| P7 | Mọi năng lượng ≥ 0 (không tràn thành số âm) |

### Negative control

Một điểm yếu kinh điển của loại harness này: comparator hỏng thì mọi thứ đều "PASS".
Nên trước khi chạy test, `comparatorSanityCheck()` cố tình bẻ lệch từng trường
(`attackIndex`, `windowNrgF`, `groupLen`) và **yêu cầu comparator phải báo lỗi**.
Nếu nó im lặng → harness tự đánh trượt chính mình.

---

## 4. Đặc tả toán học (mô hình float)

Đây là "spec" của khối, viết ở dạng thuần toán, tương ứng `RefBsF_Process()`:

```
Chuẩn hoá:   s[n] = pcm[n] / 65536                      (không phải /32768 — xem §5.2)

Lọc high-pass IIR bậc 1:
             y[n] = 0.7548·(s[n] − s[n−1]) + 0.5095·y[n−1]

Năng lượng sub-window w (dài L = granuleLength/8):
             E [w] = Σ_{n∈w} s[n]² / 32
             EF[w] = Σ_{n∈w} y[n]² / 32

Trung bình trượt (chạy vắt qua biên frame):
             acc[w] = 0.7·acc[w−1] + 0.3·EF[w−1]

Attack:      ∃w : 0.1·EF[w] > acc[w]     và    max_w EF[w] ≥ minAttackNrg
             attackIndex = w lớn nhất thoả điều kiện

Attack vắt biên frame (khi frame này không attack, frame trước có):
             EF_prev[7] > 10·EF_cur[1]  và  lastAttackIndex = 7
             ⟹ attack = true, attackIndex = 0

Window sequence:
             LC (look-ahead): seq ← chgWndSqLkAhd[lastattack][attack][seq]
             LD (no la)     : seq ← chgWndSq[attack][seq]

Window shape:
             shape ← blockType2windowShape[allowShortFrames][seq]
```

---

## 5. Đặc tả fixed-point (mô hình bit-exact)

### 5.1 Ánh xạ phép toán libFDK

| libFDK | Ngữ nghĩa chính xác | Nguồn |
|---|---|---|
| `FIXP_DBL` | `int32_t`, Q31 (giá trị thực = raw/2³¹) | `common_fix.h:171` |
| `FIXP_SGL` | `int16_t`, Q15 | `common_fix.h:170` |
| `fMultDiv2(a,b)` | `(int32)(((int64)a*b) >> 32)` = a·b/2 | `fixmul.h:131` |
| `fMult(a,b)` | `fMultDiv2(a,b) << 1` = a·b | `fixmul.h:146` |
| `fPow2Div2(a)` | `fMultDiv2(a,a)` = a²/2 | `fixmul.h:277` |
| `fMultAdd(x,a,b)` | `(x + fMultDiv2(a,b)) << 1` = 2x + a·b | `fixmadd.h:251` |
| `FX_SGL2FX_DBL(a)` | `(int32)a << 16` | `common_fix.h:218` |
| `FL2FXCONST_DBL(v)` | `(int32)(v·2³¹ ± 0.5)`, **cắt về 0** (không phải làm tròn), bão hoà | `common_fix.h:191` |
| `FL2FXCONST_SGL(v)` | như trên nhưng ·2¹⁵ | `common_fix.h:179` |

Mọi phép dịch trái trong reference model đi qua `uint32_t` để tránh UB khi tràn —
đúng với hành vi wrap-around mà trình dịch thực tế sinh ra.

### 5.2 Scaling đầu vào

```c
tempUnfiltered = (FIXP_DBL)pcm << (DFRACT_BITS - SAMPLE_BITS - 1);   // << 15
```

Dịch **15** chứ không phải 16: giá trị thực là `pcm/65536`, tức nửa full-scale.
Một bit headroom này để bộ lọc IIR (gain đỉnh > 1) không tràn.

Năng lượng: `nrg += fPow2Div2(x) >> (BLOCK_SWITCH_ENERGY_SHIFT − 1 − 2)`
= `x²/2 >> 4` = `x²/32`, tích luỹ bằng `uint32` rồi bão hoà về `MAXVAL_DBL`.

### 5.3 ⚠ Khối này KHÔNG bit-exact giữa các kiến trúc

Đây là phát hiện quan trọng nhất khi xây model. `block_switch.cpp` có hai nhánh
hằng số:

```c
#ifndef SINETABLE_16BIT
  static const FIXP_DBL hiPassCoeff[2] = {FL2FXCONST_DBL(-0.5095), FL2FXCONST_DBL(0.7548)};
  static const FIXP_DBL oneMinusAccWindowNrgFac = FL2FXCONST_DBL(0.7f);
  static const FIXP_DBL invAttackRatio          = FL2FXCONST_DBL(0.1f);
#else
  static const FIXP_SGL hiPassCoeff[2] = {FL2FXCONST_SGL(-0.5095), FL2FXCONST_SGL(0.7548)};
  static const FIXP_SGL oneMinusAccWindowNrgFac = FL2FXCONST_SGL(0.7f);
  static const FIXP_SGL invAttackRatio          = FL2FXCONST_SGL(0.1f);
#endif
```

`FDK_archdef.h` bật `SINETABLE_16BIT` cho **x86 (kể cả x86-64), ARM, RISC-V,
s390x, sparc**; tắt cho **MIPS, PowerPC**.

Ở nhánh Q15, mọi phép nhân đi qua `fixmuldiv2_SD()` → hệ số được mở rộng
`Q15 << 16` trước khi nhân, tức **16 bit thấp của hệ số bị ép về 0**:

| Hằng số | Nhánh Q31 (MIPS/PPC) | Nhánh Q15 (x86-64/ARM) | Q15 raw |
|---|---|---|---|
| `hiPassCoeff[0]` (−0.5095) | −1 094 142 919 | −1 094 123 520 | −16 695 |
| `hiPassCoeff[1]` (0.7548) | 1 620 920 658 | 1 620 901 888 | 24 733 |
| `oneMinusAccWindowNrgFac` (0.7f) | 1 503 238 528 | 1 503 133 696 | 22 938 |
| `invAttackRatio` (0.1f) | 214 748 368 | 214 761 472 | 3 277 |
| `accWindowNrgFac` (0.3f) | 644 245 120 | 644 245 120 (luôn Q31) | — |
| `minAttackNrg` | 15 625 | 15 625 (luôn Q31) | — |

Sai khác cỡ 10⁻⁵ tương đối, nhưng bộ lọc **hồi tiếp** nên tích luỹ, và
`accWindowNrg` chạy vắt qua các frame nên tích luỹ tiếp nữa. Trên tín hiệu ở sát
ngưỡng, hai kiến trúc có thể ra **window sequence khác nhau** → bitstream khác nhau.

Reference model chọn nhánh bằng `REF_COEFF_16BIT` (tự nhận diện, ghi đè bằng
`-DREF_COEFF_16BIT=0/1`), và harness **kiểm tra chéo lúc chạy** rằng REF và DUT
đang ở cùng nhánh.

### 5.4 Hai cái bẫy khác đã gặp

1. **Hậu tố `f`**: `FL2FXCONST_DBL(0.3f)` = 644 245 120 nhưng `FL2FXCONST_DBL(0.3)`
   = 644 245 094. Macro ép `(double)` *sau khi* literal đã là float, nên sai số của
   `float` được giữ lại. Bỏ sót chữ `f` là lệch ngay.
2. **Ép kiểu cắt về 0, không làm tròn**: `FL2FXCONST_*` cộng/trừ 0.5 rồi ép
   `(LONG)`, mà ép kiểu C cắt về phía 0. Với số âm, kết quả **khác** `round()`.

---

## 6. Bộ test

| Nhóm | Kịch bản | Mục đích |
|---|---|---|
| [1] AAC-LC 1024 mẫu | `silence`, `dc`, `steady_sine`, `white_noise`, `low_dither`, `fullscale_square`, `alt_extreme`, `castanets`, `slow_crescendo`, `dirac_train` | biên độ từ 0 tới full-scale, có/không transient, stress bão hoà |
| [2] Quét attack | `attack_w0` … `attack_w7` | đặt transient vào giữa từng sub-window → phủ hết 8 dòng bảng grouping và 8 giá trị `attackIndex` |
| [3] AAC-LD 512 mẫu | `ld_castanets`, `ld_attack`, `ld_silence` | nhánh 4 sub-window, không look-ahead, có `LOWOV_WINDOW` |
| [4] LFE | `lfe_castanets` | phải luôn LONG/SINE bất kể tín hiệu |
| [5] Stereo | `stereo_common`, `stereo_nocommon` | `FDKaacEnc_SyncBlockSwitching()`, đồng bộ grouping theo `maxWindowNrg` |
| [6] PCM thật | `--pcm file.raw` | chạy trên file PCM 16-bit mono bất kỳ |

Vài kịch bản đáng chú ý:

- `alt_extreme` (±32767 xen kẽ) — ép `windowNrgF` bão hoà tại `MAXVAL_DBL`. Đây là
  trường hợp duy nhất mô hình float và fixed **quyết định khác nhau**.
- `low_dither` (±1.5 LSB) — dưới `minAttackNrg`, kiểm tra cổng năng lượng.
- `slow_crescendo` — biên độ tăng đều, tỉ lệ luôn < 10× → không được coi là attack.
- `dirac_train` — xung cách nhau 1152 mẫu, lệch pha so với biên frame.

---

## 7. Kết quả

```
=== DOI CHIEU HANG SO  DUT vs REF ===
  nhanh he so    DUT=Q15(SINETABLE_16BIT)  REF=Q15(SINETABLE_16BIT)   OK
  minAttackNrg   DUT=15625        REF=15625        OK
  hiPassCoeff[0] DUT=-1094123520  REF=-1094123520  OK
  hiPassCoeff[1] DUT=1620901888   REF=1620901888   OK
  accWindowNrgFac DUT=644245120   REF=644245120    OK
  invAttackRatio DUT=214761472    REF=214761472    OK

=== TU KIEM TRA HARNESS ===
  Negative control (comparator tu kiem tra) : PASS

=== DO PHU ===
  window sequence : LONG(155) START(25) SHORT(36) STOP(22) LOWOV(2)
  attackIndex     : 0 1 2 3 4 5 6 7 (8/8 vi tri)
  dong grouping   : 0 1 2 3 4 5 6 7 (8/8 dong)
  o FSM look-ahead: 10/16 o duoc kich hoat

=== TONG KET ===
  kich ban              : 25
  frame da kiem tra     : 292
  [A] bit-exact  lech   : 0
  [B] decision   lech   : 9
  [C] property   loi    : 0

  KET QUA: *** PASS ***
```

**Đọc kết quả:**

- **[A] = 0 trên 292 frame** → mô hình tham chiếu khớp DUT **từng bit** ở mọi trường
  trạng thái, kể cả các biến nội bộ (`iirStates`, `accWindowNrg`, cả 32 ô năng lượng).
  Nghĩa là thuật toán đã được hiểu và tái hiện đúng hoàn toàn.
- **[C] = 0** → 7 bất biến đều đúng trên toàn bộ kịch bản.
- **[B] = 9** — tất cả nằm ở `alt_extreme`. Không phải lỗi: khi `windowNrgF` bão hoà
  ở `MAXVAL_DBL`, phiên bản fixed-point mất khả năng phân biệt "rất to" với "cực to"
  nên vẫn báo attack, còn mô hình float lý tưởng thì không. **Đây là giới hạn thực sự
  của hiện thực fixed-point**, đo được bằng chính bộ test này.
- **Độ phủ**: đủ 8/8 vị trí attack và 8/8 dòng bảng grouping. FSM look-ahead đạt
  10/16 ô — 6 ô còn lại là tổ hợp không thể tới được từ tín hiệu thật (ví dụ
  `LOWOV_WINDOW` ở nhánh LC, hoặc `SHORT` khi `allowShortFrames = 0`).

---

## 8. Dùng model này để làm gì tiếp

1. **Kiểm chứng RTL**: `RefBs_Process()` chính là golden model. Đưa cùng vector PCM
   vào RTL, so `windowNrgF[]`, `accWindowNrg`, `attack`, `attackIndex`, `seq`.
   `verify_trace.csv` là định dạng trace sẵn sàng để diff.
2. **Đổi tham số an toàn**: muốn thử ngưỡng attack khác 10×, đổi trong REF trước, đo
   trên bộ tín hiệu, rồi mới sửa DUT — có ngay số liệu so sánh.
3. **Kiểm tra port sang kiến trúc khác**: build với `-DREF_COEFF_16BIT=0` để lấy kết
   quả của nhánh MIPS/PowerPC mà không cần máy đó.
4. **Regression**: exit code 0/1, cắm thẳng vào CI.
5. **Thêm tín hiệu**: khai báo một `GenFn` rồi thêm vào mảng `lcCases[]` trong
   `tb/verify_block_switch.cpp`.

---

## 9. Tóm tắt một dòng

Reference model độc lập khớp **bit-exact 292/292 frame** với
`FDKaacEnc_BlockSwitching()` và `FDKaacEnc_SyncBlockSwitching()`; quá trình xây
model làm lộ ra rằng khối này **phụ thuộc kiến trúc** (hệ số Q15 trên x86-64/ARM,
Q31 trên MIPS/PowerPC) và rằng bão hoà fixed-point làm thay đổi quyết định attack ở
tín hiệu cực đại.
