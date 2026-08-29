#ifndef FIXED_MDCT_RADIX2_H
#define FIXED_MDCT_RADIX2_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fixed_mdct_radix2 {

// Fixed contract implemented by this model.  It is intentionally narrower
// than the complete FDK API: AAC-LC frameLength=1024, transform lengths 1024
// and 128, signed PCM16 input, Q31 data and literal FDK Q15 coefficients.
extern const char kMdctProfileName[];
extern const char kFftProfileName[];
extern const char kFdkCommit[];
extern const char kFftTwiddleOctantSha256[];

constexpr int kFrameLength = 1024;
constexpr int kTimeSampleCount = 2 * kFrameLength;
constexpr int kLongTransformLength = 1024;
constexpr int kShortTransformLength = 128;
constexpr int kShortTransformCount = 8;

enum class BlockType : std::uint8_t {
  kLong = 0,
  kStart = 1,
  kShort = 2,
  kStop = 3,
};

enum class WindowShape : std::uint8_t {
  kSine = 0,
  kKbd = 1,
};

struct ComplexQ31 {
  std::int32_t re = 0;
  std::int32_t im = 0;
};

// Equivalent state to the forward-MDCT fields prev_wrs/prev_fr/prev_tl.
// The left slope of the next transform is the right slope saved here.
struct WindowState {
  bool initialized = false;
  WindowShape previous_right_shape = WindowShape::kSine;
  int previous_right_slope_length = 0;
  int previous_transform_length = 0;
};

struct SubtransformTrace {
  int subtransform = 0;
  int input_offset = 0;
  int transform_length = 0;
  WindowShape left_shape = WindowShape::kSine;
  WindowShape right_shape = WindowShape::kSine;
  int left_slope_length = 0;
  int right_slope_length = 0;

  // Cumulative exponent at each raw FDK MDCT boundary.  The fold starts at 2,
  // pre-rotation adds 2, the FFT adds 8 (L=1024) or 5 (L=128), and the
  // post-rotation adds 0.  There is no dynamic normalization here; AAC encoder
  // psychoacoustic normalization is a downstream operation.
  int fold_exponent = 2;
  int pre_rotation_exponent = 4;
  int fft_scale_increment = 0;
  int fft_exponent = 0;
  int post_rotation_exponent = 0;
  int final_exponent = 0;

  // All data fields are signed Q1.31 mantissas.  pre_fft and fft contain
  // transform_length/2 complex samples in natural complex-array order.
  std::vector<std::int32_t> fold;
  std::vector<ComplexQ31> pre_fft;
  std::vector<ComplexQ31> fft;
  std::vector<std::int32_t> post;
};

struct FrameTrace {
  BlockType block_type = BlockType::kLong;
  WindowShape requested_right_shape = WindowShape::kSine;
  WindowState state_before;
  WindowState state_after;

  // Always 1024 MDCT lines.  For kShort, eight 128-line spectra are stored at
  // [0..127], [128..255], ... in the same order as FDK mdct_block().
  std::vector<std::int32_t> final_spectrum;
  std::vector<SubtransformTrace> transforms;
};

struct RomFingerprints {
  // FNV-1a/64 over little-endian raw Q15 words, pair order {re, im}.
  std::uint64_t sine_window_1024 = 0;
  std::uint64_t kbd_window_1024 = 0;
  std::uint64_t sine_window_128 = 0;
  std::uint64_t kbd_window_128 = 0;
  std::uint64_t sine_table_1024 = 0;
  std::uint64_t fft_octant_512 = 0;
};

const char* BlockTypeName(BlockType type);
const char* WindowShapeName(WindowShape shape);

// Reads the linked literal tables from libFDK/src/FDK_tools_rom.cpp.  This also
// provides a simple build/runtime check that the Q15 ROM profile is the one
// used to create the golden vectors.
RomFingerprints GetRomFingerprints();

// Process one AAC-LC analysis snapshot.  time_data must contain exactly 2048
// signed PCM16 samples in the layout accepted by FDK mdct_block() with
// noInSamples=1024.  On success, state and trace are updated atomically.
//
// Block mapping:
//   LONG/STOP  -> 1 x L=1024, right slope length 1024
//   START      -> 1 x L=1024, right slope length 128
//   SHORT      -> 8 x L=128,  first input offset 448, stride 128
//
// At an uninitialized state, the left slope is initialized from the current
// right slope exactly like mdct_block().  Returns false without changing state
// or trace if the contract or state is invalid.
bool ProcessAacLc1024(const std::int16_t* time_data,
                      std::size_t time_sample_count,
                      BlockType block_type,
                      WindowShape right_shape,
                      WindowState* state,
                      FrameTrace* trace,
                      std::string* error = nullptr);

}  // namespace fixed_mdct_radix2

#endif  // FIXED_MDCT_RADIX2_H
