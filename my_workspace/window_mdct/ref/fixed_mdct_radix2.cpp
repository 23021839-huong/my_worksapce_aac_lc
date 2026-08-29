/*
 * Fixed-point AAC-LC MDCT oracle for FDK_AACLC_1024_MDCT_Q31Q15_RAD2_V1.
 *
 * This is a clean, portable statement model of the generic FDK mdct_block(),
 * dct_IV() and the already frozen pure-radix-2 FFT profile.  It deliberately
 * does not call the FDK transform functions: only their literal ROM symbols are
 * linked.  Signed overflow and negative right shift are made explicit so the
 * result does not depend on host C++ behavior.
 *
 * Required link input:
 *   libFDK/src/FDK_tools_rom.cpp
 * Required include paths:
 *   libFDK/include, libSYS/include
 */

#include "fixed_mdct_radix2.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "FDK_tools_rom.h"

#if !defined(SINETABLE_16BIT) || !defined(WINDOWTABLE_16BIT)
#error "This model requires the frozen FDK Q15 sine/window ROM profile"
#endif

static_assert(sizeof(SineTable512[0].v.re) == sizeof(std::int16_t),
              "SineTable512 must be Q15");
static_assert(sizeof(SineWindow1024[0].v.re) == sizeof(std::int16_t),
              "AAC-LC window tables must be Q15");

