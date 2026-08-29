/*
 * ref_block_switch.cpp
 *
 * Hien thuc REFERENCE MODEL cho khoi Block Switching.
 * Doc lap hoan toan voi fdk-aac: khong include header nao cua thu vien.
 *
 * Nguon doi chieu (chi de doc, khong link):
 *   libAACenc/src/block_switch.cpp
 *   libAACenc/src/block_switch.h
 *   libAACenc/src/psy_const.h
 *   libFDK/include/common_fix.h, fixmul.h, fixmadd.h   (ngu nghia fixed-point)
 *   libSYS/include/machine_type.h                      (SAMPLE_BITS = 16)
 */

#include "ref_block_switch.h"

#include <string.h>

/* ======================================================================== */
/* PHAN A - So hoc fixed-point Q31, tai tao dung ngu nghia cua libFDK        */
/* ======================================================================== */

/*
 * FIXP_DBL  = int32_t, dinh dang Q31 (gia tri thuc = raw / 2^31)
 * FIXP_SGL  = int16_t, dinh dang Q15
 *
 * fixmul.h  : fixmuldiv2_DD(a,b) = (int32)(((int64)a*b) >> 32)      = a*b/2
 *             fixmul_DD(a,b)     = fixmuldiv2_DD(a,b) << 1          = a*b
 *             fixpow2div2_D(a)   = fixmuldiv2_DD(a,a)               = a^2/2
 * fixmadd.h : fixmadddiv2_DD(x,a,b) = x + fixmuldiv2_DD(a,b)        = x + a*b/2
 *             fixmadd_DD(x,a,b)     = fixmadddiv2_DD(x,a,b) << 1    = 2x + a*b
 *
 * Dich trai duoc lam qua uint32 de tranh UB khi tran/so am
 * (dung hanh vi wrap-around ma trinh dich C thuc te sinh ra).
 */

static inline int32_t rShl1(int32_t x) { return (int32_t)((uint32_t)x << 1); }

static inline int32_t rAddWrap(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a + (uint32_t)b);
}
static inline int32_t rSubWrap(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a - (uint32_t)b);
}

static inline int32_t rMultDiv2(int32_t a, int32_t b) {
  return (int32_t)((((int64_t)a) * b) >> 32);
}
static inline int32_t rMult(int32_t a, int32_t b) {
  return rShl1(rMultDiv2(a, b));
}
static inline int32_t rPow2Div2(int32_t a) { return rMultDiv2(a, a); }

static inline int32_t rMultAdd(int32_t x, int32_t a, int32_t b) {
  return rShl1(rAddWrap(x, rMultDiv2(a, b)));
}

static inline int32_t rMax(int32_t a, int32_t b) { return (a > b) ? a : b; }

/*
 * Tai tao macro FL2FXCONST_DBL(val) trong libFDK/include/common_fix.h:
 *   val >= 0 : (val*2^31 + 0.5 >= 0x7FFFFFFF) ? 0x7FFFFFFF : (LONG)(val*2^31 + 0.5)
 *   val <  0 : (val*2^31 - 0.5 <= 0x80000000) ? 0x80000000 : (LONG)(val*2^31 - 0.5)
 * Luu y: ep kieu (LONG) cat ve 0 (truncation toward zero), KHONG phai lam tron.
 */
static int32_t rFl2Dbl(double val) {
  const double scale = 2147483648.0; /* 2^31 */
  if (val >= 0.0) {
    double t = val * scale + 0.5;
    if (t >= 2147483647.0) return (int32_t)0x7FFFFFFF;
    return (int32_t)t;
  } else {
    double t = val * scale - 0.5;
    if (t <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)t;
  }
}

/* FL2FXCONST_SGL: y het nhung sang Q15 (MAXVAL_SGL=32767, MINVAL_SGL=-32768) */
static int16_t rFl2Sgl(double val) {
  const double scale = 32768.0; /* 2^15 */
  if (val >= 0.0) {
    double t = val * scale + 0.5;
    if (t >= 32767.0) return (int16_t)32767;
    return (int16_t)t;
  } else {
    double t = val * scale - 0.5;
    if (t <= -32768.0) return (int16_t)(-32768);
    return (int16_t)t;
  }
}

/*
 * FX_SGL2FX_DBL(val) = (LONG)val << 16   (common_fix.h:218)
 *
 * Khi he so la FIXP_SGL, moi phep nhan cua FDK di qua
 *   fixmuldiv2_SD(a,b) = fixmuldiv2_DD(FX_SGL2FX_DBL(a), b)
 * => tuong duong nhan Q31 voi he so "mo rong" (Q15 << 16).
 * Do do 16 bit thap cua he so bi ep ve 0 -> ket qua KHAC voi nhanh Q31.
 */
