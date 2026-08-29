/*
 * test_block_switch.cpp
 *
 * Standalone harness for libAACenc/src/block_switch.cpp
 *
 * Muc dich: cho thay ro INPUT (PCM 16-bit) va OUTPUT (BLOCK_SWITCHING_CONTROL)
 * cua khoi block switching, khong can chay ca encoder.
 *
 * Build: xem build.ps1 trong cung thu muc.
 * Chay:  .\block_switch_test.exe            (in bang ra man hinh + ghi CSV vao .\out)
 *        .\block_switch_test.exe --no-csv   (chi in ra man hinh)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
/* nothing */
}

#include "block_switch.h"
#include "psy_const.h"

/* ------------------------------------------------------------------ */
/* Tien ich hien thi                                                    */
/* ------------------------------------------------------------------ */

static const char *windowSequenceName(INT ws) {
  switch (ws) {
    case LONG_WINDOW:
      return "LONG ";
    case START_WINDOW:
      return "START";
    case SHORT_WINDOW:
      return "SHORT";
    case STOP_WINDOW:
      return "STOP ";
    case _LOWOV_WINDOW:
      return "LOWOV";
    case WRONG_WINDOW:
      return "WRONG";
    default:
      return "?????";
  }
}

static const char *windowShapeName(INT sh) {
  switch (sh) {
    case SINE_WINDOW:
      return "SINE";
    case KBD_WINDOW:
      return "KBD ";
    case LOL_WINDOW:
      return "LOL ";
    default:
      return "????";
  }
}

/* FIXP_DBL la so fixed point Q31 -> doi ve float trong [-1,1) */
static double q31(FIXP_DBL v) { return (double)(LONG)v / 2147483648.0; }

/* dB tuong doi so voi full-scale cua bien tich luy nang luong */
static double q31_dB(FIXP_DBL v) {
  double x = q31(v);
  if (x <= 0.0) return -999.0;
  return 10.0 * log10(x);
}

/* Ve mot thanh bar ASCII theo thang log de de "nhin thay" nang luong */
static void energyBar(char *dst, int dstLen, FIXP_DBL v) {
  double db = q31_dB(v);          /* thuong nam trong khoang -120..0 dB */
  int n = (int)((db + 120.0) / 6); /* 1 ky tu ~ 6 dB */
  if (n < 0) n = 0;
  if (n > dstLen - 1) n = dstLen - 1;
  for (int i = 0; i < n; i++) dst[i] = '#';
  dst[n] = 0;
}

/* ------------------------------------------------------------------ */
/* Sinh tin hieu thu (INPUT)                                            */
/* ------------------------------------------------------------------ */

static INT_PCM clipToPcm(double x) {
  if (x > 32767.0) x = 32767.0;
  if (x < -32768.0) x = -32768.0;
  return (INT_PCM)((x < 0) ? (x - 0.5) : (x + 0.5));
}

/* Im lang tuyet doi */
static void genSilence(INT_PCM *buf, int n, int /*sampleRate*/) {
  memset(buf, 0, n * sizeof(INT_PCM));
}

/* Sine 1 kHz on dinh, -6 dBFS */
static void genSteadySine(INT_PCM *buf, int n, int sampleRate) {
  const double amp = 32768.0 * 0.5;
  const double f = 1000.0;
  for (int i = 0; i < n; i++)
    buf[i] = clipToPcm(amp * sin(2.0 * M_PI * f * i / sampleRate));
}

/* Nen yen tinh + 1 cu "danh" (transient) o giua -> phai sinh ra attack */
static void genSingleAttack(INT_PCM *buf, int n, int sampleRate) {
  const double bedAmp = 32768.0 * 0.02; /* nen rat nho, -34 dBFS */
  const double f = 400.0;
  /* dat cu danh o mau 3.5 * 1024 = giua frame 3 */
  const int attackPos = (int)(3.5 * 1024);
  for (int i = 0; i < n; i++) {
    double s = bedAmp * sin(2.0 * M_PI * f * i / sampleRate);
    if (i >= attackPos) {
      /* xung tat dan nhanh, bien do lon */
      double env = exp(-(double)(i - attackPos) / 300.0);
      double noise = ((double)((i * 1103515245u + 12345u) % 65536) / 32768.0) - 1.0;
      s += 32768.0 * 0.9 * env * noise;
    }
    buf[i] = clipToPcm(s);
  }
}

