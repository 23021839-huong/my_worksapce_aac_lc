/*
 * verify_block_switch.cpp
 *
 * Harness DOI CHIEU:  DUT (libAACenc/src/block_switch.cpp)  vs
 *                     REF (ref_block_switch.cpp - mo hinh doc lap)
 *
 * Ba lop kiem chung:
 *   [A] BIT-EXACT   : so sanh TOAN BO truong cua BLOCK_SWITCHING_CONTROL
 *                     giua DUT va REF fixed-point, tung frame.
 *   [B] DECISION    : so sanh quyet dinh (attack / attackIndex / window
 *                     sequence / shape / grouping) giua DUT va REF float.
 *   [C] PROPERTY    : kiem tra cac bat bien ma DUT phai luon thoa man
 *                     (chuyen trang thai hop le, tong groupLen, shape mapping...)
 *
 * Chay:
 *   verify_block_switch.exe                 chay toan bo bo test
 *   verify_block_switch.exe --verbose       in bang chi tiet tung frame
 *   verify_block_switch.exe --csv out\verify.csv
 *   verify_block_switch.exe --pcm file.raw  chay tren PCM 16-bit mono thuc te
 *
 * Exit code: 0 = tat ca PASS, 1 = co FAIL.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block_switch.h" /* DUT */
#include "psy_const.h"
#include "../ref/ref_block_switch.h" /* REF */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ==================================================================== */
/* Bien dem ket qua                                                      */
/* ==================================================================== */

typedef struct {
  long framesChecked;
  long bitExactFail;   /* [A] */
  long decisionFail;   /* [B] */
  long propertyFail;   /* [C] */
  long scenarios;
} Stats;

static Stats g_stats;
static int g_verbose = 0;
static FILE *g_csv = NULL;

/* do phu (coverage) */
static int g_covSeq[REF_N_BLOCKTYPES];              /* window sequence da gap */
static int g_covAttackIndex[REF_BLOCK_SWITCH_WINDOWS];
static int g_covFsmLkAhd[2][2][REF_N_BLOCKTYPES];   /* o FSM da kich hoat  */
static int g_covGroupRow[REF_TRANS_FAC];            /* dong bang grouping  */

/* ==================================================================== */
/* [A] So sanh bit-exact toan bo trang thai                              */
/* ==================================================================== */

#define CMP_SCALAR(field)                                                 \
  do {                                                                    \
    if ((long)dut->field != (long)ref->field) {                           \
      if (nDiff < maxMsg)                                                 \
        snprintf(msg[nDiff], msgLen, "%-18s DUT=%-12ld REF=%-12ld", #field, \
                 (long)dut->field, (long)ref->field);                     \
      nDiff++;                                                            \
    }                                                                     \
  } while (0)