namespace fixed_mdct_radix2 {

const char kMdctProfileName[] =
    "FDK_AACLC_1024_MDCT_Q31Q15_RAD2_V1";
const char kFftProfileName[] = "FDK_AACLC_1024_Q31Q15_RAD2_V1";
const char kFdkCommit[] = "35f9c13cb6df0c5d4e7ba958ef2d251c48b8d1d9";
const char kFftTwiddleOctantSha256[] =
    "95eb626e5d5a6f44fdbd00f5700bb5f8b1b8e9425d20d374412968090d595ad9";

namespace {

constexpr std::uint64_t kFnvOffsetBasis = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);
constexpr std::int16_t kSqrtHalfQ15 = INT16_C(0x5A82);

struct TwiddleMagnitude {
  std::int16_t cos_mag;
  std::int16_t sin_mag;
  bool cos_negative;
};

std::int32_t Wrap32(std::int64_t value) {
  const std::uint32_t raw =
      static_cast<std::uint32_t>(static_cast<std::uint64_t>(value));
  if ((raw & UINT32_C(0x80000000)) == 0) {
    return static_cast<std::int32_t>(raw);
  }
  return static_cast<std::int32_t>(static_cast<std::int64_t>(raw) -
                                   INT64_C(0x100000000));
}

std::int64_t ArithmeticShiftRight(std::int64_t value, unsigned shift) {
  if (shift == 0) return value;
  const std::int64_t divisor = INT64_C(1) << shift;
  if (value >= 0) return value / divisor;
  // C++ division truncates toward zero.  FDK/RTL ASR rounds toward -infinity.
  return -(((-value) + divisor - 1) / divisor);
}

std::int32_t Asr32(std::int32_t value, unsigned shift) {
  return Wrap32(ArithmeticShiftRight(value, shift));
}

std::int32_t Neg32(std::int32_t value) {
  return Wrap32(-static_cast<std::int64_t>(value));
}

std::int32_t Add32(std::int32_t a, std::int32_t b) {
  return Wrap32(static_cast<std::int64_t>(a) + b);
}

std::int32_t Sub32(std::int32_t a, std::int32_t b) {
  return Wrap32(static_cast<std::int64_t>(a) - b);
}

std::int32_t MulDiv2Q31Q15(std::int32_t data, std::int16_t coefficient) {
  const std::int64_t product = static_cast<std::int64_t>(data) * coefficient;
  return Wrap32(ArithmeticShiftRight(product, 16));
}

std::int32_t MulQ31Q15(std::int32_t data, std::int16_t coefficient) {
  // FDK fMult(32x16) is (fMultDiv2(...) << 1), including the truncation
  // before the doubling.
  return Wrap32(static_cast<std::int64_t>(MulDiv2Q31Q15(data, coefficient)) *
                2);
}

std::int32_t MulDiv2PcmQ15(std::int16_t pcm,
                           std::int16_t coefficient) {
  // fMultDiv2(16x16) returns the Q15*Q15 product directly as a Q31 mantissa.
  return static_cast<std::int32_t>(pcm) * coefficient;
}

template <typename PackedPair>
std::int16_t ReQ15(const PackedPair& value) {
  return static_cast<std::int16_t>(value.v.re);
}

template <typename PackedPair>
std::int16_t ImQ15(const PackedPair& value) {
  return static_cast<std::int16_t>(value.v.im);
}

void ComplexMulDiv2(std::int32_t* out_re, std::int32_t* out_im,
                    std::int32_t a_re, std::int32_t a_im,
                    std::int16_t b_re, std::int16_t b_im) {
  const std::int32_t rr = MulDiv2Q31Q15(a_re, b_re);
  const std::int32_t ii = MulDiv2Q31Q15(a_im, b_im);
  const std::int32_t ri = MulDiv2Q31Q15(a_re, b_im);
  const std::int32_t ir = MulDiv2Q31Q15(a_im, b_re);
  *out_re = Sub32(rr, ii);
  *out_im = Add32(ri, ir);
}

void ComplexMul(std::int32_t* out_re, std::int32_t* out_im,
                std::int32_t a_re, std::int32_t a_im,
                std::int16_t b_re, std::int16_t b_im) {
  const std::int32_t rr = MulQ31Q15(a_re, b_re);
  const std::int32_t ii = MulQ31Q15(a_im, b_im);
  const std::int32_t ri = MulQ31Q15(a_re, b_im);
  const std::int32_t ir = MulQ31Q15(a_im, b_re);
  *out_re = Sub32(rr, ii);
  *out_im = Add32(ri, ir);
}

bool IsValidBlockType(BlockType type) {
  switch (type) {
    case BlockType::kLong:
    case BlockType::kStart:
    case BlockType::kShort:
    case BlockType::kStop:
      return true;
  }
  return false;
}

bool IsValidWindowShape(WindowShape shape) {
  return shape == WindowShape::kSine || shape == WindowShape::kKbd;
}

void SetError(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
}

const FIXP_WTP* WindowSlope(WindowShape shape, int length) {
  if (shape == WindowShape::kSine) {
    return length == kLongTransformLength ? SineWindow1024 : SineWindow128;
  }
  return length == kLongTransformLength ? KBDWindow1024 : KBDWindow128;
}

int BitReverse(int value, int bits) {
  int result = 0;
  for (int bit = 0; bit < bits; ++bit) {
    result = (result << 1) | (value & 1);
    value >>= 1;
  }
  return result;
}

TwiddleMagnitude DecodeFftTwiddle(unsigned phase) {
  // phase denotes 2*pi*phase/512 and is always in [0,255].  The generic FDK
  // radix-2 kernel indexes every fourth literal SineTable512 entry.
  unsigned base = 0;
  bool swap = false;
  bool cos_negative = false;
  if (phase <= 64) {
    base = phase;
  } else if (phase <= 128) {
    base = 128 - phase;
    swap = true;
  } else if (phase <= 192) {
    base = phase - 128;
    swap = true;
    cos_negative = true;
  } else {
    base = 256 - phase;
    cos_negative = true;
  }

  const FIXP_STP& entry = SineTable512[base * 4];
  if (swap) {
    return {ImQ15(entry), ReQ15(entry), cos_negative};
  }
  return {ReQ15(entry), ImQ15(entry), cos_negative};
}

std::int32_t ApplySignAfterTruncation(std::int32_t value, bool negative) {
  return negative ? Neg32(value) : value;
}

ComplexQ31 MultiplyByForwardTwiddle(ComplexQ31 b, unsigned phase) {
  if (phase == 0) {
    // Exact W=1 bypass.  Using Q15 0x7fff here would lose precision.
    return {Asr32(b.re, 1), Asr32(b.im, 1)};
  }
  if (phase == 128) {
    // Exact W=-j bypass; shift first, then negate, matching FDK.
    return {Asr32(b.im, 1), Neg32(Asr32(b.re, 1))};
  }

  const TwiddleMagnitude w = DecodeFftTwiddle(phase);
  const std::int32_t br_cos = ApplySignAfterTruncation(
      MulDiv2Q31Q15(b.re, w.cos_mag), w.cos_negative);
  const std::int32_t bi_cos = ApplySignAfterTruncation(
      MulDiv2Q31Q15(b.im, w.cos_mag), w.cos_negative);
  const std::int32_t br_sin = MulDiv2Q31Q15(b.re, w.sin_mag);
  const std::int32_t bi_sin = MulDiv2Q31Q15(b.im, w.sin_mag);
  return {Add32(br_cos, bi_sin), Sub32(bi_cos, br_sin)};
}

// Pure radix-2 DIT kernel for M=64/512.  The scale mask is stage 1 = shift,
// stage 2 = no shift, stages 3..ldn = shift: 101111 / 101111111.
int FftFixed(std::vector<ComplexQ31>* samples) {
  const int n = static_cast<int>(samples->size());
  const int ldn = n == 512 ? 9 : 6;
  std::vector<ComplexQ31> source(n);
  std::vector<ComplexQ31> destination(n);
  for (int i = 0; i < n; ++i) {
    source[BitReverse(i, ldn)] = (*samples)[i];
  }

  for (int stage = 1; stage <= ldn; ++stage) {
    const int m = 1 << stage;
    const int half = m >> 1;
    const int phase_step = 1 << (9 - stage);
    for (int group_base = 0; group_base < n; group_base += m) {
      int phase = 0;
      for (int j = 0; j < half; ++j, phase += phase_step) {
        const int address_a = group_base + j;
        const int address_b = address_a + half;
        const ComplexQ31 a = source[address_a];
        const ComplexQ31 b = source[address_b];
        ComplexQ31 top{};
        ComplexQ31 bottom{};

        if (stage == 1) {
          top.re = Wrap32(ArithmeticShiftRight(
              static_cast<std::int64_t>(a.re) + b.re, 1));
          top.im = Wrap32(ArithmeticShiftRight(
              static_cast<std::int64_t>(a.im) + b.im, 1));
          bottom.re = Sub32(top.re, b.re);
          bottom.im = Sub32(top.im, b.im);
        } else {
          ComplexQ31 u{};
          ComplexQ31 t{};
          if (stage == 2) {
            // This is the single unscaled stage in the frozen profile.
            u = a;
            t = phase == 0 ? b : ComplexQ31{b.im, Neg32(b.re)};
          } else {
            u = {Asr32(a.re, 1), Asr32(a.im, 1)};
            t = MultiplyByForwardTwiddle(b, static_cast<unsigned>(phase));
          }
          top = {Add32(u.re, t.re), Add32(u.im, t.im)};
          bottom = {Sub32(u.re, t.re), Sub32(u.im, t.im)};
        }
        destination[address_a] = top;
        destination[address_b] = bottom;
      }
    }
    source.swap(destination);
  }

  samples->swap(source);
  return ldn - 1;
}

void FoldAndWindow(const std::int16_t* time_data, int transform_length,
                   const FIXP_WTP* left_window, int left_length,
                   const FIXP_WTP* right_window, int right_length,
                   std::vector<std::int32_t>* data) {
  const int half = transform_length >> 1;
  const int left_offset = (transform_length - left_length) >> 1;
  const int right_offset = (transform_length - right_length) >> 1;
  data->assign(transform_length, 0);

  // Left flat/zero part: 0(A)-Br, including the explicit /2 scale.
  for (int i = 0; i < left_offset; ++i) {
    const std::int32_t b =
        static_cast<std::int32_t>(time_data[transform_length - i - 1]) *
        INT32_C(32768);
    (*data)[half + i] = Neg32(b);
  }

  // Windowed A-Br.  Each 16x16 product is kept separately, like FDK's
  // fMultDiv2 followed by fMultSubDiv2.
  for (int i = 0; i < left_length / 2; ++i) {
    const std::int32_t a = MulDiv2PcmQ15(
        time_data[i + left_offset], ImQ15(left_window[i]));
    const std::int32_t b = MulDiv2PcmQ15(
        time_data[transform_length - left_offset - i - 1],
        ReQ15(left_window[i]));
    (*data)[half + left_offset + i] = Sub32(a, b);
  }

  // Right flat/zero part: -Cr-0(D), reversed at placement.
  for (int i = 0; i < right_offset; ++i) {
    const std::int32_t c =
        static_cast<std::int32_t>(time_data[transform_length + i]) *
        INT32_C(32768);
    (*data)[half - 1 - i] = Neg32(c);
  }

  // Windowed -(C+Dr), reversed at placement.
  for (int i = 0; i < right_length / 2; ++i) {
    const std::int32_t c = MulDiv2PcmQ15(
        time_data[transform_length + right_offset + i],
        ReQ15(right_window[i]));
    const std::int32_t d = MulDiv2PcmQ15(
        time_data[2 * transform_length - right_offset - i - 1],
        ImQ15(right_window[i]));
    (*data)[half - right_offset - i - 1] = Neg32(Add32(c, d));
  }
}

void PreRotate(std::vector<std::int32_t>* data) {
  const int length = static_cast<int>(data->size());
  const int m = length >> 1;
  const FIXP_WTP* twiddle =
      length == kLongTransformLength ? SineWindow1024 : SineWindow128;

  for (int i = 0; i < m - 1; i += 2) {
    const int p0 = i;
    const int p1 = length - 2 - i;
    std::int32_t accu1 = (*data)[p1 + 1];
    std::int32_t accu2 = (*data)[p0];
    std::int32_t accu3 = (*data)[p0 + 1];
    std::int32_t accu4 = (*data)[p1];

    ComplexMulDiv2(&accu1, &accu2, accu1, accu2, ReQ15(twiddle[i]),
                   ImQ15(twiddle[i]));
    ComplexMulDiv2(&accu3, &accu4, accu4, accu3,
                   ReQ15(twiddle[i + 1]), ImQ15(twiddle[i + 1]));

    (*data)[p0] = Asr32(accu2, 1);
    (*data)[p0 + 1] = Asr32(accu1, 1);
    (*data)[p1] = Asr32(accu4, 1);
    (*data)[p1 + 1] = Neg32(Asr32(accu3, 1));
  }
}

std::vector<ComplexQ31> ToComplex(const std::vector<std::int32_t>& data) {
  std::vector<ComplexQ31> result(data.size() / 2);
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i] = {data[2 * i], data[2 * i + 1]};
  }
  return result;
}