/* Chuoi cu danh lien tiep kieu castanets -> nhieu attack */
static void genCastanets(INT_PCM *buf, int n, int sampleRate) {
  const double bedAmp = 32768.0 * 0.01;
  const double f = 300.0;
  const int period = 700; /* mot cu danh moi 700 mau */
  for (int i = 0; i < n; i++) {
    double s = bedAmp * sin(2.0 * M_PI * f * i / sampleRate);
    int phase = i % period;
    double env = exp(-(double)phase / 120.0);
    double noise = ((double)((i * 1103515245u + 12345u) % 65536) / 32768.0) - 1.0;
    s += 32768.0 * 0.8 * env * noise;
    buf[i] = clipToPcm(s);
  }
}

/* Len bien do TU TU (crescendo) -> KHONG duoc coi la attack (ratio < 10) */
static void genSlowCrescendo(INT_PCM *buf, int n, int sampleRate) {
  const double f = 800.0;
  for (int i = 0; i < n; i++) {
    double g = 0.02 + 0.9 * ((double)i / (double)n); /* tang dan tuyen tinh */
    buf[i] = clipToPcm(32768.0 * g * sin(2.0 * M_PI * f * i / sampleRate));
  }
}

typedef void (*SignalGenFn)(INT_PCM *buf, int n, int sampleRate);

typedef struct {
  const char *name;
  const char *desc;
  SignalGenFn gen;
} SIGNAL_CASE;

static const SIGNAL_CASE signalCases[] = {
    {"silence", "im lang tuyet doi", genSilence},
    {"steady_sine", "sine 1 kHz -6 dBFS on dinh", genSteadySine},
    {"single_attack", "nen yen tinh + 1 transient tai mau 3584", genSingleAttack},
    {"castanets", "chuoi transient lien tuc (chu ky 700 mau)", genCastanets},
    {"slow_crescendo", "bien do tang dan tuyen tinh", genSlowCrescendo},
};
static const int nSignalCases = (int)(sizeof(signalCases) / sizeof(signalCases[0]));

/* ------------------------------------------------------------------ */
/* In trang thai OUTPUT cua mot frame                                   */
/* ------------------------------------------------------------------ */

static void printFrameHeader(void) {
  printf(
      "\n"
      " fr | atk idx | seq   shape | grp groupLen  | maxWndNrg  | accWndNrg\n"
      "----+---------+-------------+---------------+------------+-----------\n");
}

static void printFrame(int frame, const BLOCK_SWITCHING_CONTROL *bsc) {
  char grp[32];
  int p = 0;
  for (int i = 0; i < MAX_NO_OF_GROUPS; i++)
    p += snprintf(grp + p, sizeof(grp) - p, "%d%s", bsc->groupLen[i],
                  (i + 1 < MAX_NO_OF_GROUPS) ? "," : "");

  printf(" %2d |  %d   %d  | %s %s |  %d  %-9s | %10ld | %10ld\n", frame,
         bsc->attack ? 1 : 0, bsc->attackIndex,
         windowSequenceName(bsc->lastWindowSequence),
         windowShapeName(bsc->windowShape), bsc->noOfGroups, grp,
         (long)bsc->maxWindowNrg, (long)bsc->accWindowNrg);
}

/* In chi tiet nang luong tung sub-window cua frame hien tai */
static void printWindowEnergies(const BLOCK_SWITCHING_CONTROL *bsc) {
  char bar[64];
  printf("     w |  windowNrg[1] |  windowNrgF[1] |   dB(F) | bar (log, 6dB/ky tu)\n");
  for (UINT w = 0; w < bsc->nBlockSwitchWindows; w++) {
    energyBar(bar, (int)sizeof(bar), bsc->windowNrgF[THIS_WINDOW][w]);
    printf("     %u | %13ld | %14ld | %7.1f | %s%s\n", w,
           (long)bsc->windowNrg[THIS_WINDOW][w],
           (long)bsc->windowNrgF[THIS_WINDOW][w],
           q31_dB(bsc->windowNrgF[THIS_WINDOW][w]), bar,
           (bsc->attack && (INT)w == bsc->attackIndex) ? "  <== ATTACK" : "");
  }
}