static inline int32_t rSgl2Dbl(int16_t v) {
  return (int32_t)((uint32_t)((int32_t)v) << 16);
}

/* ======================================================================== */
/* PHAN B - Hang so & bang tra (soi guong block_switch.cpp)                  */
/* ======================================================================== */

/* IIR high-pass coefficients: y[n] = 0.7548*(x[n]-x[n-1]) + 0.5095*y[n-1] */
static const double HI_PASS_COEFF_D[REF_BLOCK_SWITCHING_IIR_LEN] = {-0.5095,
                                                                    0.7548};

/* he so trung binh truot cua accWindowNrg.
 * Ban goc viet 0.3f / 0.7f / 0.1f (float literal) roi macro moi ep sang double
 * -> gia tri KHAC voi 0.3 / 0.7 / 0.1 o dang double. Phai giu dung hau to 'f'. */
static const double ACC_WINDOW_NRG_FAC_D = (double)0.3f;
static const double ONE_MINUS_ACC_WINDOW_NRG_FAC_D = (double)0.7f;

/* nguong ti le attack: nrgF > 10 * acc  <=>  0.1 * nrgF > acc */
static const double INV_ATTACK_RATIO_D = (double)0.1f;

/* NORM_PCM_ENERGY = (1/32768)^2 = 2^-30  (psy_const.h) */
static const double NORM_PCM_ENERGY_D = 1.0 / 1073741824.0;

/* blockType2windowShape[allowShortFrames][blockType]
 *   hang 0 = AAC-LD/ELD, hang 1 = AAC-LC                                   */
static const int BLOCKTYPE2WINDOWSHAPE[2][5] = {
    /* LD */ {REF_SINE_WINDOW, REF_KBD_WINDOW, REF_WRONG_WINDOW,
              REF_SINE_WINDOW, REF_KBD_WINDOW},
    /* LC */ {REF_KBD_WINDOW, REF_SINE_WINDOW, REF_SINE_WINDOW, REF_KBD_WINDOW,
              REF_WRONG_WINDOW}};

/* grouping goi y theo vi tri attack cua frame TRUOC */
static const int SUGGESTED_GROUPING_TABLE[REF_TRANS_FAC][REF_MAX_NO_OF_GROUPS] =
    {
        /* attack @ w0 */ {1, 3, 3, 1},
        /* attack @ w1 */ {1, 1, 3, 3},
        /* attack @ w2 */ {2, 1, 3, 2},
        /* attack @ w3 */ {3, 1, 3, 1},
        /* attack @ w4 */ {3, 1, 1, 3},
        /* attack @ w5 */ {3, 2, 1, 2},
        /* attack @ w6 */ {3, 3, 1, 1},
        /* attack @ w7 */ {3, 3, 1, 1}};

/* FSM khong look-ahead (AAC-LD): chgWndSq[attack][seq] */
static const int CHG_WND_SQ[2][REF_N_BLOCKTYPES] = {
    /* no attack */ {REF_LONG_WINDOW, REF_STOP_WINDOW, REF_WRONG_WINDOW,
                     REF_LONG_WINDOW, REF_STOP_WINDOW, REF_WRONG_WINDOW},
    /* attack    */ {REF_START_WINDOW, REF_LOWOV_WINDOW, REF_WRONG_WINDOW,
                     REF_START_WINDOW, REF_LOWOV_WINDOW, REF_WRONG_WINDOW}};

/* FSM co look-ahead (AAC-LC): chgWndSqLkAhd[lastattack][attack][seq] */
static const int CHG_WND_SQ_LK_AHD[2][2][REF_N_BLOCKTYPES] = {
    /* lastattack = 0 */
    {/* attack = 0 */ {REF_LONG_WINDOW, REF_SHORT_WINDOW, REF_STOP_WINDOW,
                       REF_LONG_WINDOW, REF_WRONG_WINDOW, REF_WRONG_WINDOW},
     /* attack = 1 */
     {REF_START_WINDOW, REF_SHORT_WINDOW, REF_SHORT_WINDOW, REF_START_WINDOW,
      REF_WRONG_WINDOW, REF_WRONG_WINDOW}},
    /* lastattack = 1 */
    {/* attack = 0 */ {REF_LONG_WINDOW, REF_SHORT_WINDOW, REF_SHORT_WINDOW,
                       REF_LONG_WINDOW, REF_WRONG_WINDOW, REF_WRONG_WINDOW},
     /* attack = 1 */
     {REF_START_WINDOW, REF_SHORT_WINDOW, REF_SHORT_WINDOW, REF_START_WINDOW,
      REF_WRONG_WINDOW, REF_WRONG_WINDOW}}};