void FromComplex(const std::vector<ComplexQ31>& complex,
                 std::vector<std::int32_t>* data) {
  data->resize(2 * complex.size());
  for (std::size_t i = 0; i < complex.size(); ++i) {
    (*data)[2 * i] = complex[i].re;
    (*data)[2 * i + 1] = complex[i].im;
  }
}

void PostRotate(std::vector<std::int32_t>* data) {
  const int length = static_cast<int>(data->size());
  const int m = length >> 1;
  // dct_getTables(): SineTable1024 step is 2 for L=1024 and 16 for L=128.
  const int sine_step = length == kLongTransformLength ? 2 : 16;
  int p0 = 0;
  int p1 = length - 2;
  std::int32_t accu1 = (*data)[p1];
  std::int32_t accu2 = (*data)[p1 + 1];
  (*data)[p1 + 1] = Neg32((*data)[p0 + 1]);

  int table_index = sine_step;
  for (int i = 1; i < ((m + 1) >> 1); ++i, table_index += sine_step) {
    const FIXP_STP& twiddle = SineTable1024[table_index];
    std::int32_t accu3 = 0;
    std::int32_t accu4 = 0;
    ComplexMul(&accu3, &accu4, accu1, accu2, ReQ15(twiddle),
               ImQ15(twiddle));
    (*data)[p0 + 1] = accu3;
    (*data)[p1] = accu4;

    p0 += 2;
    p1 -= 2;

    ComplexMul(&accu3, &accu4, (*data)[p0 + 1], (*data)[p0],
               ReQ15(twiddle), ImQ15(twiddle));
    accu1 = (*data)[p1];
    accu2 = (*data)[p1 + 1];
    (*data)[p1 + 1] = Neg32(accu3);
    (*data)[p0] = accu4;
  }

  // M is even for both supported lengths.  FDK handles pi/4 separately.
  accu1 = MulQ31Q15(accu1, kSqrtHalfQ15);
  accu2 = MulQ31Q15(accu2, kSqrtHalfQ15);
  (*data)[p1] = Add32(accu1, accu2);
  (*data)[p0 + 1] = Sub32(accu1, accu2);
}

