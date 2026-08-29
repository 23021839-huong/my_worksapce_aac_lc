/*
 * dump_wav_block_switch.cpp
 *
 * Read a real PCM16 mono WAV/raw file, feed it frame-by-frame to the
 * independent fixed-point reference model, and dump:
 *   - input.txt       : PCM samples by frame/index
 *   - out_blocksw.txt : human-readable block switching output
 *   - out_blocksw.csv : machine-readable block switching output
 *   - out_window_mdct_control.txt : block type/window shape for Window/MDCT
 *   - meta.txt        : source/output metadata
 *
 * Usage:
 *   ./dump_wav_block_switch --wav ../../input/abc_votay.wav --out out/abc_votay_blockswitch
 *   ./dump_wav_block_switch --raw out/abc_votay.raw --out out/abc_votay_blockswitch
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "../ref/ref_block_switch.h"

#ifdef _WIN32
static const char PATH_SEP = '\\';
static int makeOneDirectory(const char *path) { return _mkdir(path); }
#else
static const char PATH_SEP = '/';
static int makeOneDirectory(const char *path) { return mkdir(path, 0755); }
#endif

typedef struct {
  int sampleRate;
  int channels;
  int bitsPerSample;
  long dataOffset;
  long dataBytes;
} WavInfo;

static uint16_t rd16(FILE *f) {
  unsigned char b[2];
  if (fread(b, 1, 2, f) != 2) return 0;
  return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t rd32(FILE *f) {
  unsigned char b[4];
  if (fread(b, 1, 4, f) != 4) return 0;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

static int readWavPcm16Mono(const char *path, int16_t **pcmOut, int *nSamplesOut,
                            WavInfo *infoOut) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;

  char id[5] = {0};
  fread(id, 1, 4, f);
  (void)rd32(f);
  char wave[5] = {0};
  fread(wave, 1, 4, f);
  if (strcmp(id, "RIFF") || strcmp(wave, "WAVE")) {
    fclose(f);
    return -2;
  }

  WavInfo wi;
  memset(&wi, 0, sizeof(wi));
  int haveFmt = 0, haveData = 0;

  while (!feof(f)) {
    char cid[5] = {0};
    if (fread(cid, 1, 4, f) != 4) break;
    uint32_t sz = rd32(f);
    long payload = ftell(f);

    if (!strcmp(cid, "fmt ")) {
      uint16_t audioFormat = rd16(f);
      wi.channels = (int)rd16(f);
      wi.sampleRate = (int)rd32(f);
      (void)rd32(f); /* byteRate */
      (void)rd16(f); /* blockAlign */
      wi.bitsPerSample = (int)rd16(f);
      if (audioFormat != 1) {
        fclose(f);
        return -3; /* not PCM */
      }
      haveFmt = 1;
    } else if (!strcmp(cid, "data")) {
      wi.dataOffset = payload;
      wi.dataBytes = (long)sz;
      haveData = 1;
      break;
    }

    fseek(f, payload + (long)sz + (sz & 1), SEEK_SET);
  }

  if (!haveFmt || !haveData) {
    fclose(f);
    return -4;
  }
  if (wi.channels != 1 || wi.bitsPerSample != 16) {
    fclose(f);
    return -5;
  }

  int nSamples = (int)(wi.dataBytes / 2);
  int16_t *pcm = (int16_t *)malloc((size_t)nSamples * sizeof(int16_t));
  if (!pcm) {
    fclose(f);
    return -6;
  }

  fseek(f, wi.dataOffset, SEEK_SET);
  if (fread(pcm, sizeof(int16_t), (size_t)nSamples, f) != (size_t)nSamples) {
    free(pcm);
    fclose(f);
    return -7;
  }

  fclose(f);
  *pcmOut = pcm;
  *nSamplesOut = nSamples;
  *infoOut = wi;
  return 0;
}

