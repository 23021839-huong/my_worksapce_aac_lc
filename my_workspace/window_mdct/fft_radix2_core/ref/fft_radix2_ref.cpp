/*
 * Portable bit-exact reference for FDK_AACLC_1024_Q31Q15_RAD2_V1.
 *
 * This model deliberately avoids host-dependent signed overflow and negative
 * right-shift behavior.  It mirrors the RTL stage order, Q31/Q15 truncation,
 * unity/-j bypasses and stage scaling.  It reads the authoritative input suite
 * and produces independent C++ output/trace files; only the Python generator
 * in tools/gen_fft_vectors.py is allowed to create the golden suite.
 *
 * File formats:
 *   input_fft.txt:
 *     mode frame idx re_hex im_hex
 *   output_fft_cpp.txt:
 *     mode frame idx re_hex im_hex scale_exp
 *   output_fft_cpp_stage.txt:
 *     mode frame stage pair addr_a addr_b a_re a_im b_re b_im
 */

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "fdk_sinetable512_q15.h"

namespace {

constexpr int kModeFft64 = 0;
constexpr int kModeFft512 = 1;

struct ComplexQ31 {
  int32_t re;
  int32_t im;
};

struct TwiddleMagnitude {
  int16_t cos_mag;
  int16_t sin_mag;
  bool cos_negative;
};

// ---------------------------------------------------------------------------
// SECTION 1: portable fixed-point primitives.
// ---------------------------------------------------------------------------
int32_t wrap32(int64_t value) {
  const uint32_t raw = static_cast<uint32_t>(static_cast<uint64_t>(value));
  if ((raw & UINT32_C(0x80000000)) == 0) {
    return static_cast<int32_t>(raw);
  }
  return static_cast<int32_t>(static_cast<int64_t>(raw) - INT64_C(0x100000000));
}

int64_t arithmeticShiftRight(int64_t value, unsigned shift) {
  if (shift == 0) return value;
  const int64_t divisor = INT64_C(1) << shift;
  if (value >= 0) return value / divisor;
  // C++ division truncates toward zero; FDK/RTL arithmetic shift floors.
  return -(((-value) + divisor - 1) / divisor);
}

int32_t asr32(int32_t value, unsigned shift) {
  return wrap32(arithmeticShiftRight(value, shift));
}

int32_t mulDiv2Q31Q15(int32_t data, int16_t coefficient) {
  const int64_t product = static_cast<int64_t>(data) * coefficient;
  return wrap32(arithmeticShiftRight(product, 16));
}

int bitReverse(int value, int bits) {
  int result = 0;
  for (int bit = 0; bit < bits; ++bit) {
    result = (result << 1) | (value & 1);
    value >>= 1;
  }
  return result;
}

// ---------------------------------------------------------------------------
// SECTION 2: FDK Q15 twiddle decode and exact special-angle arithmetic.
// ---------------------------------------------------------------------------
TwiddleMagnitude decodeTwiddle(unsigned phase) {
  if (phase > 255) {
    std::fprintf(stderr, "internal error: phase index exceeds 255\n");
    std::abort();
  }

  unsigned base = 0;
  bool swap = false;
  bool cosNegative = false;
  if (phase <= 64) {
    base = phase;
  } else if (phase <= 128) {
    base = 128 - phase;
    swap = true;
  } else if (phase <= 192) {
    base = phase - 128;
    swap = true;
    cosNegative = true;
  } else {
    base = 256 - phase;
    cosNegative = true;
  }

  const FdkTwiddleQ15 entry = kFdkSineTable512Octant[base];
  return swap ? TwiddleMagnitude{entry.sin_q15, entry.cos_q15, cosNegative}
              : TwiddleMagnitude{entry.cos_q15, entry.sin_q15, cosNegative};
}

int32_t applySignAfterTruncation(int32_t value, bool negative) {
  return negative ? wrap32(-static_cast<int64_t>(value)) : value;
}

ComplexQ31 multiplyByForwardTwiddle(ComplexQ31 b, unsigned phase) {
  if (phase == 0) {
    // Stage >=3 W=1 branch in FDK shifts B directly instead of multiplying by
    // the Q15 approximation 0x7FFF.
    return {asr32(b.re, 1), asr32(b.im, 1)};
  }
  if (phase == 128) {
    // Exact -j bypass used by FDK at every stage.  Negate after shifting.
    return {asr32(b.im, 1), wrap32(-static_cast<int64_t>(asr32(b.re, 1)))};
  }

  const TwiddleMagnitude w = decodeTwiddle(phase);
  const int32_t brCos = applySignAfterTruncation(
      mulDiv2Q31Q15(b.re, w.cos_mag), w.cos_negative);
  const int32_t biCos = applySignAfterTruncation(
      mulDiv2Q31Q15(b.im, w.cos_mag), w.cos_negative);
  const int32_t brSin = mulDiv2Q31Q15(b.re, w.sin_mag);
  const int32_t biSin = mulDiv2Q31Q15(b.im, w.sin_mag);

  return {
      wrap32(static_cast<int64_t>(brCos) + biSin),
      wrap32(static_cast<int64_t>(biCos) - brSin),
  };
}

uint32_t rawBits(int32_t value) {
  return static_cast<uint32_t>(value);
}

void writeTraceRow(FILE *trace, int mode, int frame, int stage, int pair,
                   int addrA, int addrB, const ComplexQ31 &top,
                   const ComplexQ31 &bottom) {
  if (!trace) return;
  std::fprintf(trace, "%d %d %d %d %d %d %08X %08X %08X %08X\n", mode,
               frame, stage, pair, addrA, addrB, rawBits(top.re),
               rawBits(top.im), rawBits(bottom.re), rawBits(bottom.im));
}

// ---------------------------------------------------------------------------
// SECTION 3: pure radix-2 DIT transform, kept statement-aligned with RTL.
// ---------------------------------------------------------------------------
int fftFixed(std::vector<ComplexQ31> &samples, int mode, int frame, FILE *trace) {
  const int n = mode == kModeFft512 ? 512 : 64;
  const int ldn = mode == kModeFft512 ? 9 : 6;
  if (static_cast<int>(samples.size()) != n) {
    std::fprintf(stderr, "internal error: vector length does not match mode\n");
    return -1;
  }

  std::vector<ComplexQ31> source(n);
  std::vector<ComplexQ31> destination(n);
  for (int i = 0; i < n; ++i) source[bitReverse(i, ldn)] = samples[i];

  for (int stage = 1; stage <= ldn; ++stage) {
    const int m = 1 << stage;
    const int half = m >> 1;
    const int phaseStep = 1 << (9 - stage);
    int pair = 0;

    for (int groupBase = 0; groupBase < n; groupBase += m) {
      int phase = 0;
      for (int j = 0; j < half; ++j, phase += phaseStep, ++pair) {
        const int addrA = groupBase + j;
        const int addrB = addrA + half;
        const ComplexQ31 a = source[addrA];
        const ComplexQ31 b = source[addrB];
        ComplexQ31 top{};
        ComplexQ31 bottom{};

        if (stage == 1) {
          top.re = wrap32(arithmeticShiftRight(static_cast<int64_t>(a.re) + b.re, 1));
          top.im = wrap32(arithmeticShiftRight(static_cast<int64_t>(a.im) + b.im, 1));
          bottom.re = wrap32(static_cast<int64_t>(top.re) - b.re);
          bottom.im = wrap32(static_cast<int64_t>(top.im) - b.im);
        } else {
          ComplexQ31 u{};
          ComplexQ31 tValue{};
          if (stage == 2) {
            u = a;
            if (phase == 0) {
              tValue = b;
            } else {
              if (phase != 128) {
                std::fprintf(stderr, "internal error: invalid stage-2 phase\n");
                return -1;
              }
              tValue = {b.im, wrap32(-static_cast<int64_t>(b.re))};
            }
          } else {
            u = {asr32(a.re, 1), asr32(a.im, 1)};
            tValue = multiplyByForwardTwiddle(b, static_cast<unsigned>(phase));
          }
          top = {wrap32(static_cast<int64_t>(u.re) + tValue.re),
                 wrap32(static_cast<int64_t>(u.im) + tValue.im)};
          bottom = {wrap32(static_cast<int64_t>(u.re) - tValue.re),
                    wrap32(static_cast<int64_t>(u.im) - tValue.im)};
        }

        destination[addrA] = top;
        destination[addrB] = bottom;
        writeTraceRow(trace, mode, frame, stage, pair, addrA, addrB, top, bottom);
      }
    }
    source.swap(destination);
  }

  samples.swap(source);
  return ldn - 1;
}

// ---------------------------------------------------------------------------
// SECTION 4: vector-file parser, writers and command-line entry points.
// ---------------------------------------------------------------------------
bool parseHexQ31(const std::string &text, int32_t *value) {
  if (text.empty() || text.size() > 8) return false;
  errno = 0;
  char *end = nullptr;
  const unsigned long raw = std::strtoul(text.c_str(), &end, 16);
  if (errno != 0 || end == text.c_str() || *end != '\0' || raw > UINT32_MAX)
    return false;
  *value = wrap32(static_cast<int64_t>(static_cast<uint32_t>(raw)));
  return true;
}

std::string siblingPath(const std::string &inputPath, const char *name) {
  const std::string::size_type separator = inputPath.find_last_of("/\\");
  if (separator == std::string::npos) return name;
  return inputPath.substr(0, separator + 1) + name;
}

int transformFile(const char *inputPath, const char *outputPath,
                  const char *tracePath) {
  std::ifstream input(inputPath);
  if (!input) {
    std::fprintf(stderr, "cannot open input: %s\n", inputPath);
    return 1;
  }
  FILE *output = std::fopen(outputPath, "w");
  FILE *trace = tracePath ? std::fopen(tracePath, "w") : nullptr;
  if (!output || (tracePath && !trace)) {
    std::fprintf(stderr, "cannot create output/trace file\n");
    if (output) std::fclose(output);
    if (trace) std::fclose(trace);
    return 1;
  }

  int recordCount = 0;
  int frameCount = 0;
  int mode = 0;
  int frame = 0;
  int index = 0;
  std::string reHex;
  std::string imHex;
  while (input >> mode >> frame >> index >> reHex >> imHex) {
    if ((mode != kModeFft64 && mode != kModeFft512) || frame < 0 || index != 0) {
      std::fprintf(stderr, "record %d: invalid frame header\n", recordCount + 1);
      return 1;
    }
    const int n = mode == kModeFft512 ? 512 : 64;
    std::vector<ComplexQ31> samples(n);
    if (!parseHexQ31(reHex, &samples[0].re) ||
        !parseHexQ31(imHex, &samples[0].im)) {
      std::fprintf(stderr, "record %d: invalid Q31 hex\n", recordCount + 1);
      return 1;
    }
    ++recordCount;

    for (int i = 1; i < n; ++i) {
      int nextMode = 0;
      int nextFrame = 0;
      int nextIndex = 0;
      if (!(input >> nextMode >> nextFrame >> nextIndex >> reHex >> imHex) ||
          nextMode != mode || nextFrame != frame || nextIndex != i ||
          !parseHexQ31(reHex, &samples[i].re) ||
          !parseHexQ31(imHex, &samples[i].im)) {
        std::fprintf(stderr, "frame %d: malformed record at idx=%d\n", frame, i);
        return 1;
      }
      ++recordCount;
    }

    const int scaleExp = fftFixed(samples, mode, frame, trace);
    if (scaleExp < 0) return 1;
    for (int i = 0; i < n; ++i) {
      std::fprintf(output, "%d %d %d %08X %08X %d\n", mode, frame, i,
                   rawBits(samples[i].re), rawBits(samples[i].im), scaleExp);
    }
    ++frameCount;
  }

  if (!input.eof() || frameCount == 0) {
    std::fprintf(stderr, "input is malformed or empty\n");
    return 1;
  }
  std::fclose(output);
  if (trace) std::fclose(trace);
  std::printf("profile      : FDK_AACLC_1024_Q31Q15_RAD2_V1\n");
  std::printf("twiddle hash : %s\n", kFdkSineTable512OctantSha256);
  std::printf("frames       : %d\nrecords      : %d\n", frameCount, recordCount);
  return 0;
}

void usage(const char *program) {
  std::fprintf(stderr,
      "usage:\n"
      "  %s --input input_fft.txt [--output output_fft_cpp.txt] "
      "[--trace output_fft_cpp_stage.txt]\n",
      program);
}

}  // namespace

int main(int argc, char **argv) {
  const char *inputPath = nullptr;
  const char *outputPath = nullptr;
  const char *tracePath = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--input") && i + 1 < argc)
      inputPath = argv[++i];
    else if (!std::strcmp(argv[i], "--output") && i + 1 < argc)
      outputPath = argv[++i];
    else if (!std::strcmp(argv[i], "--trace") && i + 1 < argc)
      tracePath = argv[++i];
    else {
      usage(argv[0]);
      return 2;
    }
  }

  if (!inputPath) {
    usage(argv[0]);
    return 2;
  }

  const std::string defaultOutput = siblingPath(inputPath, "output_fft_cpp.txt");
  const std::string defaultTrace =
      siblingPath(inputPath, "output_fft_cpp_stage.txt");
  return transformFile(inputPath, outputPath ? outputPath : defaultOutput.c_str(),
                       tracePath ? tracePath : defaultTrace.c_str());
}