/* ------------------------------------------------------------------ */
/* Chay mot kich ban: mono, N frame                                     */
/* ------------------------------------------------------------------ */

typedef struct {
  int isLowDelay;   /* 0 = AAC-LC (8 window, cho phep SHORT), 1 = AAC-LD */
  int granuleLength;/* so mau moi frame: 1024 (LC) / 512 (LD) */
  int isLFE;
  int sampleRate;
  int nFrames;
  int verboseEnergies; /* in chi tiet nang luong tung sub-window */
} RUN_CFG;

static void runMonoCase(const SIGNAL_CASE *sc, const RUN_CFG *cfg, FILE *csv) {
  BLOCK_SWITCHING_CONTROL bsc;
  const int nSamples = cfg->granuleLength * cfg->nFrames;
  INT_PCM *signal = (INT_PCM *)malloc(nSamples * sizeof(INT_PCM));

  sc->gen(signal, nSamples, cfg->sampleRate);

  /* ---- INIT: dat trang thai ban dau ---- */
  FDKaacEnc_InitBlockSwitching(&bsc, cfg->isLowDelay);

  printf(
      "\n============================================================\n"
      "KICH BAN : %s (%s)\n"
      "CHE DO   : %s | granuleLength=%d | isLFE=%d | fs=%d Hz\n"
      "SAU INIT : nBlockSwitchWindows=%u allowShortFrames=%d allowLookAhead=%d\n"
      "           lastWindowSequence=%s windowShape=%s\n"
      "           => do dai 1 sub-window = %d mau\n"
      "============================================================\n",
      sc->name, sc->desc, cfg->isLowDelay ? "AAC-LD" : "AAC-LC",
      cfg->granuleLength, cfg->isLFE, cfg->sampleRate, bsc.nBlockSwitchWindows,
      bsc.allowShortFrames, bsc.allowLookAhead,
      windowSequenceName(bsc.lastWindowSequence),
      windowShapeName(bsc.windowShape),
      cfg->granuleLength >> (bsc.nBlockSwitchWindows == 4 ? 2 : 3));

  printFrameHeader();

  for (int f = 0; f < cfg->nFrames; f++) {
    const INT_PCM *frame = signal + (size_t)f * cfg->granuleLength;

    /* ---- GOI KHOI CAN TEST ---- */
    int err = FDKaacEnc_BlockSwitching(&bsc, cfg->granuleLength, cfg->isLFE,
                                       frame);
    if (err) {
      printf("  !! FDKaacEnc_BlockSwitching tra ve loi %d\n", err);
      break;
    }

    printFrame(f, &bsc);
    if (cfg->verboseEnergies) printWindowEnergies(&bsc);

    if (csv) {
      fprintf(csv, "%s,%s,%d,%d,%d,%d,%s,%s,%d", sc->name,
              cfg->isLowDelay ? "LD" : "LC", f, bsc.attack ? 1 : 0,
              bsc.attackIndex, bsc.lastWindowSequence,
              windowSequenceName(bsc.lastWindowSequence),
              windowShapeName(bsc.windowShape), bsc.noOfGroups);
      for (int i = 0; i < MAX_NO_OF_GROUPS; i++)
        fprintf(csv, ",%d", bsc.groupLen[i]);
      fprintf(csv, ",%ld", (long)bsc.accWindowNrg);
      for (int w = 0; w < BLOCK_SWITCH_WINDOWS; w++)
        fprintf(csv, ",%ld", (long)bsc.windowNrg[THIS_WINDOW][w]);
      for (int w = 0; w < BLOCK_SWITCH_WINDOWS; w++)
        fprintf(csv, ",%ld", (long)bsc.windowNrgF[THIS_WINDOW][w]);
      fprintf(csv, "\n");
    }
  }

  free(signal);
}