void HashByte(std::uint64_t* hash, std::uint8_t byte) {
  *hash ^= byte;
  *hash *= kFnvPrime;
}

void HashQ15(std::uint64_t* hash, std::int16_t value) {
  const std::uint16_t raw = static_cast<std::uint16_t>(value);
  HashByte(hash, static_cast<std::uint8_t>(raw & UINT16_C(0x00ff)));
  HashByte(hash, static_cast<std::uint8_t>(raw >> 8));
}

std::uint64_t HashWindow(const FIXP_WTP* table, int pair_count) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (int i = 0; i < pair_count; ++i) {
    HashQ15(&hash, ReQ15(table[i]));
    HashQ15(&hash, ImQ15(table[i]));
  }
  return hash;
}

std::uint64_t HashSineTable1024() {
  std::uint64_t hash = kFnvOffsetBasis;
  for (int i = 0; i <= 512; ++i) {
    HashQ15(&hash, ReQ15(SineTable1024[i]));
    HashQ15(&hash, ImQ15(SineTable1024[i]));
  }
  return hash;
}

std::uint64_t HashFftOctant() {
  std::uint64_t hash = kFnvOffsetBasis;
  for (int i = 0; i <= 64; ++i) {
    HashQ15(&hash, ReQ15(SineTable512[i * 4]));
    HashQ15(&hash, ImQ15(SineTable512[i * 4]));
  }
  return hash;
}

}  // namespace