/* dong bo block type giua 2 kenh cua mot CPE */
static const unsigned char SYNCHRONIZED_BLOCK_TYPE_TABLE[5][5] = {
    /* LONG  */ {REF_LONG_WINDOW, REF_START_WINDOW, REF_SHORT_WINDOW,
                 REF_STOP_WINDOW, REF_LOWOV_WINDOW},
    /* START */
    {REF_START_WINDOW, REF_START_WINDOW, REF_SHORT_WINDOW, REF_SHORT_WINDOW,
     REF_LOWOV_WINDOW},
    /* SHORT */
    {REF_SHORT_WINDOW, REF_SHORT_WINDOW, REF_SHORT_WINDOW, REF_SHORT_WINDOW,
     REF_WRONG_WINDOW},
    /* STOP  */
    {REF_STOP_WINDOW, REF_SHORT_WINDOW, REF_SHORT_WINDOW, REF_STOP_WINDOW,
     REF_LOWOV_WINDOW},
    /* LOWOV */
    {REF_LOWOV_WINDOW, REF_LOWOV_WINDOW, REF_WRONG_WINDOW, REF_LOWOV_WINDOW,
     REF_LOWOV_WINDOW}};

/* --- hang so fixed-point dan xuat ---
 *
 * QUAN TRONG: block_switch.cpp co HAI nhanh hang so tuy kien truc:
 *   #ifndef SINETABLE_16BIT : hiPassCoeff / oneMinusAccWindowNrgFac /
 *                             invAttackRatio la FIXP_DBL (Q31)
 *   #else                   : ba hang so tren la FIXP_SGL (Q15)
 * FDK_archdef.h dinh nghia SINETABLE_16BIT cho x86 (ke ca x86-64), ARM,
 * RISC-V, s390x, sparc; KHONG dinh nghia cho MIPS, PowerPC.
 * => Khoi nay KHONG bit-exact giua cac kien truc. REF_COEFF_16BIT chon nhanh.
 *
 * accWindowNrgFac luon la FIXP_DBL o ca hai nhanh.
 * Luu y hau to 'f': FL2FXCONST_*(0.3f) khac FL2FXCONST_*(0.3).
 */
static int32_t coefEff(double v) {
#if REF_COEFF_16BIT
  return rSgl2Dbl(rFl2Sgl(v));
#else
  return rFl2Dbl(v);
#endif
}

static int32_t hiPassCoeff0(void) { return coefEff(HI_PASS_COEFF_D[0]); }
static int32_t hiPassCoeff1(void) { return coefEff(HI_PASS_COEFF_D[1]); }
static int32_t accWindowNrgFac(void) {
  return rFl2Dbl(ACC_WINDOW_NRG_FAC_D); /* luon Q31 */
}
static int32_t oneMinusAccWindowNrgFac(void) {
  return coefEff(ONE_MINUS_ACC_WINDOW_NRG_FAC_D);
}
static int32_t invAttackRatio(void) { return coefEff(INV_ATTACK_RATIO_D); }

int RefBs_UsesCoeff16(void) { return REF_COEFF_16BIT ? 1 : 0; }

/* minAttackNrg = FL2FXCONST_DBL(1e6 * NORM_PCM_ENERGY) >> 7  = 2000000 >> 7 */
static int32_t minAttackNrg(void) {
  return rFl2Dbl(1.0e6 * NORM_PCM_ENERGY_D) >> REF_BLOCK_SWITCH_ENERGY_SHIFT;
}

int32_t RefBs_GetMinAttackNrg(void) { return minAttackNrg(); }
int32_t RefBs_GetHiPassCoeff(int idx) {
  return (idx == 0) ? hiPassCoeff0() : hiPassCoeff1();
}
int32_t RefBs_GetAccWindowNrgFac(void) { return accWindowNrgFac(); }
int32_t RefBs_GetInvAttackRatio(void) { return invAttackRatio(); }
int RefBs_GetWindowShapeFor(int allowShortFrames, int blockType) {
  if (blockType < 0 || blockType > 4) return REF_WRONG_WINDOW;
  return BLOCKTYPE2WINDOWSHAPE[allowShortFrames ? 1 : 0][blockType];
}

/* ======================================================================== */
/* PHAN C - MO HINH FIXED-POINT (bit-exact)                                  */
/* ======================================================================== */

void RefBs_Init(RefBsState *st, int isLowDelay) {
  memset(st, 0, sizeof(*st));

  if (isLowDelay) {
    st->nBlockSwitchWindows = 4;
    st->allowShortFrames = 0;
    st->allowLookAhead = 0;
  } else {
    st->nBlockSwitchWindows = 8;
    st->allowShortFrames = 1;
    st->allowLookAhead = 1;
  }

  st->noOfGroups = REF_MAX_NO_OF_GROUPS;
  st->lastWindowSequence = REF_LONG_WINDOW;
  st->windowShape =
      BLOCKTYPE2WINDOWSHAPE[st->allowShortFrames][st->lastWindowSequence];
}

