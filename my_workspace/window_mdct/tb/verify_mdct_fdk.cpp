/*
 * End-to-end compatibility check against the original FDK mdct_block/dct_IV.
 *
 * Only the FFT dispatcher is narrowed to the two AAC-LC sizes used here; it
 * calls FDK's own fused dit_fft kernel.  Window/fold, pre/post rotation, state,
 * ROM literals and fixed arithmetic remain the unmodified FDK implementation.
 */

#include "../ref/fixed_mdct_radix2.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "FDK_tools_rom.h"
#include "fft_rad2.h"
#include "mdct.h"

/* dct.cpp calls this symbol.  The full FDK dispatcher supports many unrelated
 * mixed-radix sizes; this verification target deliberately exposes only the
 * FFT64/FFT512 contract of the AAC-LC MDCT core. */
void fft(int length, FIXP_DBL* input, INT* scale_factor) {
  if (length == 64) {
    dit_fft(input, 6, SineTable512, 512);
    *scale_factor += 5;
  } else if (length == 512) {
    dit_fft(input, 9, SineTable512, 512);
    *scale_factor += 8;
  } else {
    std::fprintf(stderr, "unsupported FFT length in verify_mdct_fdk: %d\n",
                 length);
    std::abort();
  }
}

namespace {

namespace ref = fixed_mdct_radix2;

constexpr std::array<ref::BlockType, 6> kBlocks = {
    ref::BlockType::kLong,  ref::BlockType::kLong,
    ref::BlockType::kStart, ref::BlockType::kShort,
    ref::BlockType::kStop,  ref::BlockType::kLong,
};
constexpr std::array<ref::WindowShape, 6> kShapes = {
    ref::WindowShape::kKbd,  ref::WindowShape::kKbd,
    ref::WindowShape::kSine, ref::WindowShape::kSine,
    ref::WindowShape::kKbd,  ref::WindowShape::kKbd,
};

std::int16_t ToSigned16(std::uint32_t value) {
  const std::uint16_t bits = static_cast<std::uint16_t>(value);
  return static_cast<std::int16_t>(
      (bits & UINT16_C(0x8000)) ? static_cast<std::int32_t>(bits) - 65536
                                : static_cast<std::int32_t>(bits));
}

std::array<std::int16_t, 2048> MakePcm(int frame) {
  std::array<std::int16_t, 2048> pcm{};
  std::uint32_t state = UINT32_C(0x4D444354) ^
                        (UINT32_C(0x9E3779B9) * static_cast<unsigned>(frame + 1));
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    pcm[i] = ToSigned16(state >> 16);
  }

  /* Directed edge values around the eight-short placement boundaries. */
  constexpr std::array<int, 8> kEdges = {0, 447, 448, 575, 576, 1599, 1600,
                                          2047};
  for (std::size_t i = 0; i < kEdges.size(); ++i) {
    pcm[kEdges[i]] = ((frame + static_cast<int>(i)) & 1) ? INT16_MIN : INT16_MAX;
  }
  return pcm;
}

int RightSlope(ref::BlockType block) {
  return (block == ref::BlockType::kStart || block == ref::BlockType::kShort)
             ? 128
             : 1024;
}

}  // namespace

int main() {
  static_assert(sizeof(INT_PCM) == sizeof(std::int16_t),
                "verification profile requires PCM16");
  static_assert(sizeof(FIXP_DBL) == sizeof(std::int32_t),
                "verification profile requires Q31 FIXP_DBL");
  static_assert(sizeof(FIXP_WTB) == sizeof(std::int16_t),
                "verification profile requires WINDOWTABLE_16BIT");

  ref::WindowState reference_state;
  mdct_t fdk_state{};
  mdct_init(&fdk_state, nullptr, 0);

  std::size_t checked_bins = 0;
  for (std::size_t frame = 0; frame < kBlocks.size(); ++frame) {
    const auto pcm = MakePcm(static_cast<int>(frame));
    ref::FrameTrace expected;
    std::string error;
    if (!ref::ProcessAacLc1024(pcm.data(), pcm.size(), kBlocks[frame],
                               kShapes[frame], &reference_state, &expected,
                               &error)) {
      std::fprintf(stderr, "reference failed at frame %zu: %s\n", frame,
                   error.c_str());
      return 2;
    }

    const int n_spec = kBlocks[frame] == ref::BlockType::kShort ? 8 : 1;
    const int transform_length =
        kBlocks[frame] == ref::BlockType::kShort ? 128 : 1024;
    const int right_slope = RightSlope(kBlocks[frame]);
    std::array<FIXP_DBL, 1024> actual{};
    std::array<SHORT, 8> actual_exp{};
    const int consumed = mdct_block(
        &fdk_state, reinterpret_cast<const INT_PCM*>(pcm.data()), 1024,
        actual.data(), n_spec, transform_length,
        FDKgetWindowSlope(right_slope, static_cast<int>(kShapes[frame])),
        right_slope, actual_exp.data());
    if (consumed != 1024) {
      std::fprintf(stderr, "FDK consumed %d samples at frame %zu\n", consumed,
                   frame);
      return 2;
    }

    const int expected_exp = kBlocks[frame] == ref::BlockType::kShort ? 9 : 12;
    for (int sub = 0; sub < n_spec; ++sub) {
      if (actual_exp[sub] != expected_exp) {
        std::fprintf(stderr,
                     "exponent mismatch frame=%zu sub=%d got=%d expected=%d\n",
                     frame, sub, actual_exp[sub], expected_exp);
        return 1;
      }
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
      if (static_cast<std::int32_t>(actual[index]) !=
          expected.final_spectrum[index]) {
        std::fprintf(
            stderr,
            "MDCT mismatch frame=%zu index=%zu got=%08X expected=%08X\n",
            frame, index, static_cast<std::uint32_t>(actual[index]),
            static_cast<std::uint32_t>(expected.final_spectrum[index]));
        return 1;
      }
      ++checked_bins;
    }
  }

  std::printf(
      "PASS: pure-radix2 reference == original FDK mdct_block at 0 LSB "
      "(%zu bins, LONG/START/8SHORT/STOP, exponents 12/9)\n",
      checked_bins);
  return 0;
}