static int readRawPcm16Mono(const char *path, int16_t **pcmOut, int *nSamplesOut,
                            WavInfo *infoOut) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long bytes = ftell(f);
  fseek(f, 0, SEEK_SET);

  int nSamples = (int)(bytes / 2);
  int16_t *pcm = (int16_t *)malloc((size_t)nSamples * sizeof(int16_t));
  if (!pcm) {
    fclose(f);
    return -6;
  }
  if (fread(pcm, sizeof(int16_t), (size_t)nSamples, f) != (size_t)nSamples) {
    free(pcm);
    fclose(f);
    return -7;
  }
  fclose(f);

  memset(infoOut, 0, sizeof(*infoOut));
  infoOut->sampleRate = 48000;
  infoOut->channels = 1;
  infoOut->bitsPerSample = 16;
  infoOut->dataOffset = 0;
  infoOut->dataBytes = bytes;

  *pcmOut = pcm;
  *nSamplesOut = nSamples;
  return 0;
}

static const char *seqName(int seq) {
  switch (seq) {
    case REF_LONG_WINDOW:
      return "LONG";
    case REF_START_WINDOW:
      return "START";
    case REF_SHORT_WINDOW:
      return "SHORT";
    case REF_STOP_WINDOW:
      return "STOP";
    case REF_LOWOV_WINDOW:
      return "LOWOV";
    default:
      return "WRONG";
  }
}

static const char *shapeName(int shape) {
  switch (shape) {
    case REF_SINE_WINDOW:
      return "SINE";
    case REF_KBD_WINDOW:
      return "KBD";
    case REF_LOL_WINDOW:
      return "LOL";
    default:
      return "UNKNOWN";
  }
}

static int makeDirectories(const char *path) {
  char tmp[512];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(tmp)) return -1;

  memcpy(tmp, path, len + 1);
  for (char *p = tmp + 1; *p; ++p) {
    if (*p != '/' && *p != '\\') continue;
    char saved = *p;
    *p = '\0';
    if (!(strlen(tmp) == 2 && tmp[1] == ':') &&
        makeOneDirectory(tmp) != 0 && errno != EEXIST) {
      return -1;
    }
    *p = saved;
  }
  if (makeOneDirectory(tmp) != 0 && errno != EEXIST) return -1;
  return 0;
}

static int joinPath(char *dst, size_t dstSize, const char *dir,
                    const char *name) {
  size_t len = strlen(dir);
  int hasSep = len > 0 && (dir[len - 1] == '/' || dir[len - 1] == '\\');
  int written = hasSep ? snprintf(dst, dstSize, "%s%s", dir, name)
                       : snprintf(dst, dstSize, "%s%c%s", dir, PATH_SEP, name);
  return (written >= 0 && (size_t)written < dstSize) ? 0 : -1;
}

static void usage(const char *argv0) {
  printf("Usage:\n");
  printf("  %s --wav <pcm16_mono.wav> --out <dir> [--frames N]\n", argv0);
  printf("  %s --raw <pcm16_mono.raw> --out <dir> [--frames N]\n", argv0);
}