/*
 * Buoc (3): nang luong sub-window, ban thoi va ban da loc high-pass.
 *
 * Scaling (SAMPLE_BITS = 16, DFRACT_BITS = 32):
 *   s = pcm << (32-16-1) = pcm << 15      -> gia tri thuc = pcm / 65536
 *   (dich 15 chu khong phai 16: du 1 bit headroom cho bo loc IIR)
 *
 *   nrg  += (s^2 / 2) >> (7-1-2)  = s^2 / 32
 *
 * Tich luy bang unsigned 32-bit (co wrap) roi bao hoa ve MAXVAL_DBL,
 * dung nhu ULONG temp_windowNrg trong ban goc.
 */
static void RefBs_CalcWindowEnergy(RefBsState *st, int windowLen,
                                   const int16_t *pTimeSignal) {
  const int32_t c0 = hiPassCoeff0();
  const int32_t c1 = hiPassCoeff1();

  int32_t s0 = st->iirStates[0];
  int32_t s1 = st->iirStates[1];

  for (unsigned w = 0; w < st->nBlockSwitchWindows; w++) {
    uint32_t nrg = 0u;
    uint32_t nrgF = 0u;

    for (int i = 0; i < windowLen; i++) {
      int32_t x, t1, t2;

      x = (int32_t)((uint32_t)((int32_t)(*pTimeSignal++)) << 15);

      t1 = rMultDiv2(c1, rSubWrap(x, s0));
      t2 = rMultDiv2(c0, s1);
      s0 = x;
      s1 = rShl1(rSubWrap(t1, t2));

      nrg += (uint32_t)(rPow2Div2(s0) >>
                        (REF_BLOCK_SWITCH_ENERGY_SHIFT - 1 - 2));
      nrgF += (uint32_t)(rPow2Div2(s1) >>
                         (REF_BLOCK_SWITCH_ENERGY_SHIFT - 1 - 2));
    }

    st->windowNrg[REF_THIS_WINDOW][w] =
        (int32_t)((nrg < 0x7FFFFFFFu) ? nrg : 0x7FFFFFFFu);
    st->windowNrgF[REF_THIS_WINDOW][w] =
        (int32_t)((nrgF < 0x7FFFFFFFu) ? nrgF : 0x7FFFFFFFu);
  }

  st->iirStates[0] = s0;
  st->iirStates[1] = s1;
}

int RefBs_Process(RefBsState *st, int granuleLength, int isLFE,
                  const int16_t *pTimeSignal) {
  const unsigned n = st->nBlockSwitchWindows;
  int32_t enM1, enMax;

  /* ---- (0) LFE: khong phan tich, luon LONG/SINE ---- */
  if (isLFE) {
    st->lastWindowSequence = REF_LONG_WINDOW;
    st->windowShape = REF_SINE_WINDOW;
    st->noOfGroups = 1;
    st->groupLen[0] = 1;
    return 0;
  }

  /* ---- (1) day trang thai frame hien tai -> frame truoc ---- */
  st->lastattack = st->attack;
  st->lastAttackIndex = st->attackIndex;

  memcpy(st->windowNrg[REF_LAST_WINDOW], st->windowNrg[REF_THIS_WINDOW],
         sizeof(st->windowNrg[0]));
  memcpy(st->windowNrgF[REF_LAST_WINDOW], st->windowNrgF[REF_THIS_WINDOW],
         sizeof(st->windowNrgF[0]));

  /* ---- (2) grouping cho frame TRUOC (do co look-ahead 1 frame) ---- */
  if (st->allowShortFrames) {
    memset(st->groupLen, 0, sizeof(st->groupLen));
    st->noOfGroups = REF_MAX_NO_OF_GROUPS;

    memcpy(st->groupLen, SUGGESTED_GROUPING_TABLE[st->lastAttackIndex],
           sizeof(st->groupLen));

    /* luu y: st->attack o day VAN la attack cua frame truoc */
    if (st->attack == 1)
      st->maxWindowNrg = st->windowNrg[REF_LAST_WINDOW][st->lastAttackIndex];
    else
      st->maxWindowNrg = 0;
  }

  /* ---- (3) nang luong sub-window ---- */
  RefBs_CalcWindowEnergy(st, granuleLength >> ((n == 4) ? 2 : 3), pTimeSignal);

  /* ---- (4) do attack ---- */
  st->attack = 0;

  enMax = 0;
  enM1 = st->windowNrgF[REF_LAST_WINDOW][n - 1];

  for (unsigned i = 0; i < n; i++) {
    int32_t tmp = rMultDiv2(oneMinusAccWindowNrgFac(), st->accWindowNrg);
    st->accWindowNrg = rMultAdd(tmp, accWindowNrgFac(), enM1);

    if (rMult(st->windowNrgF[REF_THIS_WINDOW][i], invAttackRatio()) >
        st->accWindowNrg) {
      st->attack = 1;
      st->attackIndex = (int32_t)i;
    }
    enM1 = st->windowNrgF[REF_THIS_WINDOW][i];
    enMax = rMax(enMax, enM1);
  }

  /* cong nang luong toi thieu */
  if (enMax < minAttackNrg()) st->attack = 0;

  /* attack tran qua bien frame: nrgF_last[n-1] > 10 * nrgF_cur[1] */
  if ((st->attack == 0) && (st->lastattack == 1)) {
    if (((st->windowNrgF[REF_LAST_WINDOW][n - 1] >> 4) >
         rMult((int32_t)(10 << (32 - 1 - 4)),
               st->windowNrgF[REF_THIS_WINDOW][1])) &&
        (st->lastAttackIndex == (int32_t)n - 1)) {
      st->attack = 1;
      st->attackIndex = 0;
    }
  }

  /* ---- (5) may trang thai window sequence ---- */
  if (st->allowLookAhead) {
    st->lastWindowSequence =
        CHG_WND_SQ_LK_AHD[st->lastattack][st->attack][st->lastWindowSequence];
  } else {
    st->lastWindowSequence = CHG_WND_SQ[st->attack][st->lastWindowSequence];
  }

  /* ---- (6) window shape ---- */
  st->windowShape =
      BLOCKTYPE2WINDOWSHAPE[st->allowShortFrames][st->lastWindowSequence];

  return 0;
}

