/*
 * ref_block_switch.h
 *
 * REFERENCE MODEL doc lap cho khoi Block Switching cua FDK-AAC.
 *
 * Muc dich: mo ta lai thuat toan cua libAACenc/src/block_switch.cpp bang mot
 * hien thuc HOAN TOAN DOC LAP (khong include bat ky header nao cua fdk-aac),
 * de dung lam "vang" (golden reference) kiem chung DUT.
 *
 * Hai muc do mo hinh:
 *   1. FIXED  (RefBs*)  - bit-exact: tai tao dung tung phep toan fixed-point
 *                         Q31 cua FDK. Ky vong: TRUNG 100% moi truong trang thai.
 *   2. FLOAT  (RefBsF*) - mo hinh hanh vi bang double, dung de doi chieu o muc
 *                         QUYET DINH (attack / window sequence), va de doc hieu
 *                         thuat toan o dang toan hoc thuan.
 *
 * Moi ten deu co tien to REF_/RefBs de khong dung do voi header cua fdk-aac
 * khi ca hai cung duoc include trong mot file.
 */

#ifndef REF_BLOCK_SWITCH_H
#define REF_BLOCK_SWITCH_H

#include <stdint.h>

/* ------------------------------------------------------------------------ *
 * REF_COEFF_16BIT - chon nhanh hang so cua block_switch.cpp
 *
 * block_switch.cpp co #ifndef SINETABLE_16BIT / #else: he so bo loc IIR va
 * nguong attack la Q31 hay Q15 tuy kien truc. FDK_archdef.h bat
 * SINETABLE_16BIT cho x86 (ca x86-64), ARM, RISC-V, s390x, sparc; tat cho
 * MIPS va PowerPC.
 *
 * => Ket qua bit-level cua khoi PHU THUOC KIEN TRUC. Mo hinh tham chieu phai
 *    chon dung nhanh thi moi bit-exact. Ghi de bang -DREF_COEFF_16BIT=0/1.
 * ------------------------------------------------------------------------ */
#ifndef REF_COEFF_16BIT
#if defined(__mips__) || defined(__powerpc__)
#define REF_COEFF_16BIT 0
#else
#define REF_COEFF_16BIT 1
#endif
#endif

/* ---------------- Hang so (soi guong block_switch.h / psy_const.h) -------- */

#define REF_BLOCK_SWITCH_WINDOWS 8    /* so sub-window toi da              */
#define REF_BLOCK_SWITCHING_IIR_LEN 2 /* bac bo loc high-pass              */
#define REF_BLOCK_SWITCH_ENERGY_SHIFT 7
#define REF_MAX_NO_OF_GROUPS 4
#define REF_TRANS_FAC 8
#define REF_N_BLOCKTYPES 6

#define REF_LAST_WINDOW 0
#define REF_THIS_WINDOW 1

/* Block types - dung gia tri so giong psy_const.h */
enum {
  REF_LONG_WINDOW = 0,
  REF_START_WINDOW = 1,
  REF_SHORT_WINDOW = 2,
  REF_STOP_WINDOW = 3,
  REF_LOWOV_WINDOW = 4,
  REF_WRONG_WINDOW = 5
};

/* Window shapes */
enum { REF_SINE_WINDOW = 0, REF_KBD_WINDOW = 1, REF_LOL_WINDOW = 2 };

/* ------------------------------------------------------------------------ */
/* 1. MO HINH FIXED-POINT (bit-exact)                                        */
/* ------------------------------------------------------------------------ */

/* Anh xa 1-1 voi BLOCK_SWITCHING_CONTROL trong block_switch.h */
typedef struct {
  int32_t lastWindowSequence;
  int32_t windowShape;
  int32_t lastWindowShape;
  uint32_t nBlockSwitchWindows;
  int32_t attack;
  int32_t lastattack;
  int32_t attackIndex;
  int32_t lastAttackIndex;
  int32_t allowShortFrames;
  int32_t allowLookAhead;
  int32_t noOfGroups;
  int32_t groupLen[REF_MAX_NO_OF_GROUPS];
  int32_t maxWindowNrg;

  int32_t windowNrg[2][REF_BLOCK_SWITCH_WINDOWS];  /* Q31 */
  int32_t windowNrgF[2][REF_BLOCK_SWITCH_WINDOWS]; /* Q31 */
  int32_t accWindowNrg;                            /* Q31 */

  int32_t iirStates[REF_BLOCK_SWITCHING_IIR_LEN];
} RefBsState;

void RefBs_Init(RefBsState *st, int isLowDelay);

int RefBs_Process(RefBsState *st, int granuleLength, int isLFE,
                  const int16_t *pTimeSignal);

int RefBs_Sync(RefBsState *left, RefBsState *right, int nChannels,
               int commonWindow);

/* Hang so noi bo - expose de harness in ra / kiem tra */
int32_t RefBs_GetMinAttackNrg(void);
int32_t RefBs_GetHiPassCoeff(int idx);
int32_t RefBs_GetAccWindowNrgFac(void);
int32_t RefBs_GetInvAttackRatio(void);
int RefBs_GetWindowShapeFor(int allowShortFrames, int blockType);

/* 1 neu mo hinh dang dung nhanh he so Q15 (tuong ung SINETABLE_16BIT) */
int RefBs_UsesCoeff16(void);

/* ------------------------------------------------------------------------ */
/* 2. MO HINH FLOAT (behavioral / spec-level)                                */
/* ------------------------------------------------------------------------ */

typedef struct {
  int lastWindowSequence;
  int windowShape;
  unsigned nBlockSwitchWindows;
  int attack;
  int lastattack;
  int attackIndex;
  int lastAttackIndex;
  int allowShortFrames;
  int allowLookAhead;
  int noOfGroups;
  int groupLen[REF_MAX_NO_OF_GROUPS];
  double maxWindowNrg;

  double windowNrg[2][REF_BLOCK_SWITCH_WINDOWS];
  double windowNrgF[2][REF_BLOCK_SWITCH_WINDOWS];
  double accWindowNrg;

  double iirStates[REF_BLOCK_SWITCHING_IIR_LEN];
} RefBsStateF;

void RefBsF_Init(RefBsStateF *st, int isLowDelay);

int RefBsF_Process(RefBsStateF *st, int granuleLength, int isLFE,
                   const int16_t *pTimeSignal);

int RefBsF_Sync(RefBsStateF *left, RefBsStateF *right, int nChannels,
                int commonWindow);

/* ------------------------------------------------------------------------ */
/* 3. Kiem tra tinh chat (property checks) - dung chung cho DUT lan REF      */
/* ------------------------------------------------------------------------ */

/* Chuyen trang thai window sequence co hop le khong?
 * prevSeq -> curSeq theo dung rang buoc overlap cua MPEG-4 AAC.
 * Tra 1 = hop le. */
int RefBs_IsLegalTransition(int prevSeq, int curSeq, int allowShortFrames);

/* Ten de in */
const char *RefBs_SeqName(int seq);
const char *RefBs_ShapeName(int shape);

#endif /* REF_BLOCK_SWITCH_H */