int main(int argc, char **argv) {
  const char *wavPath = NULL;
  const char *rawPath = NULL;
  const char *outDir = "out/blocksw_dump";
  int frameLimit = -1;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
      wavPath = argv[++i];
    } else if (!strcmp(argv[i], "--raw") && i + 1 < argc) {
      rawPath = argv[++i];
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      outDir = argv[++i];
    } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
      frameLimit = atoi(argv[++i]);
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  if ((wavPath == NULL) == (rawPath == NULL)) {
    usage(argv[0]);
    return 1;
  }

  int16_t *pcm = NULL;
  int nSamples = 0;
  WavInfo wi;
  int rc = wavPath ? readWavPcm16Mono(wavPath, &pcm, &nSamples, &wi)
                   : readRawPcm16Mono(rawPath, &pcm, &nSamples, &wi);
  if (rc != 0) {
    printf("ERROR: cannot read input (rc=%d). Need PCM 16-bit mono.\n", rc);
    return 1;
  }

  if (makeDirectories(outDir) != 0) {
    printf("ERROR: cannot create output directory %s\n", outDir);
    free(pcm);
    return 1;
  }

  char pInput[512], pTxt[512], pCsv[512], pControl[512], pMeta[512];
  if (joinPath(pInput, sizeof(pInput), outDir, "input.txt") != 0 ||
      joinPath(pTxt, sizeof(pTxt), outDir, "out_blocksw.txt") != 0 ||
      joinPath(pCsv, sizeof(pCsv), outDir, "out_blocksw.csv") != 0 ||
      joinPath(pControl, sizeof(pControl), outDir,
               "out_window_mdct_control.txt") != 0 ||
      joinPath(pMeta, sizeof(pMeta), outDir, "meta.txt") != 0) {
    printf("ERROR: output path is too long\n");
    free(pcm);
    return 1;
  }

  const int granuleLength = 1024;
  int nFrames = nSamples / granuleLength;
  if (frameLimit > 0 && frameLimit < nFrames) nFrames = frameLimit;
  int usedSamples = nFrames * granuleLength;
  int droppedSamples = nSamples - usedSamples;

  FILE *fi = fopen(pInput, "w");
  FILE *ft = fopen(pTxt, "w");
  FILE *fc = fopen(pCsv, "w");
  FILE *fw = fopen(pControl, "w");
  FILE *fm = fopen(pMeta, "w");
  if (!fi || !ft || !fc || !fw || !fm) {
    printf("ERROR: cannot open output files in %s\n", outDir);
    if (fi) fclose(fi);
    if (ft) fclose(ft);
    if (fc) fclose(fc);
    if (fw) fclose(fw);
    if (fm) fclose(fm);
    free(pcm);
    return 1;
  }

  fprintf(fm, "source=%s\n", wavPath ? wavPath : rawPath);
  fprintf(fm, "format=PCM16 mono\n");
  fprintf(fm, "sampleRate=%d\n", wi.sampleRate);
  fprintf(fm, "channels=%d\n", wi.channels);
  fprintf(fm, "bitsPerSample=%d\n", wi.bitsPerSample);
  fprintf(fm, "wavDataOffset=%ld\n", wi.dataOffset);
  fprintf(fm, "wavDataBytes=%ld\n", wi.dataBytes);
  fprintf(fm, "granuleLength=%d\n", granuleLength);
  fprintf(fm, "totalSamples=%d\n", nSamples);
  fprintf(fm, "framesDumped=%d\n", nFrames);
  fprintf(fm, "usedSamples=%d\n", usedSamples);
  fprintf(fm, "droppedTailSamples=%d\n", droppedSamples);
  fprintf(fm, "model=RefBs fixed-point independent reference\n");
  fprintf(fm, "refCoeff16=%d\n", RefBs_UsesCoeff16());
  fprintf(fm, "note=block_switch input is one 1024-sample mono PCM frame at a time\n");

  fprintf(fi, "# source=%s\n", wavPath ? wavPath : rawPath);
  fprintf(fi, "# format: frame sample_in_frame abs_sample pcm_s16\n");
  for (int f = 0; f < nFrames; f++) {
    fprintf(fi, "# frame %d\n", f);
    for (int n = 0; n < granuleLength; n++) {
      int abs = f * granuleLength + n;
      fprintf(fi, "%d %d %d %d\n", f, n, abs, (int)pcm[abs]);
    }
  }

  fprintf(ft, "# Block switching output from RefBs_Process()\n");
  fprintf(ft, "# source=%s\n", wavPath ? wavPath : rawPath);
  fprintf(ft, "# frame | attack attackIndex | seq shape | noOfGroups groupLen[0..3] | maxWindowNrg accWindowNrg\n");

  fprintf(fc,
          "frame,attack,attackIndex,lastWindowSequence,seq,windowShape,shape,"
          "noOfGroups,groupLen0,groupLen1,groupLen2,groupLen3,maxWindowNrg,"
          "accWindowNrg");
  for (int w = 0; w < REF_BLOCK_SWITCH_WINDOWS; w++)
    fprintf(fc, ",windowNrg%d", w);
  for (int w = 0; w < REF_BLOCK_SWITCH_WINDOWS; w++)
    fprintf(fc, ",windowNrgF%d", w);
  fprintf(fc, "\n");

  fprintf(fw,
          "# frame blockType blockTypeName windowShape windowShapeName "
          "noOfGroups groupLen0 groupLen1 groupLen2 groupLen3\n");

  RefBsState bsc;
  RefBs_Init(&bsc, 0); /* AAC-LC: 1024 samples, allow short frames */

  for (int f = 0; f < nFrames; f++) {
    const int16_t *frame = pcm + (size_t)f * granuleLength;
    int err = RefBs_Process(&bsc, granuleLength, 0, frame);
    if (err) {
      fprintf(ft, "ERROR frame %d: RefBs_Process returned %d\n", f, err);
      break;
    }

    fprintf(ft,
            "%5d |   %d       %d     | %-5s %-4s |     %d      [%d,%d,%d,%d] "
            "| %11ld %11ld\n",
            f, bsc.attack ? 1 : 0, (int)bsc.attackIndex,
            seqName(bsc.lastWindowSequence), shapeName(bsc.windowShape),
            (int)bsc.noOfGroups, (int)bsc.groupLen[0], (int)bsc.groupLen[1],
            (int)bsc.groupLen[2], (int)bsc.groupLen[3],
            (long)bsc.maxWindowNrg, (long)bsc.accWindowNrg);

    fprintf(ft, "      windowNrg :");
    for (int w = 0; w < REF_BLOCK_SWITCH_WINDOWS; w++)
      fprintf(ft, " %ld", (long)bsc.windowNrg[REF_THIS_WINDOW][w]);
    fprintf(ft, "\n      windowNrgF:");
    for (int w = 0; w < REF_BLOCK_SWITCH_WINDOWS; w++)
      fprintf(ft, " %ld", (long)bsc.windowNrgF[REF_THIS_WINDOW][w]);
    fprintf(ft, "\n");

    fprintf(fc, "%d,%d,%d,%d,%s,%d,%s,%d,%d,%d,%d,%d,%ld,%ld", f,
            bsc.attack ? 1 : 0, (int)bsc.attackIndex,
            (int)bsc.lastWindowSequence, seqName(bsc.lastWindowSequence),
            (int)bsc.windowShape, shapeName(bsc.windowShape),
            (int)bsc.noOfGroups, (int)bsc.groupLen[0], (int)bsc.groupLen[1],
            (int)bsc.groupLen[2], (int)bsc.groupLen[3],
            (long)bsc.maxWindowNrg, (long)bsc.accWindowNrg);
    for (int w = 0; w < REF_BLOCK_SWITCH_WINDOWS; w++)
      fprintf(fc, ",%ld", (long)bsc.windowNrg[REF_THIS_WINDOW][w]);
    for (int w = 0; w < REF_BLOCK_SWITCH_WINDOWS; w++)
      fprintf(fc, ",%ld", (long)bsc.windowNrgF[REF_THIS_WINDOW][w]);
    fprintf(fc, "\n");

    fprintf(fw, "%d %d %s %d %s %d %d %d %d %d\n", f,
            (int)bsc.lastWindowSequence, seqName(bsc.lastWindowSequence),
            (int)bsc.windowShape, shapeName(bsc.windowShape),
            (int)bsc.noOfGroups,
            (int)bsc.groupLen[0], (int)bsc.groupLen[1],
            (int)bsc.groupLen[2], (int)bsc.groupLen[3]);
  }

  fclose(fi);
  fclose(ft);
  fclose(fc);
  fclose(fw);
  fclose(fm);
  free(pcm);

  printf("OK: dumped %d frame(s) to %s\n", nFrames, outDir);
  printf("  %s\n", pInput);
  printf("  %s\n", pTxt);
  printf("  %s\n", pCsv);
  printf("  %s\n", pControl);
  printf("  %s\n", pMeta);
  return 0;
}