int RefBs_Sync(RefBsState *left, RefBsState *right, int nChannels,
               int commonWindow) {
  int patchType = REF_LONG_WINDOW;

  if (nChannels == 2 && commonWindow == 1) {
    patchType =
        SYNCHRONIZED_BLOCK_TYPE_TABLE[patchType][left->lastWindowSequence];
    patchType =
        SYNCHRONIZED_BLOCK_TYPE_TABLE[patchType][right->lastWindowSequence];

    if (patchType == REF_WRONG_WINDOW) return -1;

    left->lastWindowSequence = patchType;
    right->lastWindowSequence = patchType;

    left->windowShape =
        BLOCKTYPE2WINDOWSHAPE[left->allowShortFrames][left->lastWindowSequence];
    right->windowShape = BLOCKTYPE2WINDOWSHAPE[left->allowShortFrames]
                                              [right->lastWindowSequence];
  }

  if (left->allowShortFrames) {
    int i;

    if (nChannels == 2) {
      if (commonWindow == 1) {
        int seqLOld = left->lastWindowSequence;
        int seqROld = right->lastWindowSequence;

        if (patchType != REF_SHORT_WINDOW) {
          left->noOfGroups = 1;
          right->noOfGroups = 1;
          left->groupLen[0] = 1;
          right->groupLen[0] = 1;
          for (i = 1; i < REF_MAX_NO_OF_GROUPS; i++) {
            left->groupLen[i] = 0;
            right->groupLen[i] = 0;
          }
        } else {
          if ((seqLOld == REF_SHORT_WINDOW) && (seqROld == REF_SHORT_WINDOW)) {
            if (left->maxWindowNrg > right->maxWindowNrg) {
              right->noOfGroups = left->noOfGroups;
              for (i = 0; i < REF_MAX_NO_OF_GROUPS; i++)
                right->groupLen[i] = left->groupLen[i];
            } else {
              left->noOfGroups = right->noOfGroups;
              for (i = 0; i < REF_MAX_NO_OF_GROUPS; i++)
                left->groupLen[i] = right->groupLen[i];
            }
          } else if ((seqLOld == REF_SHORT_WINDOW) &&
                     (seqROld != REF_SHORT_WINDOW)) {
            right->noOfGroups = left->noOfGroups;
            for (i = 0; i < REF_MAX_NO_OF_GROUPS; i++)
              right->groupLen[i] = left->groupLen[i];
          } else if ((seqROld == REF_SHORT_WINDOW) &&
                     (seqLOld != REF_SHORT_WINDOW)) {
            left->noOfGroups = right->noOfGroups;
            for (i = 0; i < REF_MAX_NO_OF_GROUPS; i++)
              left->groupLen[i] = right->groupLen[i];
          } else {
            left->noOfGroups = right->noOfGroups = 2;
            left->groupLen[0] = right->groupLen[0] = 4;
            left->groupLen[1] = right->groupLen[1] = 4;
          }
        }
      } else {
        /* stereo, khong common window */
        if (left->lastWindowSequence != REF_SHORT_WINDOW) {
          left->noOfGroups = 1;
          left->groupLen[0] = 1;
          for (i = 1; i < REF_MAX_NO_OF_GROUPS; i++) left->groupLen[i] = 0;
        }
        if (right->lastWindowSequence != REF_SHORT_WINDOW) {
          right->noOfGroups = 1;
          right->groupLen[0] = 1;
          for (i = 1; i < REF_MAX_NO_OF_GROUPS; i++) right->groupLen[i] = 0;
        }
      }
    } else {
      /* mono */
      if (left->lastWindowSequence != REF_SHORT_WINDOW) {
        left->noOfGroups = 1;
        left->groupLen[0] = 1;
        for (i = 1; i < REF_MAX_NO_OF_GROUPS; i++) left->groupLen[i] = 0;
      }
    }
  }

  /* LOWOV chi ton tai noi bo -> quy ve LONG + window shape LOL */
  if (!left->allowShortFrames) {
    if (left->lastWindowSequence != REF_LONG_WINDOW &&
        left->lastWindowSequence != REF_STOP_WINDOW) {
      left->lastWindowSequence = REF_LONG_WINDOW;
      left->windowShape = REF_LOL_WINDOW;
    }
  }
  if (nChannels == 2) {
    if (!right->allowShortFrames) {
      if (right->lastWindowSequence != REF_LONG_WINDOW &&
          right->lastWindowSequence != REF_STOP_WINDOW) {
        right->lastWindowSequence = REF_LONG_WINDOW;
        right->windowShape = REF_LOL_WINDOW;
      }
    }
  }

  return 0;
}