#define CMP_ARRAY1(field, n)                                              \
  do {                                                                    \
    for (int _i = 0; _i < (n); _i++) {                                    \
      if ((long)dut->field[_i] != (long)ref->field[_i]) {                 \
        if (nDiff < maxMsg)                                               \
          snprintf(msg[nDiff], msgLen, "%s[%d]  DUT=%-12ld REF=%-12ld",   \
                   #field, _i, (long)dut->field[_i], (long)ref->field[_i]); \
        nDiff++;                                                          \
      }                                                                   \
    }                                                                     \
  } while (0)

#define CMP_ARRAY2(field, n0, n1)                                            \
  do {                                                                       \
    for (int _a = 0; _a < (n0); _a++)                                        \
      for (int _b = 0; _b < (n1); _b++) {                                    \
        if ((long)dut->field[_a][_b] != (long)ref->field[_a][_b]) {          \
          if (nDiff < maxMsg)                                                \
            snprintf(msg[nDiff], msgLen, "%s[%d][%d] DUT=%-12ld REF=%-12ld", \
                     #field, _a, _b, (long)dut->field[_a][_b],               \
                     (long)ref->field[_a][_b]);                              \
          nDiff++;                                                           \
        }                                                                    \
      }                                                                      \
  } while (0)

enum { MAX_DIFF_MSG = 8, DIFF_MSG_LEN = 96 };

static int compareStates(const BLOCK_SWITCHING_CONTROL *dut,
                         const RefBsState *ref,
                         char msg[MAX_DIFF_MSG][DIFF_MSG_LEN]) {
  const int maxMsg = MAX_DIFF_MSG;
  const size_t msgLen = DIFF_MSG_LEN;
  int nDiff = 0;

  CMP_SCALAR(lastWindowSequence);
  CMP_SCALAR(windowShape);
  CMP_SCALAR(lastWindowShape);
  CMP_SCALAR(nBlockSwitchWindows);
  CMP_SCALAR(attack);
  CMP_SCALAR(lastattack);
  CMP_SCALAR(attackIndex);
  CMP_SCALAR(lastAttackIndex);
  CMP_SCALAR(allowShortFrames);
  CMP_SCALAR(allowLookAhead);
  CMP_SCALAR(noOfGroups);
  CMP_SCALAR(maxWindowNrg);
  CMP_SCALAR(accWindowNrg);

  CMP_ARRAY1(groupLen, MAX_NO_OF_GROUPS);
  CMP_ARRAY1(iirStates, BLOCK_SWITCHING_IIR_LEN);
  CMP_ARRAY2(windowNrg, 2, BLOCK_SWITCH_WINDOWS);
  CMP_ARRAY2(windowNrgF, 2, BLOCK_SWITCH_WINDOWS);

  return nDiff;
}

/* ==================================================================== */
/* [B] So sanh quyet dinh voi mo hinh float                              */
/* ==================================================================== */

static int compareDecisions(const BLOCK_SWITCHING_CONTROL *dut,
                            const RefBsStateF *ref, char *msg, size_t msgLen) {
  if (dut->attack != ref->attack) {
    snprintf(msg, msgLen, "attack DUT=%d REF_F=%d", (int)dut->attack,
             ref->attack);
    return 1;
  }
  if (dut->attack && dut->attackIndex != ref->attackIndex) {
    snprintf(msg, msgLen, "attackIndex DUT=%d REF_F=%d", (int)dut->attackIndex,
             ref->attackIndex);
    return 1;
  }
  if (dut->lastWindowSequence != ref->lastWindowSequence) {
    snprintf(msg, msgLen, "seq DUT=%s REF_F=%s",
             RefBs_SeqName(dut->lastWindowSequence),
             RefBs_SeqName(ref->lastWindowSequence));
    return 1;
  }
  if (dut->windowShape != ref->windowShape) {
    snprintf(msg, msgLen, "shape DUT=%s REF_F=%s",
             RefBs_ShapeName(dut->windowShape),
             RefBs_ShapeName(ref->windowShape));
    return 1;
  }
  if (dut->noOfGroups != ref->noOfGroups) {
    snprintf(msg, msgLen, "noOfGroups DUT=%d REF_F=%d", (int)dut->noOfGroups,
             ref->noOfGroups);
    return 1;
  }
  for (int i = 0; i < MAX_NO_OF_GROUPS; i++) {
    if (dut->groupLen[i] != ref->groupLen[i]) {
      snprintf(msg, msgLen, "groupLen[%d] DUT=%d REF_F=%d", i,
               (int)dut->groupLen[i], ref->groupLen[i]);
      return 1;
    }
  }
  return 0;
}

/* ==================================================================== */
/* [C] Bat bien ma DUT phai luon thoa man                                */
/* ==================================================================== */

static int checkProperties(const BLOCK_SWITCHING_CONTROL *bsc, int prevSeq,
                           int isLFE, char *msg, size_t msgLen) {
  /* P1: khong bao gio duoc phat ra WRONG_WINDOW */
  if (bsc->lastWindowSequence == WRONG_WINDOW) {
    snprintf(msg, msgLen, "P1: phat ra WRONG_WINDOW");
    return 1;
  }

  /* P2: LFE luon LONG + SINE */
  if (isLFE) {
    if (bsc->lastWindowSequence != LONG_WINDOW ||
        bsc->windowShape != SINE_WINDOW) {
      snprintf(msg, msgLen, "P2: LFE cho ra seq=%s shape=%s",
               RefBs_SeqName(bsc->lastWindowSequence),
               RefBs_ShapeName(bsc->windowShape));
      return 1;
    }
    return 0;
  }

  /* P3: chuyen trang thai hop le theo rang buoc overlap */
  if (!RefBs_IsLegalTransition(prevSeq, bsc->lastWindowSequence,
                               bsc->allowShortFrames)) {
    snprintf(msg, msgLen, "P3: chuyen %s -> %s khong hop le",
             RefBs_SeqName(prevSeq), RefBs_SeqName(bsc->lastWindowSequence));
    return 1;
  }

  /* P4: window shape phai khop bang blockType2windowShape */
  {
    int expect =
        RefBs_GetWindowShapeFor(bsc->allowShortFrames,
                                bsc->lastWindowSequence);
    if (bsc->windowShape != expect) {
      snprintf(msg, msgLen, "P4: shape=%s, ky vong %s cho seq=%s",
               RefBs_ShapeName(bsc->windowShape), RefBs_ShapeName(expect),
               RefBs_SeqName(bsc->lastWindowSequence));
      return 1;
    }
  }

  /* P5: attackIndex nam trong [0, nBlockSwitchWindows) */
  if (bsc->attackIndex < 0 ||
      bsc->attackIndex >= (INT)bsc->nBlockSwitchWindows) {
    snprintf(msg, msgLen, "P5: attackIndex=%d ngoai [0,%u)",
             (int)bsc->attackIndex, bsc->nBlockSwitchWindows);
    return 1;
  }

  /* P6: khi cho phep short frame, tong groupLen phai = TRANS_FAC */
  if (bsc->allowShortFrames) {
    int sum = 0;
    for (int i = 0; i < MAX_NO_OF_GROUPS; i++) sum += bsc->groupLen[i];
    if (bsc->noOfGroups == MAX_NO_OF_GROUPS && sum != TRANS_FAC) {
      snprintf(msg, msgLen, "P6: sum(groupLen)=%d, ky vong %d", sum, TRANS_FAC);
      return 1;
    }
    if (bsc->noOfGroups == 1 && sum != 1) {
      snprintf(msg, msgLen, "P6: noOfGroups=1 nhung sum(groupLen)=%d", sum);
      return 1;
    }
  }

  /* P7: energy khong am */
  for (int w = 0; w < BLOCK_SWITCH_WINDOWS; w++) {
    if (bsc->windowNrg[THIS_WINDOW][w] < 0 ||
        bsc->windowNrgF[THIS_WINDOW][w] < 0) {
      snprintf(msg, msgLen, "P7: nang luong am tai w=%d", w);
      return 1;
    }
  }

  return 0;
}

/* ==================================================================== */
/* Sinh tin hieu thu                                                     */
/* ==================================================================== */

static unsigned g_rngState = 12345u;
static void rngReset(void) { g_rngState = 12345u; }
static double rngNoise(void) { /* [-1,1) */
  g_rngState = g_rngState * 1103515245u + 12345u;
  return ((double)((g_rngState >> 16) & 0xFFFF) / 32768.0) - 1.0;
}

static INT_PCM clipPcm(double x) {
  if (x > 32767.0) x = 32767.0;
  if (x < -32768.0) x = -32768.0;
  return (INT_PCM)((x < 0) ? (x - 0.5) : (x + 0.5));
}

static void genSilence(INT_PCM *b, int n, int fs, double p) {
  (void)fs;
  (void)p;
  memset(b, 0, n * sizeof(INT_PCM));
}

static void genDC(INT_PCM *b, int n, int fs, double p) {
  (void)fs;
  (void)p;
  for (int i = 0; i < n; i++) b[i] = 16384;
}

static void genSteadySine(INT_PCM *b, int n, int fs, double p) {
  (void)p;
  for (int i = 0; i < n; i++)
    b[i] = clipPcm(16384.0 * sin(2.0 * M_PI * 1000.0 * i / fs));
}

static void genFullScaleSquare(INT_PCM *b, int n, int fs, double p) {
  (void)fs;
  (void)p;
  for (int i = 0; i < n; i++) b[i] = (INT_PCM)(((i / 7) & 1) ? 32767 : -32768);
}

static void genAlternatingExtreme(INT_PCM *b, int n, int fs, double p) {
  (void)fs;
  (void)p;
  for (int i = 0; i < n; i++) b[i] = (INT_PCM)((i & 1) ? 32767 : -32768);
}

static void genWhiteNoise(INT_PCM *b, int n, int fs, double p) {
  (void)fs;
  (void)p;
  rngReset();
  for (int i = 0; i < n; i++) b[i] = clipPcm(30000.0 * rngNoise());
}

static void genLowLevelDither(INT_PCM *b, int n, int fs, double p) {
  (void)fs;
  (void)p;
  rngReset();
  for (int i = 0; i < n; i++) b[i] = clipPcm(1.5 * rngNoise());
}

/* p = vi tri attack tinh theo mau */
static void genAttackAt(INT_PCM *b, int n, int fs, double p) {
  const int pos = (int)p;
  rngReset();
  for (int i = 0; i < n; i++) {
    double s = 655.0 * sin(2.0 * M_PI * 400.0 * i / fs); /* nen -34 dBFS */
    if (i >= pos) {
      double env = exp(-(double)(i - pos) / 300.0);
      s += 29500.0 * env * rngNoise();
    }
    b[i] = clipPcm(s);
  }
}

static void genCastanets(INT_PCM *b, int n, int fs, double p) {
  (void)p;
  rngReset();
  for (int i = 0; i < n; i++) {
    double s = 327.0 * sin(2.0 * M_PI * 300.0 * i / fs);
    double env = exp(-(double)(i % 700) / 120.0);
    s += 26000.0 * env * rngNoise();
    b[i] = clipPcm(s);
  }
}

static void genSlowCrescendo(INT_PCM *b, int n, int fs, double p) {
  (void)p;
  for (int i = 0; i < n; i++) {
    double g = 0.02 + 0.9 * ((double)i / (double)n);
    b[i] = clipPcm(32768.0 * g * sin(2.0 * M_PI * 800.0 * i / fs));
  }
}

static void genDiracTrain(INT_PCM *b, int n, int fs, double p) {
  (void)fs;
  (void)p;
  memset(b, 0, n * sizeof(INT_PCM));
  for (int i = 0; i < n; i += 1024 + 128) b[i] = 32767;
}

typedef void (*GenFn)(INT_PCM *, int, int, double);

typedef struct {
  const char *name;
  GenFn gen;
  double param;
} SigCase;

/* ==================================================================== */
/* Chay 1 kich ban mono: DUT + REF fixed + REF float song song            */
/* ==================================================================== */

static void runScenario(const char *label, GenFn gen, double param,
                        int isLowDelay, int granuleLength, int isLFE,
                        int sampleRate, int nFrames) {
  BLOCK_SWITCHING_CONTROL dut;
  RefBsState ref;
  RefBsStateF refF;

  const int nSamples = granuleLength * nFrames;
  INT_PCM *sig = (INT_PCM *)malloc((size_t)nSamples * sizeof(INT_PCM));
  if (!sig) return;

  gen(sig, nSamples, sampleRate, param);

  FDKaacEnc_InitBlockSwitching(&dut, isLowDelay);
  RefBs_Init(&ref, isLowDelay);
  RefBsF_Init(&refF, isLowDelay);

  g_stats.scenarios++;

  /* trang thai sau INIT phai giong nhau */
  {
    char msg[MAX_DIFF_MSG][DIFF_MSG_LEN];
    int nd = compareStates(&dut, &ref, msg);
    if (nd) {
      g_stats.bitExactFail++;
      printf("  [A] FAIL sau INIT (%s): %d truong lech\n", label, nd);
      for (int i = 0; i < nd && i < MAX_DIFF_MSG; i++)
        printf("      %s\n", msg[i]);
    }
  }

  if (g_verbose) {
    printf(
        "\n--- %s | %s | gran=%d | LFE=%d | %d frame ---\n"
        " fr | atk idx | seq   shape | grp | A  B  C\n"
        "----+---------+-------------+-----+--------\n",
        label, isLowDelay ? "LD" : "LC", granuleLength, isLFE, nFrames);
  }

  int prevSeq = dut.lastWindowSequence;

  for (int f = 0; f < nFrames; f++) {
    const INT_PCM *frame = sig + (size_t)f * granuleLength;

    FDKaacEnc_BlockSwitching(&dut, granuleLength, isLFE, frame);
    RefBs_Process(&ref, granuleLength, isLFE, (const int16_t *)frame);
    RefBsF_Process(&refF, granuleLength, isLFE, (const int16_t *)frame);

    g_stats.framesChecked++;

    /* [A] bit-exact */
    char dmsg[MAX_DIFF_MSG][DIFF_MSG_LEN];
    int nd = compareStates(&dut, &ref, dmsg);
    if (nd) {
      g_stats.bitExactFail++;
      printf("  [A] FAIL %s frame %d: %d truong lech\n", label, f, nd);
      for (int i = 0; i < nd && i < MAX_DIFF_MSG; i++)
        printf("      %s\n", dmsg[i]);
    }

    /* [B] decision vs float */
    char bmsg[128] = "";
    int bfail = compareDecisions(&dut, &refF, bmsg, sizeof(bmsg));
    if (bfail) {
      g_stats.decisionFail++;
      printf("  [B] DIFF %s frame %d: %s\n", label, f, bmsg);
    }

    /* [C] properties */
    char cmsg[128] = "";
    int cfail = checkProperties(&dut, prevSeq, isLFE, cmsg, sizeof(cmsg));
    if (cfail) {
      g_stats.propertyFail++;
      printf("  [C] FAIL %s frame %d: %s\n", label, f, cmsg);
    }

    /* coverage */
    if (!isLFE) {
      if (dut.lastWindowSequence >= 0 &&
          dut.lastWindowSequence < REF_N_BLOCKTYPES)
        g_covSeq[dut.lastWindowSequence]++;
      if (dut.attack && dut.attackIndex >= 0 &&
          dut.attackIndex < REF_BLOCK_SWITCH_WINDOWS)
        g_covAttackIndex[dut.attackIndex]++;
      if (dut.allowLookAhead && prevSeq < REF_N_BLOCKTYPES)
        g_covFsmLkAhd[dut.lastattack ? 1 : 0][dut.attack ? 1 : 0][prevSeq]++;
      if (dut.allowShortFrames && dut.lastAttackIndex >= 0 &&
          dut.lastAttackIndex < REF_TRANS_FAC)
        g_covGroupRow[dut.lastAttackIndex]++;
    }

    if (g_verbose) {
      printf(" %2d |  %d   %d  | %s %s | %d%d%d%d | %s %s %s\n", f,
             (int)dut.attack, (int)dut.attackIndex,
             RefBs_SeqName(dut.lastWindowSequence),
             RefBs_ShapeName(dut.windowShape), (int)dut.groupLen[0],
             (int)dut.groupLen[1], (int)dut.groupLen[2], (int)dut.groupLen[3],
             nd ? "X" : ".", bfail ? "X" : ".", cfail ? "X" : ".");
    }

    if (g_csv) {
      fprintf(g_csv, "%s,%d,%d,%d,%d,%s,%s,%d,%ld,%d,%d,%d\n", label, f,
              (int)dut.attack, (int)dut.attackIndex,
              (int)dut.lastWindowSequence,
              RefBs_SeqName(dut.lastWindowSequence),
              RefBs_ShapeName(dut.windowShape), (int)dut.noOfGroups,
              (long)dut.accWindowNrg, nd, bfail, cfail);
    }

    prevSeq = dut.lastWindowSequence;
  }

  free(sig);
}

/* ==================================================================== */
/* Kich ban stereo: kiem chung FDKaacEnc_SyncBlockSwitching              */
/* ==================================================================== */

static void runStereoScenario(const char *label, int commonWindow,
                              int sampleRate, int nFrames) {
  BLOCK_SWITCHING_CONTROL dutL, dutR;
  RefBsState refL, refR;
  const int gran = 1024;
  const int nSamples = gran * nFrames;

  INT_PCM *L = (INT_PCM *)malloc((size_t)nSamples * sizeof(INT_PCM));
  INT_PCM *R = (INT_PCM *)malloc((size_t)nSamples * sizeof(INT_PCM));
  if (!L || !R) {
    free(L);
    free(R);
    return;
  }

  genAttackAt(L, nSamples, sampleRate, 3.5 * 1024);
  genCastanets(R, nSamples, sampleRate, 0);

  FDKaacEnc_InitBlockSwitching(&dutL, 0);
  FDKaacEnc_InitBlockSwitching(&dutR, 0);
  RefBs_Init(&refL, 0);
  RefBs_Init(&refR, 0);

  g_stats.scenarios++;

  if (g_verbose)
    printf(
        "\n--- %s (commonWindow=%d) ---\n"
        " fr | L sau sync | R sau sync | grpL    | grpR    | A\n"
        "----+------------+------------+---------+---------+---\n",
        label, commonWindow);

  for (int f = 0; f < nFrames; f++) {
    const INT_PCM *fl = L + (size_t)f * gran;
    const INT_PCM *fr = R + (size_t)f * gran;

    FDKaacEnc_BlockSwitching(&dutL, gran, 0, fl);
    FDKaacEnc_BlockSwitching(&dutR, gran, 0, fr);
    RefBs_Process(&refL, gran, 0, (const int16_t *)fl);
    RefBs_Process(&refR, gran, 0, (const int16_t *)fr);

    int eD = FDKaacEnc_SyncBlockSwitching(&dutL, &dutR, 2, commonWindow);
    int eR = RefBs_Sync(&refL, &refR, 2, commonWindow);

    g_stats.framesChecked++;

    if (eD != eR) {
      g_stats.bitExactFail++;
      printf("  [A] FAIL %s frame %d: ma loi Sync DUT=%d REF=%d\n", label, f,
             eD, eR);
    }

    char m1[MAX_DIFF_MSG][DIFF_MSG_LEN], m2[MAX_DIFF_MSG][DIFF_MSG_LEN];
    int n1 = compareStates(&dutL, &refL, m1);
    int n2 = compareStates(&dutR, &refR, m2);
    if (n1 || n2) {
      g_stats.bitExactFail++;
      printf("  [A] FAIL %s frame %d sau Sync: L=%d lech, R=%d lech\n", label,
             f, n1, n2);
      for (int i = 0; i < n1 && i < MAX_DIFF_MSG; i++)
        printf("      L: %s\n", m1[i]);
      for (int i = 0; i < n2 && i < MAX_DIFF_MSG; i++)
        printf("      R: %s\n", m2[i]);
    }

    /* sau sync voi commonWindow, hai kenh phai cung window sequence */
    if (commonWindow && dutL.lastWindowSequence != dutR.lastWindowSequence) {
      g_stats.propertyFail++;
      printf("  [C] FAIL %s frame %d: commonWindow nhung L=%s R=%s\n", label, f,
             RefBs_SeqName(dutL.lastWindowSequence),
             RefBs_SeqName(dutR.lastWindowSequence));
    }

    if (g_verbose)
      printf(" %2d |   %s    |   %s    | %d%d%d%d    | %d%d%d%d    | %s\n", f,
             RefBs_SeqName(dutL.lastWindowSequence),
             RefBs_SeqName(dutR.lastWindowSequence), (int)dutL.groupLen[0],
             (int)dutL.groupLen[1], (int)dutL.groupLen[2],
             (int)dutL.groupLen[3], (int)dutR.groupLen[0],
             (int)dutR.groupLen[1], (int)dutR.groupLen[2],
             (int)dutR.groupLen[3], (n1 || n2) ? "X" : ".");
  }

  free(L);
  free(R);
}

/* ==================================================================== */
/* Negative control: chung minh bo so sanh THUC SU phat hien duoc lech   */
/* ==================================================================== */

static int comparatorSanityCheck(void) {
  BLOCK_SWITCHING_CONTROL dut;
  RefBsState ref;
  char msg[MAX_DIFF_MSG][DIFF_MSG_LEN];
  int ok = 1;

  FDKaacEnc_InitBlockSwitching(&dut, 0);
  RefBs_Init(&ref, 0);

  if (compareStates(&dut, &ref, msg) != 0) {
    printf("  sanity: trang thai INIT da lech san -> comparator/model sai\n");
    ok = 0;
  }

  /* co tinh lam lech tung truong -> comparator phai bao */
  ref.attackIndex ^= 1;
  if (compareStates(&dut, &ref, msg) == 0) {
    printf("  sanity: comparator KHONG phat hien lech attackIndex\n");
    ok = 0;
  }
  ref.attackIndex ^= 1;

  ref.windowNrgF[1][5] += 1;
  if (compareStates(&dut, &ref, msg) == 0) {
    printf("  sanity: comparator KHONG phat hien lech windowNrgF\n");
    ok = 0;
  }
  ref.windowNrgF[1][5] -= 1;

  ref.groupLen[2] += 1;
  if (compareStates(&dut, &ref, msg) == 0) {
    printf("  sanity: comparator KHONG phat hien lech groupLen\n");
    ok = 0;
  }
  ref.groupLen[2] -= 1;

  if (compareStates(&dut, &ref, msg) != 0) {
    printf("  sanity: khoi phuc trang thai that bai\n");
    ok = 0;
  }

  printf("  Negative control (comparator tu kiem tra) : %s\n",
         ok ? "PASS" : "FAIL");
  return ok;
}

/* ==================================================================== */

/* He so "hieu dung" ma DUT thuc su dung, tinh theo dung nhanh #ifdef cua
 * block_switch.cpp. Voi nhanh Q15, FDK mo rong Q15 -> Q31 bang
 * FX_SGL2FX_DBL (dich trai 16) truoc khi nhan. */
#ifdef SINETABLE_16BIT
#define DUT_COEF16 1
/* tuong duong FX_SGL2FX_DBL nhung di qua uint32 de khong dich trai so am */
#define DUT_COEF(v) \
  ((long)(int32_t)((uint32_t)(int32_t)(SHORT)FL2FXCONST_SGL(v) << 16))
#else
#define DUT_COEF16 0
#define DUT_COEF(v) ((long)(LONG)FL2FXCONST_DBL(v))
#endif

static int printConstantCheck(void) {
  int ok = 1;

  const long dutMinAttack = (long)(LONG)(
      FL2FXCONST_DBL(1e+6f * NORM_PCM_ENERGY) >> BLOCK_SWITCH_ENERGY_SHIFT);
  const long dutC0 = DUT_COEF(-0.5095);
  const long dutC1 = DUT_COEF(0.7548);
  const long dutAcc = (long)(LONG)FL2FXCONST_DBL(0.3f);
  const long dutInvRatio = DUT_COEF(0.1f);

  printf("=== DOI CHIEU HANG SO  DUT vs REF ===\n");
  printf("  nhanh he so    DUT=%s  REF=%s   %s\n",
         DUT_COEF16 ? "Q15(SINETABLE_16BIT)" : "Q31",
         RefBs_UsesCoeff16() ? "Q15(SINETABLE_16BIT)" : "Q31",
         (DUT_COEF16 == RefBs_UsesCoeff16())
             ? "OK"
             : "MISMATCH -> build lai REF voi -DREF_COEFF_16BIT=?");
  if (DUT_COEF16 != RefBs_UsesCoeff16()) ok = 0;

#define CHK(name, d, r)                                                    \
  do {                                                                     \
    int _same = ((long)(d) == (long)(r));                                  \
    printf("  %-14s DUT=%-12ld REF=%-12ld %s\n", name, (long)(d), (long)(r), \
           _same ? "OK" : "MISMATCH");                                     \
    if (!_same) ok = 0;                                                    \
  } while (0)

  CHK("minAttackNrg", dutMinAttack, RefBs_GetMinAttackNrg());
  CHK("hiPassCoeff[0]", dutC0, RefBs_GetHiPassCoeff(0));
  CHK("hiPassCoeff[1]", dutC1, RefBs_GetHiPassCoeff(1));
  CHK("accWindowNrgFac", dutAcc, RefBs_GetAccWindowNrgFac());
  CHK("invAttackRatio", dutInvRatio, RefBs_GetInvAttackRatio());
#undef CHK

  printf(
      "  BLOCK_SWITCH_WINDOWS=%d  ENERGY_SHIFT=%d  MAX_NO_OF_GROUPS=%d"
      "  SAMPLE_BITS=%d  DFRACT_BITS=%d\n\n",
      BLOCK_SWITCH_WINDOWS, BLOCK_SWITCH_ENERGY_SHIFT, MAX_NO_OF_GROUPS,
      SAMPLE_BITS, DFRACT_BITS);

  return ok;
}

static void printCoverage(void) {
  printf("\n=== DO PHU ===\n  window sequence : ");
  for (int i = 0; i < REF_N_BLOCKTYPES; i++)
    if (g_covSeq[i]) printf("%s(%d) ", RefBs_SeqName(i), g_covSeq[i]);

  printf("\n  attackIndex     : ");
  int nIdx = 0;
  for (int i = 0; i < REF_BLOCK_SWITCH_WINDOWS; i++)
    if (g_covAttackIndex[i]) {
      printf("%d ", i);
      nIdx++;
    }
  printf("(%d/8 vi tri)", nIdx);

  printf("\n  dong grouping   : ");
  int nGrp = 0;
  for (int i = 0; i < REF_TRANS_FAC; i++)
    if (g_covGroupRow[i]) {
      printf("%d ", i);
      nGrp++;
    }
  printf("(%d/8 dong)", nGrp);

  printf("\n  o FSM look-ahead: ");
  int nFsm = 0, nFsmReach = 0;
  for (int a = 0; a < 2; a++)
    for (int b = 0; b < 2; b++)
      for (int s = 0; s < 4; s++) { /* chi 4 blocktype thuc su dung o LC */
        nFsm++;
        if (g_covFsmLkAhd[a][b][s]) nFsmReach++;
      }
  printf("%d/%d o duoc kich hoat\n", nFsmReach, nFsm);
}

/* ==================================================================== */

int main(int argc, char **argv) {
  const char *pcmPath = NULL;
  const char *csvPath = NULL;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--verbose"))
      g_verbose = 1;
    else if (!strcmp(argv[i], "--pcm") && i + 1 < argc)
      pcmPath = argv[++i];
    else if (!strcmp(argv[i], "--csv") && i + 1 < argc)
      csvPath = argv[++i];
    else {
      printf("Cach dung: %s [--verbose] [--pcm raw16.pcm] [--csv f.csv]\n",
             argv[0]);
      return 2;
    }
  }

  memset(&g_stats, 0, sizeof(g_stats));

  if (csvPath) {
    g_csv = fopen(csvPath, "w");
    if (g_csv)
      fprintf(g_csv,
              "case,frame,attack,attackIndex,seqId,seq,shape,noOfGroups,"
              "accWindowNrg,bitExactDiff,decisionDiff,propertyFail\n");
  }

  printf(
      "############################################################\n"
      "#  KIEM CHUNG KHOI BLOCK SWITCHING                          #\n"
      "#  DUT = libAACenc/src/block_switch.cpp                     #\n"
      "#  REF = ref_block_switch.cpp (mo hinh doc lap)             #\n"
      "############################################################\n\n");

  int constOk = printConstantCheck();

  printf("=== TU KIEM TRA HARNESS ===\n");
  int sanityOk = comparatorSanityCheck();
  printf("\n");

  /* ---------------- AAC-LC, 1024 mau ---------------- */
  const SigCase lcCases[] = {
      {"silence", genSilence, 0},
      {"dc", genDC, 0},
      {"steady_sine", genSteadySine, 0},
      {"white_noise", genWhiteNoise, 0},
      {"low_dither", genLowLevelDither, 0},
      {"fullscale_square", genFullScaleSquare, 0},
      {"alt_extreme", genAlternatingExtreme, 0},
      {"castanets", genCastanets, 0},
      {"slow_crescendo", genSlowCrescendo, 0},
      {"dirac_train", genDiracTrain, 0},
  };
  const int nLc = (int)(sizeof(lcCases) / sizeof(lcCases[0]));

  printf("=== [1] AAC-LC mono, 1024 mau/frame, 12 frame ===\n");
  for (int i = 0; i < nLc; i++)
    runScenario(lcCases[i].name, lcCases[i].gen, lcCases[i].param, 0, 1024, 0,
                48000, 12);

  /* quet vi tri attack qua tung sub-window -> phu het bang grouping */
  printf("\n=== [2] Quet vi tri attack qua 8 sub-window ===\n");
  for (int w = 0; w < 8; w++) {
    char nm[32];
    snprintf(nm, sizeof(nm), "attack_w%d", w);
    /* dat attack o giua sub-window w cua frame 3 */
    double pos = 3.0 * 1024.0 + (w + 0.5) * 128.0;
    runScenario(nm, genAttackAt, pos, 0, 1024, 0, 48000, 10);
  }

  /* ---------------- AAC-LD, 512 mau ---------------- */
  printf("\n=== [3] AAC-LD mono, 512 mau/frame ===\n");
  runScenario("ld_castanets", genCastanets, 0, 1, 512, 0, 48000, 16);
  runScenario("ld_attack", genAttackAt, 3.5 * 512, 1, 512, 0, 48000, 16);
  runScenario("ld_silence", genSilence, 0, 1, 512, 0, 48000, 8);

  /* ---------------- LFE ---------------- */
  printf("\n=== [4] LFE (phai luon LONG/SINE) ===\n");
  runScenario("lfe_castanets", genCastanets, 0, 0, 1024, 1, 48000, 8);

  /* ---------------- Stereo sync ---------------- */
  printf("\n=== [5] Stereo + SyncBlockSwitching ===\n");
  runStereoScenario("stereo_common", 1, 48000, 12);
  runStereoScenario("stereo_nocommon", 0, 48000, 12);

  /* ---------------- PCM thuc te (tuy chon) ---------------- */
  if (pcmPath) {
    FILE *fp = fopen(pcmPath, "rb");
    if (!fp) {
      printf("\n!! Khong mo duoc PCM '%s'\n", pcmPath);
    } else {
      fseek(fp, 0, SEEK_END);
      long bytes = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      int nFrames = (int)(bytes / (2 * 1024));
      if (nFrames > 0) {
        INT_PCM *buf =
            (INT_PCM *)malloc((size_t)nFrames * 1024 * sizeof(INT_PCM));
        if (buf && fread(buf, sizeof(INT_PCM), (size_t)nFrames * 1024, fp) ==
                       (size_t)nFrames * 1024) {
          printf("\n=== [6] PCM thuc te: %s (%d frame) ===\n", pcmPath,
                 nFrames);

          BLOCK_SWITCHING_CONTROL dut;
          RefBsState ref;
          FDKaacEnc_InitBlockSwitching(&dut, 0);
          RefBs_Init(&ref, 0);
          g_stats.scenarios++;
          int prevSeq = dut.lastWindowSequence;

          for (int f = 0; f < nFrames; f++) {
            FDKaacEnc_BlockSwitching(&dut, 1024, 0, buf + (size_t)f * 1024);
            RefBs_Process(&ref, 1024, 0,
                          (const int16_t *)(buf + (size_t)f * 1024));
            g_stats.framesChecked++;

            char m[MAX_DIFF_MSG][DIFF_MSG_LEN];
            int nd = compareStates(&dut, &ref, m);
            if (nd) {
              g_stats.bitExactFail++;
              printf("  [A] FAIL pcm frame %d: %d truong lech\n", f, nd);
              for (int k = 0; k < nd && k < MAX_DIFF_MSG; k++)
                printf("      %s\n", m[k]);
            }
            char cm[128];
            if (checkProperties(&dut, prevSeq, 0, cm, sizeof(cm))) {
              g_stats.propertyFail++;
              printf("  [C] FAIL pcm frame %d: %s\n", f, cm);
            }
            prevSeq = dut.lastWindowSequence;
          }
          printf("  da kiem tra %d frame tu file PCM\n", nFrames);
        }
        free(buf);
      }
      fclose(fp);
    }
  }

  printCoverage();

  printf(
      "\n=== TONG KET ===\n"
      "  kich ban              : %ld\n"
      "  frame da kiem tra     : %ld\n"
      "  [A] bit-exact  lech   : %ld\n"
      "  [B] decision   lech   : %ld   (mo hinh float, sai lech nho la binh "
      "thuong)\n"
      "  [C] property   loi    : %ld\n",
      g_stats.scenarios, g_stats.framesChecked, g_stats.bitExactFail,
      g_stats.decisionFail, g_stats.propertyFail);

  int fail = (g_stats.bitExactFail != 0) || (g_stats.propertyFail != 0) ||
             !sanityOk || !constOk;

  printf("\n  KET QUA: %s\n",
         fail ? "*** FAIL ***"
              : "*** PASS *** (REF khop bit-exact voi DUT, moi bat bien thoa)");

  if (g_csv) {
    fclose(g_csv);
    printf("  CSV: %s\n", csvPath);
  }

  return fail ? 1 : 0;
}