/* ------------------------------------------------------------------ */
/* Kich ban stereo: kiem tra FDKaacEnc_SyncBlockSwitching               */
/* ------------------------------------------------------------------ */

static void runStereoSyncCase(int commonWindow, int sampleRate, int nFrames) {
  BLOCK_SWITCHING_CONTROL bscL, bscR;
  const int granuleLength = 1024;
  const int nSamples = granuleLength * nFrames;

  INT_PCM *left = (INT_PCM *)malloc(nSamples * sizeof(INT_PCM));
  INT_PCM *right = (INT_PCM *)malloc(nSamples * sizeof(INT_PCM));

  genSingleAttack(left, nSamples, sampleRate); /* kenh L co transient */
  genSteadySine(right, nSamples, sampleRate);  /* kenh R on dinh */

  FDKaacEnc_InitBlockSwitching(&bscL, 0);
  FDKaacEnc_InitBlockSwitching(&bscR, 0);

  printf(
      "\n============================================================\n"
      "KICH BAN STEREO SYNC  (commonWindow=%d)\n"
      "  L = single_attack, R = steady_sine, AAC-LC, 1024 mau/frame\n"
      "============================================================\n"
      " fr | L truoc sync | R truoc sync || L sau sync | R sau sync | grpL      "
      " | grpR\n"
      "----+--------------+--------------++------------+------------+----------"
      "-+-----------\n",
      commonWindow);

  for (int f = 0; f < nFrames; f++) {
    const INT_PCM *fl = left + (size_t)f * granuleLength;
    const INT_PCM *fr = right + (size_t)f * granuleLength;

    FDKaacEnc_BlockSwitching(&bscL, granuleLength, 0, fl);
    FDKaacEnc_BlockSwitching(&bscR, granuleLength, 0, fr);

    const char *preL = windowSequenceName(bscL.lastWindowSequence);
    const char *preR = windowSequenceName(bscR.lastWindowSequence);

    int err = FDKaacEnc_SyncBlockSwitching(&bscL, &bscR, 2, commonWindow);
    if (err) {
      printf("  !! FDKaacEnc_SyncBlockSwitching tra ve loi %d\n", err);
      break;
    }

    char gl[32], gr[32];
    int pl = 0, pr = 0;
    for (int i = 0; i < MAX_NO_OF_GROUPS; i++) {
      pl += snprintf(gl + pl, sizeof(gl) - pl, "%d%s", bscL.groupLen[i],
                     (i + 1 < MAX_NO_OF_GROUPS) ? "," : "");
      pr += snprintf(gr + pr, sizeof(gr) - pr, "%d%s", bscR.groupLen[i],
                     (i + 1 < MAX_NO_OF_GROUPS) ? "," : "");
    }

    printf(" %2d |    %s     |    %s     ||   %s    |   %s    | %-9s | %-9s\n",
           f, preL, preR, windowSequenceName(bscL.lastWindowSequence),
           windowSequenceName(bscR.lastWindowSequence), gl, gr);
  }

  free(left);
  free(right);
}

/* ------------------------------------------------------------------ */

static void printConstants(void) {
  printf(
      "=================== HANG SO CUA KHOI ===================\n"
      "  BLOCK_SWITCH_WINDOWS      = %d\n"
      "  BLOCK_SWITCHING_IIR_LEN   = %d\n"
      "  BLOCK_SWITCH_ENERGY_SHIFT = %d\n"
      "  MAX_NO_OF_GROUPS          = %d\n"
      "  TRANS_FAC                 = %d\n"
      "  SAMPLE_BITS (INT_PCM)     = %d\n"
      "  DFRACT_BITS (FIXP_DBL)    = %d\n"
      "  minAttackNrg (Q31 int)    = %ld   (= 1e6*NORM_PCM_ENERGY >> %d)\n"
      "  nguong attack             : windowNrgF[w] * 0.1 > accWindowNrg\n"
      "                              (tuc la nang luong > 10x trung binh truot)\n"
      "  accWindowNrg              : acc = 0.7*acc + 0.3*nrgF[w-1]\n"
      "========================================================\n",
      BLOCK_SWITCH_WINDOWS, BLOCK_SWITCHING_IIR_LEN, BLOCK_SWITCH_ENERGY_SHIFT,
      MAX_NO_OF_GROUPS, TRANS_FAC, SAMPLE_BITS, DFRACT_BITS,
      (long)(LONG)(FL2FXCONST_DBL(1e+6f * NORM_PCM_ENERGY) >>
                   BLOCK_SWITCH_ENERGY_SHIFT),
      BLOCK_SWITCH_ENERGY_SHIFT);
}