/* ======================================================================== */
/* PHAN D - MO HINH FLOAT (spec-level)                                       */
/* ======================================================================== */
/*
 * Cung thuat toan, viet bang double. Cac cong thuc o day chinh la "dac ta
 * toan hoc" cua khoi:
 *
 *   s[n]    = pcm[n] / 65536
 *   y[n]    = 0.7548*(s[n] - s[n-1]) + 0.5095*y[n-1]
 *   E[w]    = sum_{n in w} s[n]^2 / 32
 *   EF[w]   = sum_{n in w} y[n]^2 / 32
 *   acc[w]  = 0.7*acc[w-1] + 0.3*EF[w-1]
 *   attack <=> 0.1*EF[w] > acc[w]   va   max_w EF[w] >= minAttackNrg
 */

static const double MIN_ATTACK_NRG_D =
    (1.0e6 * (1.0 / 1073741824.0)) / 128.0; /* = 7.2759576141834259e-6 */

void RefBsF_Init(RefBsStateF *st, int isLowDelay) {
  memset(st, 0, sizeof(*st));

  if (isLowDelay) {
    st->nBlockSwitchWindows = 4;
    st->allowShortFrames = 0;
    st->allowLookAhead = 0;
  } else {
    st->nBlockSwitchWindows = 8;
    st->allowShortFrames = 1;
    st->allowLookAhead = 1;
  }

  st->noOfGroups = REF_MAX_NO_OF_GROUPS;
  st->lastWindowSequence = REF_LONG_WINDOW;
  st->windowShape =
      BLOCKTYPE2WINDOWSHAPE[st->allowShortFrames][st->lastWindowSequence];
}

static void RefBsF_CalcWindowEnergy(RefBsStateF *st, int windowLen,
                                    const int16_t *pTimeSignal) {
  double s0 = st->iirStates[0];
  double s1 = st->iirStates[1];

  for (unsigned w = 0; w < st->nBlockSwitchWindows; w++) {
    double nrg = 0.0, nrgF = 0.0;

    for (int i = 0; i < windowLen; i++) {
      double x = (double)(*pTimeSignal++) / 65536.0;

      double y = HI_PASS_COEFF_D[1] * (x - s0) - HI_PASS_COEFF_D[0] * s1;
      s0 = x;
      s1 = y;

      nrg += (s0 * s0) / 32.0;
      nrgF += (s1 * s1) / 32.0;
    }

    st->windowNrg[REF_THIS_WINDOW][w] = (nrg < 1.0) ? nrg : 1.0;
    st->windowNrgF[REF_THIS_WINDOW][w] = (nrgF < 1.0) ? nrgF : 1.0;
  }

  st->iirStates[0] = s0;
  st->iirStates[1] = s1;
}