const char* BlockTypeName(BlockType type) {
  switch (type) {
    case BlockType::kLong:
      return "LONG";
    case BlockType::kStart:
      return "START";
    case BlockType::kShort:
      return "SHORT";
    case BlockType::kStop:
      return "STOP";
  }
  return "INVALID";
}

const char* WindowShapeName(WindowShape shape) {
  switch (shape) {
    case WindowShape::kSine:
      return "SINE";
    case WindowShape::kKbd:
      return "KBD";
  }
  return "INVALID";
}

RomFingerprints GetRomFingerprints() {
  RomFingerprints result;
  result.sine_window_1024 = HashWindow(SineWindow1024, 512);
  result.kbd_window_1024 = HashWindow(KBDWindow1024, 512);
  result.sine_window_128 = HashWindow(SineWindow128, 64);
  result.kbd_window_128 = HashWindow(KBDWindow128, 64);
  result.sine_table_1024 = HashSineTable1024();
  result.fft_octant_512 = HashFftOctant();
  return result;
}

bool ProcessAacLc1024(const std::int16_t* time_data,
                      std::size_t time_sample_count,
                      BlockType block_type,
                      WindowShape right_shape,
                      WindowState* state,
                      FrameTrace* trace,
                      std::string* error) {
  if (error != nullptr) error->clear();
  if (time_data == nullptr || state == nullptr || trace == nullptr) {
    SetError(error, "time_data, state and trace must be non-null");
    return false;
  }
  if (time_sample_count != static_cast<std::size_t>(kTimeSampleCount)) {
    SetError(error, "AAC-LC 1024 mode requires exactly 2048 PCM samples");
    return false;
  }
  if (!IsValidBlockType(block_type) || !IsValidWindowShape(right_shape)) {
    SetError(error, "invalid block type or window shape");
    return false;
  }

  const bool is_short = block_type == BlockType::kShort;
  const int transform_count = is_short ? kShortTransformCount : 1;
  const int transform_length =
      is_short ? kShortTransformLength : kLongTransformLength;
  const int right_length =
      (block_type == BlockType::kStart || is_short) ? kShortTransformLength
                                                    : kLongTransformLength;

  WindowState working_state = *state;
  const WindowState state_before = working_state;
  if (working_state.initialized) {
    if (!IsValidWindowShape(working_state.previous_right_shape) ||
        (working_state.previous_right_slope_length != kShortTransformLength &&
         working_state.previous_right_slope_length != kLongTransformLength) ||
        (working_state.previous_transform_length != kShortTransformLength &&
         working_state.previous_transform_length != kLongTransformLength)) {
      SetError(error, "invalid saved left-window state");
      return false;
    }
  } else {
    // mdct_block() startup rule: prev_wrs/prev_fr/prev_tl := current right.
    working_state.initialized = true;
    working_state.previous_right_shape = right_shape;
    working_state.previous_right_slope_length = right_length;
    working_state.previous_transform_length = transform_length;
  }

  FrameTrace result;
  result.block_type = block_type;
  result.requested_right_shape = right_shape;
  result.state_before = state_before;
  result.final_spectrum.assign(kFrameLength, 0);
  result.transforms.reserve(transform_count);

  const int first_input_offset =
      (kFrameLength - transform_length) >> 1;  // 0 long, 448 short.
  for (int sub = 0; sub < transform_count; ++sub) {
    const int left_length = working_state.previous_right_slope_length;
    if (left_length > transform_length || ((transform_length - left_length) & 1)) {
      SetError(error,
               "illegal block sequence: saved left slope does not fit transform");
      return false;
    }

    SubtransformTrace subtrace;
    subtrace.subtransform = sub;
    subtrace.input_offset = first_input_offset + sub * transform_length;
    subtrace.transform_length = transform_length;
    subtrace.left_shape = working_state.previous_right_shape;
    subtrace.right_shape = right_shape;
    subtrace.left_slope_length = left_length;
    subtrace.right_slope_length = right_length;

    const FIXP_WTP* left_window =
        WindowSlope(subtrace.left_shape, left_length);
    const FIXP_WTP* right_window = WindowSlope(right_shape, right_length);
    const std::int16_t* transform_input = time_data + subtrace.input_offset;
    FoldAndWindow(transform_input, transform_length, left_window, left_length,
                  right_window, right_length, &subtrace.fold);

    std::vector<std::int32_t> data = subtrace.fold;
    PreRotate(&data);
    subtrace.pre_fft = ToComplex(data);
    subtrace.fft = subtrace.pre_fft;
    subtrace.fft_scale_increment = FftFixed(&subtrace.fft);
    subtrace.fft_exponent =
        subtrace.pre_rotation_exponent + subtrace.fft_scale_increment;
    FromComplex(subtrace.fft, &data);
    PostRotate(&data);
    subtrace.post_rotation_exponent = subtrace.fft_exponent;
    subtrace.final_exponent = subtrace.post_rotation_exponent;
    subtrace.post = data;

    std::copy(data.begin(), data.end(),
              result.final_spectrum.begin() + sub * transform_length);
    result.transforms.push_back(std::move(subtrace));

    working_state.previous_right_shape = right_shape;
    working_state.previous_right_slope_length = right_length;
    working_state.previous_transform_length = transform_length;
  }

  result.state_after = working_state;
  *state = working_state;
  *trace = std::move(result);
  return true;
}

}  // namespace fixed_mdct_radix2