int main(int argc, char **argv) {
  int writeCsv = 1;
  const char *csvPath = "out/block_switch_trace.csv";

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--no-csv"))
      writeCsv = 0;
    else if (!strcmp(argv[i], "--csv") && i + 1 < argc)
      csvPath = argv[++i];
    else {
      printf("Cach dung: %s [--no-csv] [--csv <duong_dan.csv>]\n", argv[0]);
      return 1;
    }
  }

  FILE *csv = NULL;
  if (writeCsv) {
    csv = fopen(csvPath, "w");
    if (!csv) {
      printf(
          "Khong mo duoc '%s' (thu tao thu muc 'out' truoc). Bo qua CSV.\n",
          csvPath);
    } else {
      fprintf(csv, "case,mode,frame,attack,attackIndex,seqId,seq,shape,noOfGroups");
      for (int i = 0; i < MAX_NO_OF_GROUPS; i++) fprintf(csv, ",groupLen%d", i);
      fprintf(csv, ",accWindowNrg");
      for (int w = 0; w < BLOCK_SWITCH_WINDOWS; w++) fprintf(csv, ",nrg%d", w);
      for (int w = 0; w < BLOCK_SWITCH_WINDOWS; w++) fprintf(csv, ",nrgF%d", w);
      fprintf(csv, "\n");
    }
  }

  printConstants();

  RUN_CFG lc;
  lc.isLowDelay = 0;
  lc.granuleLength = 1024;
  lc.isLFE = 0;
  lc.sampleRate = 48000;
  lc.nFrames = 8;
  lc.verboseEnergies = 0;

  /* --- 1. Tat ca kich ban o che do AAC-LC --- */
  for (int i = 0; i < nSignalCases; i++) {
    /* chi in chi tiet nang luong cho kich ban co transient */
    lc.verboseEnergies = (strcmp(signalCases[i].name, "single_attack") == 0);
    runMonoCase(&signalCases[i], &lc, csv);
  }

  /* --- 2. AAC-LD: 4 sub-window, khong co SHORT_WINDOW --- */
  RUN_CFG ld;
  ld.isLowDelay = 1;
  ld.granuleLength = 512;
  ld.isLFE = 0;
  ld.sampleRate = 48000;
  ld.nFrames = 12;
  ld.verboseEnergies = 0;
  for (int i = 0; i < nSignalCases; i++) {
    if (strcmp(signalCases[i].name, "single_attack") &&
        strcmp(signalCases[i].name, "castanets"))
      continue;
    runMonoCase(&signalCases[i], &ld, csv);
  }

  /* --- 3. LFE: luon LONG/SINE bat ke tin hieu --- */
  RUN_CFG lfe = lc;
  lfe.isLFE = 1;
  lfe.verboseEnergies = 0;
  for (int i = 0; i < nSignalCases; i++) {
    if (strcmp(signalCases[i].name, "castanets")) continue;
    runMonoCase(&signalCases[i], &lfe, csv);
  }

  /* --- 4. Stereo sync --- */
  runStereoSyncCase(1 /*commonWindow*/, 48000, 8);
  runStereoSyncCase(0 /*commonWindow*/, 48000, 8);

  if (csv) {
    fclose(csv);
    printf("\n>> Da ghi trace CSV: %s\n", csvPath);
  }
  return 0;
}