int RefBsF_Process(RefBsStateF *st, int granuleLength, int isLFE,
                   const int16_t *pTimeSignal) {
  const unsigned n = st->nBlockSwitchWindows;
  double enM1, enMax;

  if (isLFE) {
    st->lastWindowSequence = REF_LONG_WINDOW;
    st->windowShape = REF_SINE_WINDOW;
    st->noOfGroups = 1;
    st->groupLen[0] = 1;
    return 0;
  }

  st->lastattack = st->attack;
  st->lastAttackIndex = st->attackIndex;

  memcpy(st->windowNrg[REF_LAST_WINDOW], st->windowNrg[REF_THIS_WINDOW],
         sizeof(st->windowNrg[0]));
  memcpy(st->windowNrgF[REF_LAST_WINDOW], st->windowNrgF[REF_THIS_WINDOW],
         sizeof(st->windowNrgF[0]));

  if (st->allowShortFrames) {
    memset(st->groupLen, 0, sizeof(st->groupLen));
    st->noOfGroups = REF_MAX_NO_OF_GROUPS;
    for (int g = 0; g < REF_MAX_NO_OF_GROUPS; g++)
      st->groupLen[g] = SUGGESTED_GROUPING_TABLE[st->lastAttackIndex][g];

    if (st->attack == 1)
      st->maxWindowNrg = st->windowNrg[REF_LAST_WINDOW][st->lastAttackIndex];
    else
      st->maxWindowNrg = 0.0;
  }

  RefBsF_CalcWindowEnergy(st, granuleLength >> ((n == 4) ? 2 : 3), pTimeSignal);

  st->attack = 0;
  enMax = 0.0;
  enM1 = st->windowNrgF[REF_LAST_WINDOW][n - 1];

  for (unsigned i = 0; i < n; i++) {
    st->accWindowNrg = ONE_MINUS_ACC_WINDOW_NRG_FAC_D * st->accWindowNrg +
                       ACC_WINDOW_NRG_FAC_D * enM1;

    if (INV_ATTACK_RATIO_D * st->windowNrgF[REF_THIS_WINDOW][i] >
        st->accWindowNrg) {
      st->attack = 1;
      st->attackIndex = (int)i;
    }
    enM1 = st->windowNrgF[REF_THIS_WINDOW][i];
    if (enM1 > enMax) enMax = enM1;
  }

  if (enMax < MIN_ATTACK_NRG_D) st->attack = 0;

  if ((st->attack == 0) && (st->lastattack == 1)) {
    if ((st->windowNrgF[REF_LAST_WINDOW][n - 1] >
         10.0 * st->windowNrgF[REF_THIS_WINDOW][1]) &&
        (st->lastAttackIndex == (int)n - 1)) {
      st->attack = 1;
      st->attackIndex = 0;
    }
  }

  if (st->allowLookAhead) {
    st->lastWindowSequence =
        CHG_WND_SQ_LK_AHD[st->lastattack][st->attack][st->lastWindowSequence];
  } else {
    st->lastWindowSequence = CHG_WND_SQ[st->attack][st->lastWindowSequence];
  }

  st->windowShape =
      BLOCKTYPE2WINDOWSHAPE[st->allowShortFrames][st->lastWindowSequence];

  return 0;
}

int RefBsF_Sync(RefBsStateF *left, RefBsStateF *right, int nChannels,
                int commonWindow) {
  int patchType = REF_LONG_WINDOW;

  if (nChannels == 2 && commonWindow == 1) {
    patchType =
        SYNCHRONIZED_BLOCK_TYPE_TABLE[patchType][left->lastWindowSequence];
    patchType =
        SYNCHRONIZED_BLOCK_TYPE_TABLE[patchType][right->lastWindowSequence];
    if (patchType == REF_WRONG_WINDOW) return -1;

    left->lastWindowSequence = patchType;
    right->lastWindowSequence = patchType;
    left->windowShape =
        BLOCKTYPE2WINDOWSHAPE[left->allowShortFrames][left->lastWindowSequence];
    right->windowShape = BLOCKTYPE2WINDOWSHAPE[left->allowShortFrames]
                                              [right->lastWindowSequence];
  }

  if (left->allowShortFrames) {
    int i;
    if (nChannels == 2) {
      if (commonWindow == 1) {
        int seqLOld = left->lastWindowSequence;
        int seqROld = right->lastWindowSequence;

        if (patchType != REF_SHORT_WINDOW) {
          left->noOfGroups = right->noOfGroups = 1;
          left->groupLen[0] = right->groupLen[0] = 1;
          for (i = 1; i < REF_MAX_NO_OF_GROUPS; i++)
            left->groupLen[i] = right->groupLen[i] = 0;
        } else {
          if ((seqLOld == REF_SHORT_WINDOW) && (seqROld == REF_SHORT_WINDOW)) {
            if (left->maxWindowNrg > right->maxWindowNrg) {
              right->noOfGroups = left->noOfGroups;
              for (i = 0; i < REF_MAX_NO_OF_GROUPS; i++)
                right->groupLen[i] = left->groupLen[i];
            } else {
              left->noOfGroups = right->noOfGroups;
              for (i = 0; i < REF_MAX_NO_OF_GROUPS; i++)
                left->groupLen[i] = right->groupLen[i];
            }
          } else if ((seqLOld == REF_SHORT_WINDOW) &&
                     (seqROld != REF_SHORT_WINDOW)) {
            right->noOfGroups = left->noOfGroups;
            for (i = 0; i < REF_MAX_NO_OF_GROUPS; i++)
              right->groupLen[i] = left->groupLen[i];
          } else if ((seqROld == REF_SHORT_WINDOW) &&
                     (seqLOld != REF_SHORT_WINDOW)) {
            left->noOfGroups = right->noOfGroups;
            for (i = 0; i < REF_MAX_NO_OF_GROUPS; i++)
              left->groupLen[i] = right->groupLen[i];
          } else {
            left->noOfGroups = right->noOfGroups = 2;
            left->groupLen[0] = right->groupLen[0] = 4;
            left->groupLen[1] = right->groupLen[1] = 4;
          }
        }
      } else {
        if (left->lastWindowSequence != REF_SHORT_WINDOW) {
          left->noOfGroups = 1;
          left->groupLen[0] = 1;
          for (i = 1; i < REF_MAX_NO_OF_GROUPS; i++) left->groupLen[i] = 0;
        }
        if (right->lastWindowSequence != REF_SHORT_WINDOW) {
          right->noOfGroups = 1;
          right->groupLen[0] = 1;
          for (i = 1; i < REF_MAX_NO_OF_GROUPS; i++) right->groupLen[i] = 0;
        }
      }
    } else {
      if (left->lastWindowSequence != REF_SHORT_WINDOW) {
        left->noOfGroups = 1;
        left->groupLen[0] = 1;
        for (i = 1; i < REF_MAX_NO_OF_GROUPS; i++) left->groupLen[i] = 0;
      }
    }
  }

  if (!left->allowShortFrames) {
    if (left->lastWindowSequence != REF_LONG_WINDOW &&
        left->lastWindowSequence != REF_STOP_WINDOW) {
      left->lastWindowSequence = REF_LONG_WINDOW;
      left->windowShape = REF_LOL_WINDOW;
    }
  }
  if (nChannels == 2) {
    if (!right->allowShortFrames) {
      if (right->lastWindowSequence != REF_LONG_WINDOW &&
          right->lastWindowSequence != REF_STOP_WINDOW) {
        right->lastWindowSequence = REF_LONG_WINDOW;
        right->windowShape = REF_LOL_WINDOW;
      }
    }
  }

  return 0;
}

/* ======================================================================== */
/* PHAN E - Kiem tra tinh chat                                               */
/* ======================================================================== */

/*
 * Rang buoc overlap cua MPEG-4 AAC: suon phai cua frame n phai khop suon trai
 * cua frame n+1.
 *   LONG  (suon phai dai)   -> ke tiep phai bat dau bang suon dai : LONG, START
 *   START (suon phai ngan)  -> ke tiep phai bat dau bang suon ngan: SHORT, STOP
 *   SHORT (suon phai ngan)  -> SHORT, STOP
 *   STOP  (suon phai dai)   -> LONG, START
 * Voi AAC-LD (khong SHORT) chi con LONG/START/STOP + LOWOV.
 */
int RefBs_IsLegalTransition(int prevSeq, int curSeq, int allowShortFrames) {
  if (curSeq == REF_WRONG_WINDOW) return 0;

  if (!allowShortFrames) {
    /* LD: LOWOV duoc phep, SHORT thi khong */
    if (curSeq == REF_SHORT_WINDOW) return 0;
    return 1;
  }

  switch (prevSeq) {
    case REF_LONG_WINDOW:
    case REF_STOP_WINDOW:
      return (curSeq == REF_LONG_WINDOW) || (curSeq == REF_START_WINDOW);
    case REF_START_WINDOW:
    case REF_SHORT_WINDOW:
      return (curSeq == REF_SHORT_WINDOW) || (curSeq == REF_STOP_WINDOW);
    default:
      return 0;
  }
}

const char *RefBs_SeqName(int seq) {
  switch (seq) {
    case REF_LONG_WINDOW:
      return "LONG ";
    case REF_START_WINDOW:
      return "START";
    case REF_SHORT_WINDOW:
      return "SHORT";
    case REF_STOP_WINDOW:
      return "STOP ";
    case REF_LOWOV_WINDOW:
      return "LOWOV";
    case REF_WRONG_WINDOW:
      return "WRONG";
    default:
      return "?????";
  }
}

const char *RefBs_ShapeName(int shape) {
  switch (shape) {
    case REF_SINE_WINDOW:
      return "SINE";
    case REF_KBD_WINDOW:
      return "KBD ";
    case REF_LOL_WINDOW:
      return "LOL ";
    default:
      return "????";
  }
}
