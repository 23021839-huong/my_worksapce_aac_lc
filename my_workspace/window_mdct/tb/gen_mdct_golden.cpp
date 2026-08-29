/*
 * Deterministic boundary-vector generator for
 * FDK_AACLC_1024_MDCT_Q31Q15_RAD2_V1.
 *
 * Build from the repository root (example):
 *   c++ -std=c++17 -O2 \
 *     -IlibFDK/include -IlibSYS/include \
 *     my_workspace/window_mdct/ref/fixed_mdct_radix2.cpp \
 *     my_workspace/window_mdct/tb/gen_mdct_golden.cpp \
 *     libFDK/src/FDK_tools_rom.cpp -o gen_mdct_golden
 *
 * Run:
 *   gen_mdct_golden --out-dir <directory> [--seed 0x4D444354]
 *
 * The linked FDK translation unit supplies literal Q15 ROM only.  All MDCT,
 * DCT-IV and FFT arithmetic is executed by fixed_mdct_radix2.cpp.
 */

#include "../ref/fixed_mdct_radix2.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
namespace mdct = fixed_mdct_radix2;

namespace {

constexpr std::uint64_t kFnvOffsetBasis = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);

struct GoldenCase {
  const char* name;
  mdct::BlockType block_type;
  mdct::WindowShape right_shape;
  int pattern;
};

struct FrameKey {
  int case_id = 0;
  int frame_id = 0;

  bool operator<(const FrameKey& other) const {
    return std::tie(case_id, frame_id) <
           std::tie(other.case_id, other.frame_id);
  }
};

struct SubKey {
  int case_id = 0;
  int frame_id = 0;
  int sub_id = 0;

  bool operator<(const SubKey& other) const {
    return std::tie(case_id, frame_id, sub_id) <
           std::tie(other.case_id, other.frame_id, other.sub_id);
  }
};

struct ExpectedFrame {
  bool reset_before = false;
  mdct::BlockType block_type = mdct::BlockType::kLong;
  mdct::WindowShape right_shape = mdct::WindowShape::kSine;
  int transform_count = 0;
  std::vector<std::int16_t> pcm;
  std::vector<std::int32_t> final_spectrum;
  int final_exponent = 0;
  int pcm_rows = 0;
  int final_rows = 0;
};

struct ExpectedSubtransform {
  int transform_length = 0;
  int left_slope_length = 0;
  int right_slope_length = 0;
  int left_offset = 0;
  int right_offset = 0;
  int input_offset = 0;
  int output_offset = 0;
  int fft_length = 0;
  mdct::WindowShape left_shape = mdct::WindowShape::kSine;
  int fold_exponent = 0;
  int pre_exponent = 0;
  int fft_scale_increment = 0;
  int fft_exponent = 0;
  int post_exponent = 0;
  std::string fft_scale_mask;
  std::vector<std::int32_t> fold;
  std::vector<mdct::ComplexQ31> pre;
  std::vector<mdct::ComplexQ31> fft;
  std::vector<std::int32_t> post;
  int fold_rows = 0;
  int pre_rows = 0;
  int fft_rows = 0;
  int post_rows = 0;
};

const GoldenCase kCases[] = {
    {"long_kbd_zero", mdct::BlockType::kLong, mdct::WindowShape::kKbd, 0},
    {"long_kbd_impulse_edges", mdct::BlockType::kLong,
     mdct::WindowShape::kKbd, 1},
    {"start_sine_ramp_noise", mdct::BlockType::kStart,
     mdct::WindowShape::kSine, 2},
    {"eight_short_sine_edges", mdct::BlockType::kShort,
     mdct::WindowShape::kSine, 3},
    {"stop_kbd_alternating", mdct::BlockType::kStop,
     mdct::WindowShape::kKbd, 4},
    {"long_kbd_seeded_noise", mdct::BlockType::kLong,
     mdct::WindowShape::kKbd, 5},
};

std::uint32_t XorShift32(std::uint32_t* state) {
  std::uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

std::int16_t RawQ15(std::uint32_t raw) {
  const std::uint16_t bits = static_cast<std::uint16_t>(raw);
  if ((bits & UINT16_C(0x8000)) == 0) {
    return static_cast<std::int16_t>(bits);
  }
  return static_cast<std::int16_t>(static_cast<std::int32_t>(bits) - 65536);
}

std::vector<std::int16_t> MakeInput(int pattern, std::uint32_t seed) {
  std::vector<std::int16_t> pcm(mdct::kTimeSampleCount, 0);
  std::uint32_t prng = seed ^
                       (UINT32_C(0x9E3779B9) *
                        static_cast<std::uint32_t>(pattern + 1));
  if (prng == 0) prng = UINT32_C(0x6D2B79F5);

  switch (pattern) {
    case 0:
      break;

    case 1: {
      // Exercise the four long-window quarters and both ends of the snapshot.
      const int index[] = {0, 1, 511, 512, 1023, 1024, 1535, 1536, 2046, 2047};
      const std::int16_t value[] = {
          INT16_MIN, INT16_MAX, -1, 1, INT16_MAX,
          INT16_MIN, 0x4000, -0x4000, 0x1234, -0x1234};
      for (std::size_t i = 0; i < sizeof(index) / sizeof(index[0]); ++i) {
        pcm[index[i]] = value[i];
      }
      break;
    }

    case 2:
      for (int i = 0; i < mdct::kTimeSampleCount; ++i) {
        const std::int32_t ramp = ((i * 977 + 131) & 0xffff) - 32768;
        const std::int32_t noise =
            static_cast<std::int32_t>(XorShift32(&prng) & 0x1fff) - 4096;
        pcm[i] = RawQ15(static_cast<std::uint32_t>(ramp + noise));
      }
      break;

    case 3:
      for (int i = 0; i < mdct::kTimeSampleCount; ++i) {
        pcm[i] = RawQ15(XorShift32(&prng) >> 16);
      }
      // Every short transform starts at 448+128*q and consumes 256 samples.
      for (int q = 0; q < mdct::kShortTransformCount; ++q) {
        const int start = 448 + q * mdct::kShortTransformLength;
        pcm[start] = (q & 1) ? INT16_MIN : INT16_MAX;
        pcm[start + 127] = (q & 1) ? INT16_MAX : INT16_MIN;
        pcm[start + 255] = static_cast<std::int16_t>(q * 4093 - 14000);
      }
      break;

    case 4:
      for (int i = 0; i < mdct::kTimeSampleCount; ++i) {
        const std::int32_t dither =
            static_cast<std::int32_t>(XorShift32(&prng) & 0x07ff) - 1024;
        const std::int32_t carrier = (i & 1) ? 28000 : -28000;
        pcm[i] = static_cast<std::int16_t>(carrier + dither);
      }
      break;

    default:
      for (int i = 0; i < mdct::kTimeSampleCount; ++i) {
        pcm[i] = RawQ15(XorShift32(&prng) >> 16);
      }
      pcm[0] = INT16_MIN;
      pcm[mdct::kTimeSampleCount - 1] = INT16_MAX;
      break;
  }
  return pcm;
}

std::string Hex16(std::int16_t value) {
  std::ostringstream text;
  text << std::uppercase << std::hex << std::setfill('0') << std::setw(4)
       << static_cast<std::uint16_t>(value);
  return text.str();
}

std::string Hex32(std::int32_t value) {
  std::ostringstream text;
  text << std::uppercase << std::hex << std::setfill('0') << std::setw(8)
       << static_cast<std::uint32_t>(value);
  return text.str();
}

std::string Hex64(std::uint64_t value) {
  std::ostringstream text;
  text << std::uppercase << std::hex << std::setfill('0') << std::setw(16)
       << value;
  return text.str();
}

void HashByte(std::uint64_t* hash, std::uint8_t byte) {
  *hash ^= byte;
  *hash *= kFnvPrime;
}

void Hash16(std::uint64_t* hash, std::int16_t value) {
  const std::uint16_t raw = static_cast<std::uint16_t>(value);
  HashByte(hash, static_cast<std::uint8_t>(raw));
  HashByte(hash, static_cast<std::uint8_t>(raw >> 8));
}

void Hash32(std::uint64_t* hash, std::int32_t value) {
  const std::uint32_t raw = static_cast<std::uint32_t>(value);
  for (unsigned shift = 0; shift < 32; shift += 8) {
    HashByte(hash, static_cast<std::uint8_t>(raw >> shift));
  }
}

std::uint64_t HashPcm(const std::vector<std::int16_t>& values) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (std::int16_t value : values) Hash16(&hash, value);
  return hash;
}

std::uint64_t HashFold(const mdct::FrameTrace& frame) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const mdct::SubtransformTrace& transform : frame.transforms) {
    for (std::int32_t value : transform.fold) Hash32(&hash, value);
  }
  return hash;
}

std::uint64_t HashComplexBoundary(const mdct::FrameTrace& frame, bool fft) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const mdct::SubtransformTrace& transform : frame.transforms) {
    const std::vector<mdct::ComplexQ31>& values =
        fft ? transform.fft : transform.pre_fft;
    for (const mdct::ComplexQ31& value : values) {
      Hash32(&hash, value.re);
      Hash32(&hash, value.im);
    }
  }
  return hash;
}

std::uint64_t HashPost(const mdct::FrameTrace& frame) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const mdct::SubtransformTrace& transform : frame.transforms) {
    for (std::int32_t value : transform.post) Hash32(&hash, value);
  }
  return hash;
}

std::uint64_t HashFinal(const mdct::FrameTrace& frame) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (std::int32_t value : frame.final_spectrum) Hash32(&hash, value);
  return hash;
}

std::string StateShape(const mdct::WindowState& state) {
  return state.initialized ? mdct::WindowShapeName(state.previous_right_shape)
                           : "UNINITIALIZED";
}

bool ParseSeed(const char* text, std::uint32_t* seed) {
  if (text == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long raw = std::strtoull(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0' || raw > UINT32_MAX) {
    return false;
  }
  *seed = static_cast<std::uint32_t>(raw);
  return true;
}

std::vector<std::string> SplitWhitespace(const std::string& line) {
  std::istringstream input(line);
  std::vector<std::string> fields;
  std::string field;
  while (input >> field) fields.push_back(field);
  return fields;
}

std::vector<std::string> SplitCsv(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  for (char ch : line) {
    if (ch == ',') {
      fields.push_back(field);
      field.clear();
    } else {
      field.push_back(ch);
    }
  }
  fields.push_back(field);
  return fields;
}

bool ParseDecimal(const std::string& text, int* value) {
  if (text.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' ||
      parsed < INT32_MIN || parsed > INT32_MAX) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

bool ParseRawHex(const std::string& text, std::size_t width,
                 std::uint32_t* value) {
  if (text.size() != width) return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
  if (errno != 0 || end == text.c_str() || *end != '\0' ||
      parsed > UINT32_MAX) {
    return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseHex16Signed(const std::string& text, std::int16_t* value) {
  std::uint32_t raw = 0;
  if (!ParseRawHex(text, 4, &raw)) return false;
  const std::int32_t signed_value =
      (raw & UINT32_C(0x8000)) ? static_cast<std::int32_t>(raw) - 65536
                               : static_cast<std::int32_t>(raw);
  *value = static_cast<std::int16_t>(signed_value);
  return true;
}

bool ParseHex32Signed(const std::string& text, std::int32_t* value) {
  std::uint32_t raw = 0;
  if (!ParseRawHex(text, 8, &raw)) return false;
  if ((raw & UINT32_C(0x80000000)) == 0) {
    *value = static_cast<std::int32_t>(raw);
  } else {
    *value = static_cast<std::int32_t>(
        static_cast<std::int64_t>(raw) - INT64_C(0x100000000));
  }
  return true;
}

bool LoadPythonGolden(
    const fs::path& directory,
    std::map<FrameKey, ExpectedFrame>* frames,
    std::map<SubKey, ExpectedSubtransform>* transforms,
    std::string* error) {
  std::ifstream manifest(directory / "manifest.csv");
  if (!manifest) {
    *error = "cannot open Python manifest.csv";
    return false;
  }
  std::string line;
  if (!std::getline(manifest, line)) {
    *error = "Python manifest.csv is empty";
    return false;
  }
  int line_number = 1;
  while (std::getline(manifest, line)) {
    ++line_number;
    const std::vector<std::string> f = SplitCsv(line);
    if (f.size() != 26 || f[0] != "mdct-r2-fdkq15-v1") {
      *error = "manifest.csv schema/field count mismatch at line " +
               std::to_string(line_number);
      return false;
    }
    int number[19] = {};
    const int field_index[] = {1, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                               12, 13, 14, 15, 17, 18, 19, 21, 22};
    for (std::size_t i = 0; i < sizeof(field_index) / sizeof(field_index[0]);
         ++i) {
      if (!ParseDecimal(f[field_index[i]], &number[i])) {
        *error = "manifest.csv invalid integer at line " +
                 std::to_string(line_number);
        return false;
      }
    }
    int fft_scale = 0;
    int fft_out_exp = 0;
    int post_exp = 0;
    if (!ParseDecimal(f[23], &fft_scale) ||
        !ParseDecimal(f[24], &fft_out_exp) ||
        !ParseDecimal(f[25], &post_exp)) {
      *error = "manifest.csv invalid exponent at line " +
               std::to_string(line_number);
      return false;
    }

    const int case_id = number[0];
    const int frame_id = number[1];
    const int reset_before = number[2];
    const int sub_id = number[3];
    const int block = number[4];
    const int right_shape = number[5];
    const int left_shape = number[6];
    const int n_spec = number[7];
    const int tl = number[8];
    const int fl = number[9];
    const int fr = number[10];
    const int nl = number[11];
    const int nr = number[12];
    const int pcm_base = number[13];
    const int out_base = number[14];
    const int nfft = number[15];
    const int fft_sign = number[16];
    const int fold_exp = number[17];
    const int pre_exp = number[18];
    if (case_id < 0 || frame_id < 0 || sub_id < 0 || block < 0 || block > 3 ||
        right_shape < 0 || right_shape > 1 || left_shape < 0 ||
        left_shape > 1 || (tl != 128 && tl != 1024) || nfft != tl / 2 ||
        fft_sign != -1 || n_spec < 1 || sub_id >= n_spec) {
      *error = "manifest.csv unsupported contract at line " +
               std::to_string(line_number);
      return false;
    }

    const FrameKey frame_key{case_id, frame_id};
    ExpectedFrame& frame = (*frames)[frame_key];
    if (frame.transform_count == 0) {
      frame.reset_before = reset_before != 0;
      frame.block_type = static_cast<mdct::BlockType>(block);
      frame.right_shape = static_cast<mdct::WindowShape>(right_shape);
      frame.transform_count = n_spec;
      frame.pcm.assign(mdct::kTimeSampleCount, 0);
      frame.final_spectrum.assign(mdct::kFrameLength, 0);
    } else if (frame.reset_before != (reset_before != 0) ||
               frame.block_type != static_cast<mdct::BlockType>(block) ||
               frame.right_shape != static_cast<mdct::WindowShape>(right_shape) ||
               frame.transform_count != n_spec) {
      *error = "manifest.csv inconsistent frame rows at line " +
               std::to_string(line_number);
      return false;
    }

    const SubKey sub_key{case_id, frame_id, sub_id};
    if (transforms->count(sub_key) != 0) {
      *error = "manifest.csv duplicate sub-transform at line " +
               std::to_string(line_number);
      return false;
    }
    ExpectedSubtransform expected;
    expected.transform_length = tl;
    expected.left_slope_length = fl;
    expected.right_slope_length = fr;
    expected.left_offset = nl;
    expected.right_offset = nr;
    expected.input_offset = pcm_base;
    expected.output_offset = out_base;
    expected.fft_length = nfft;
    expected.left_shape = static_cast<mdct::WindowShape>(left_shape);
    expected.fold_exponent = fold_exp;
    expected.pre_exponent = pre_exp;
    expected.fft_scale_increment = fft_scale;
    expected.fft_exponent = fft_out_exp;
    expected.post_exponent = post_exp;
    expected.fft_scale_mask = f[20];
    expected.fold.assign(tl, 0);
    expected.pre.assign(nfft, {});
    expected.fft.assign(nfft, {});
    expected.post.assign(tl, 0);
    transforms->emplace(sub_key, std::move(expected));
  }
  if (!manifest.eof() || frames->empty() || transforms->empty()) {
    *error = "manifest.csv read error or no records";
    return false;
  }

  std::ifstream pcm_file(directory / "pcm_in_s16.txt");
  if (!pcm_file) {
    *error = "cannot open Python pcm_in_s16.txt";
    return false;
  }
  line_number = 0;
  while (std::getline(pcm_file, line)) {
    ++line_number;
    const std::vector<std::string> f = SplitWhitespace(line);
    int case_id = 0, frame_id = 0, index = 0;
    std::int16_t value = 0;
    if (f.size() != 4 || !ParseDecimal(f[0], &case_id) ||
        !ParseDecimal(f[1], &frame_id) || !ParseDecimal(f[2], &index) ||
        !ParseHex16Signed(f[3], &value)) {
      *error = "pcm_in_s16.txt malformed at line " +
               std::to_string(line_number);
      return false;
    }
    auto it = frames->find(FrameKey{case_id, frame_id});
    if (it == frames->end() || index < 0 || index >= mdct::kTimeSampleCount) {
      *error = "pcm_in_s16.txt key/index mismatch at line " +
               std::to_string(line_number);
      return false;
    }
    it->second.pcm[index] = value;
    ++it->second.pcm_rows;
  }

  auto load_real_boundary =
      [&](const char* filename, bool is_fold) -> bool {
    std::ifstream input(directory / filename);
    if (!input) {
      *error = std::string("cannot open Python ") + filename;
      return false;
    }
    int row = 0;
    while (std::getline(input, line)) {
      ++row;
      const std::vector<std::string> f = SplitWhitespace(line);
      int case_id = 0, frame_id = 0, sub_id = 0, index = 0, exponent = 0;
      std::int32_t value = 0;
      if (f.size() != 6 || !ParseDecimal(f[0], &case_id) ||
          !ParseDecimal(f[1], &frame_id) || !ParseDecimal(f[2], &sub_id) ||
          !ParseDecimal(f[3], &index) || !ParseHex32Signed(f[4], &value) ||
          !ParseDecimal(f[5], &exponent)) {
        *error = std::string(filename) + " malformed at line " +
                 std::to_string(row);
        return false;
      }
      auto it = transforms->find(SubKey{case_id, frame_id, sub_id});
      if (it == transforms->end() || index < 0 ||
          index >= it->second.transform_length ||
          exponent != (is_fold ? it->second.fold_exponent
                               : it->second.post_exponent)) {
        *error = std::string(filename) + " key/index/exp mismatch at line " +
                 std::to_string(row);
        return false;
      }
      if (is_fold) {
        it->second.fold[index] = value;
        ++it->second.fold_rows;
      } else {
        it->second.post[index] = value;
        ++it->second.post_rows;
      }
    }
    return input.eof();
  };
  if (!load_real_boundary("fold_q31.txt", true) ||
      !load_real_boundary("post_q31.txt", false)) {
    if (error->empty()) *error = "real boundary file read error";
    return false;
  }

  std::ifstream pre_file(directory / "pre_q31.txt");
  if (!pre_file) {
    *error = "cannot open Python pre_q31.txt";
    return false;
  }
  line_number = 0;
  while (std::getline(pre_file, line)) {
    ++line_number;
    const std::vector<std::string> f = SplitWhitespace(line);
    int case_id = 0, frame_id = 0, sub_id = 0, index = 0, exponent = 0;
    mdct::ComplexQ31 value{};
    if (f.size() != 7 || !ParseDecimal(f[0], &case_id) ||
        !ParseDecimal(f[1], &frame_id) || !ParseDecimal(f[2], &sub_id) ||
        !ParseDecimal(f[3], &index) || !ParseHex32Signed(f[4], &value.re) ||
        !ParseHex32Signed(f[5], &value.im) || !ParseDecimal(f[6], &exponent)) {
      *error = "pre_q31.txt malformed at line " + std::to_string(line_number);
      return false;
    }
    auto it = transforms->find(SubKey{case_id, frame_id, sub_id});
    if (it == transforms->end() || index < 0 || index >= it->second.fft_length ||
        exponent != it->second.pre_exponent) {
      *error = "pre_q31.txt key/index/exp mismatch at line " +
               std::to_string(line_number);
      return false;
    }
    it->second.pre[index] = value;
    ++it->second.pre_rows;
  }

  std::ifstream fft_file(directory / "fft_stage_q31.txt");
  if (!fft_file) {
    *error = "cannot open Python fft_stage_q31.txt";
    return false;
  }
  line_number = 0;
  while (std::getline(fft_file, line)) {
    ++line_number;
    const std::vector<std::string> f = SplitWhitespace(line);
    int case_id = 0, frame_id = 0, sub_id = 0, stage = 0, index = 0;
    int fft_exp = 0, cumulative_exp = 0;
    mdct::ComplexQ31 value{};
    if (f.size() != 9 || !ParseDecimal(f[0], &case_id) ||
        !ParseDecimal(f[1], &frame_id) || !ParseDecimal(f[2], &sub_id) ||
        !ParseDecimal(f[3], &stage) || !ParseDecimal(f[4], &index) ||
        !ParseHex32Signed(f[5], &value.re) ||
        !ParseHex32Signed(f[6], &value.im) ||
        !ParseDecimal(f[7], &fft_exp) ||
        !ParseDecimal(f[8], &cumulative_exp)) {
      *error = "fft_stage_q31.txt malformed at line " +
               std::to_string(line_number);
      return false;
    }
    auto it = transforms->find(SubKey{case_id, frame_id, sub_id});
    if (it == transforms->end()) {
      *error = "fft_stage_q31.txt unknown key at line " +
               std::to_string(line_number);
      return false;
    }
    const int final_stage = it->second.fft_length == 512 ? 9 : 6;
    if (stage == final_stage) {
      if (index < 0 || index >= it->second.fft_length ||
          fft_exp != it->second.fft_scale_increment ||
          cumulative_exp != it->second.fft_exponent) {
        *error = "fft_stage_q31.txt final-stage metadata mismatch at line " +
                 std::to_string(line_number);
        return false;
      }
      it->second.fft[index] = value;
      ++it->second.fft_rows;
    }
  }

  std::ifstream final_file(directory / "mdct_out_q31.txt");
  if (!final_file) {
    *error = "cannot open Python mdct_out_q31.txt";
    return false;
  }
  line_number = 0;
  while (std::getline(final_file, line)) {
    ++line_number;
    const std::vector<std::string> f = SplitWhitespace(line);
    int case_id = 0, frame_id = 0, index = 0, exponent = 0;
    std::int32_t value = 0;
    if (f.size() != 5 || !ParseDecimal(f[0], &case_id) ||
        !ParseDecimal(f[1], &frame_id) || !ParseDecimal(f[2], &index) ||
        !ParseHex32Signed(f[3], &value) || !ParseDecimal(f[4], &exponent)) {
      *error = "mdct_out_q31.txt malformed at line " +
               std::to_string(line_number);
      return false;
    }
    auto it = frames->find(FrameKey{case_id, frame_id});
    if (it == frames->end() || index < 0 || index >= mdct::kFrameLength) {
      *error = "mdct_out_q31.txt key/index mismatch at line " +
               std::to_string(line_number);
      return false;
    }
    if (it->second.final_rows != 0 && exponent != it->second.final_exponent) {
      *error = "mdct_out_q31.txt inconsistent exponent at line " +
               std::to_string(line_number);
      return false;
    }
    it->second.final_exponent = exponent;
    it->second.final_spectrum[index] = value;
    ++it->second.final_rows;
  }

  for (const auto& item : *frames) {
    const ExpectedFrame& frame = item.second;
    if (frame.pcm_rows != mdct::kTimeSampleCount ||
        frame.final_rows != mdct::kFrameLength) {
      *error = "Python golden has incomplete PCM/final frame";
      return false;
    }
  }
  for (const auto& item : *transforms) {
    const ExpectedSubtransform& sub = item.second;
    if (sub.fold_rows != sub.transform_length ||
        sub.pre_rows != sub.fft_length || sub.fft_rows != sub.fft_length ||
        sub.post_rows != sub.transform_length) {
      *error = "Python golden has incomplete boundary sub-transform";
      return false;
    }
  }
  return true;
}

int VerifyPythonGolden(const fs::path& directory) {
  std::map<FrameKey, ExpectedFrame> frames;
  std::map<SubKey, ExpectedSubtransform> expected_transforms;
  std::string error;
  if (!LoadPythonGolden(directory, &frames, &expected_transforms, &error)) {
    std::cerr << "Python golden load failed: " << error << '\n';
    return 1;
  }

  mdct::WindowState state;
  std::uint64_t scalar_comparisons = 0;
  std::size_t transform_comparisons = 0;
  for (const auto& frame_item : frames) {
    const FrameKey frame_key = frame_item.first;
    const ExpectedFrame& expected_frame = frame_item.second;
    if (expected_frame.reset_before) state = mdct::WindowState{};

    mdct::FrameTrace actual;
    if (!mdct::ProcessAacLc1024(
            expected_frame.pcm.data(), expected_frame.pcm.size(),
            expected_frame.block_type, expected_frame.right_shape, &state,
            &actual, &error)) {
      std::cerr << "C++ model failed at case=" << frame_key.case_id
                << " frame=" << frame_key.frame_id << ": " << error << '\n';
      return 1;
    }
    if (static_cast<int>(actual.transforms.size()) !=
            expected_frame.transform_count ||
        actual.block_type != expected_frame.block_type ||
        actual.requested_right_shape != expected_frame.right_shape) {
      std::cerr << "frame metadata mismatch at case=" << frame_key.case_id
                << " frame=" << frame_key.frame_id << '\n';
      return 1;
    }

    for (const mdct::SubtransformTrace& sub : actual.transforms) {
      const SubKey sub_key{frame_key.case_id, frame_key.frame_id,
                           sub.subtransform};
      const auto expected_it = expected_transforms.find(sub_key);
      if (expected_it == expected_transforms.end()) {
        std::cerr << "missing expected sub-transform at case="
                  << frame_key.case_id << " frame=" << frame_key.frame_id
                  << " sub=" << sub.subtransform << '\n';
        return 1;
      }
      const ExpectedSubtransform& expected = expected_it->second;
      const int expected_output_offset = sub.subtransform * sub.transform_length;
      const int expected_left_offset =
          (sub.transform_length - sub.left_slope_length) >> 1;
      const int expected_right_offset =
          (sub.transform_length - sub.right_slope_length) >> 1;
      const std::string actual_mask =
          sub.transform_length == mdct::kLongTransformLength ? "101111111"
                                                             : "101111";
      if (sub.transform_length != expected.transform_length ||
          sub.input_offset != expected.input_offset ||
          expected_output_offset != expected.output_offset ||
          sub.left_shape != expected.left_shape ||
          sub.right_shape != expected_frame.right_shape ||
          sub.left_slope_length != expected.left_slope_length ||
          sub.right_slope_length != expected.right_slope_length ||
          expected_left_offset != expected.left_offset ||
          expected_right_offset != expected.right_offset ||
          static_cast<int>(sub.pre_fft.size()) != expected.fft_length ||
          sub.fold_exponent != expected.fold_exponent ||
          sub.pre_rotation_exponent != expected.pre_exponent ||
          sub.fft_scale_increment != expected.fft_scale_increment ||
          sub.fft_exponent != expected.fft_exponent ||
          sub.post_rotation_exponent != expected.post_exponent ||
          actual_mask != expected.fft_scale_mask) {
        std::cerr << "sub-transform metadata mismatch at case="
                  << frame_key.case_id << " frame=" << frame_key.frame_id
                  << " sub=" << sub.subtransform << '\n';
        return 1;
      }

      auto compare_real = [&](const char* boundary,
                              const std::vector<std::int32_t>& expected_values,
                              const std::vector<std::int32_t>& actual_values)
          -> bool {
        if (expected_values.size() != actual_values.size()) {
          std::cerr << boundary << " size mismatch at case=" << frame_key.case_id
                    << " frame=" << frame_key.frame_id
                    << " sub=" << sub.subtransform << '\n';
          return false;
        }
        for (std::size_t i = 0; i < expected_values.size(); ++i) {
          ++scalar_comparisons;
          if (expected_values[i] != actual_values[i]) {
            std::cerr << boundary << " mismatch at case=" << frame_key.case_id
                      << " frame=" << frame_key.frame_id
                      << " sub=" << sub.subtransform << " index=" << i
                      << " expected=" << Hex32(expected_values[i])
                      << " actual=" << Hex32(actual_values[i]) << '\n';
            return false;
          }
        }
        return true;
      };
      auto compare_complex =
          [&](const char* boundary,
              const std::vector<mdct::ComplexQ31>& expected_values,
              const std::vector<mdct::ComplexQ31>& actual_values) -> bool {
        if (expected_values.size() != actual_values.size()) {
          std::cerr << boundary << " size mismatch at case=" << frame_key.case_id
                    << " frame=" << frame_key.frame_id
                    << " sub=" << sub.subtransform << '\n';
          return false;
        }
        for (std::size_t i = 0; i < expected_values.size(); ++i) {
          scalar_comparisons += 2;
          if (expected_values[i].re != actual_values[i].re ||
              expected_values[i].im != actual_values[i].im) {
            std::cerr << boundary << " mismatch at case=" << frame_key.case_id
                      << " frame=" << frame_key.frame_id
                      << " sub=" << sub.subtransform << " index=" << i
                      << " expected=" << Hex32(expected_values[i].re) << '/'
                      << Hex32(expected_values[i].im)
                      << " actual=" << Hex32(actual_values[i].re) << '/'
                      << Hex32(actual_values[i].im) << '\n';
            return false;
          }
        }
        return true;
      };

      if (!compare_real("fold", expected.fold, sub.fold) ||
          !compare_complex("pre", expected.pre, sub.pre_fft) ||
          !compare_complex("fft", expected.fft, sub.fft) ||
          !compare_real("post", expected.post, sub.post)) {
        return 1;
      }
      ++transform_comparisons;
    }

    if (actual.transforms.empty() ||
        actual.transforms.front().final_exponent !=
            expected_frame.final_exponent ||
        actual.final_spectrum.size() != expected_frame.final_spectrum.size()) {
      std::cerr << "final metadata mismatch at case=" << frame_key.case_id
                << " frame=" << frame_key.frame_id << '\n';
      return 1;
    }
    for (std::size_t i = 0; i < actual.final_spectrum.size(); ++i) {
      ++scalar_comparisons;
      if (actual.final_spectrum[i] != expected_frame.final_spectrum[i]) {
        std::cerr << "final mismatch at case=" << frame_key.case_id
                  << " frame=" << frame_key.frame_id << " index=" << i
                  << " expected=" << Hex32(expected_frame.final_spectrum[i])
                  << " actual=" << Hex32(actual.final_spectrum[i]) << '\n';
        return 1;
      }
    }
  }

  std::cout << "VERIFY PASS   : C++ == Python golden, 0 LSB\n"
            << "frames        : " << frames.size() << '\n'
            << "transforms    : " << transform_comparisons << '\n'
            << "scalar checks : " << scalar_comparisons << '\n'
            << "golden        : " << directory.string() << '\n';
  return 0;
}

void Usage(const char* program) {
  std::cerr << "usage: " << program
            << " --out-dir <directory> [--seed 0x4D444354]\n"
            << "       " << program << " --verify-python <golden-directory>\n";
}

bool Good(const std::ofstream& stream, const char* name) {
  if (stream) return true;
  std::cerr << "cannot create or write " << name << '\n';
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  const char* out_dir_arg = nullptr;
  const char* verify_python_arg = nullptr;
  std::uint32_t seed = UINT32_C(0x4D444354);  // ASCII "MDCT".
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--out-dir") && i + 1 < argc) {
      out_dir_arg = argv[++i];
    } else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) {
      if (!ParseSeed(argv[++i], &seed)) {
        std::cerr << "invalid --seed\n";
        return 2;
      }
    } else if (!std::strcmp(argv[i], "--verify-python") && i + 1 < argc) {
      verify_python_arg = argv[++i];
    } else if (!std::strcmp(argv[i], "--help") ||
               !std::strcmp(argv[i], "-h")) {
      Usage(argv[0]);
      return 0;
    } else {
      Usage(argv[0]);
      return 2;
    }
  }
  if (verify_python_arg != nullptr) {
    if (out_dir_arg != nullptr) {
      std::cerr << "--verify-python and --out-dir are mutually exclusive\n";
      return 2;
    }
    return VerifyPythonGolden(fs::path(verify_python_arg));
  }
  if (out_dir_arg == nullptr) {
    Usage(argv[0]);
    return 2;
  }

  const fs::path out_dir(out_dir_arg);
  std::error_code fs_error;
  fs::create_directories(out_dir, fs_error);
  if (fs_error) {
    std::cerr << "cannot create output directory: " << fs_error.message()
              << '\n';
    return 1;
  }

  std::ofstream input_file(out_dir / "mdct_input_pcm.csv");
  std::ofstream fold_file(out_dir / "mdct_fold.csv");
  std::ofstream pre_file(out_dir / "mdct_pre.csv");
  std::ofstream fft_file(out_dir / "mdct_fft.csv");
  std::ofstream post_file(out_dir / "mdct_post.csv");
  std::ofstream final_file(out_dir / "mdct_final.csv");
  std::ofstream manifest_file(out_dir / "mdct_manifest.csv");
  std::ofstream contract_file(out_dir / "mdct_contract.meta");
  if (!Good(input_file, "mdct_input_pcm.csv") ||
      !Good(fold_file, "mdct_fold.csv") ||
      !Good(pre_file, "mdct_pre.csv") ||
      !Good(fft_file, "mdct_fft.csv") ||
      !Good(post_file, "mdct_post.csv") ||
      !Good(final_file, "mdct_final.csv") ||
      !Good(manifest_file, "mdct_manifest.csv") ||
      !Good(contract_file, "mdct_contract.meta")) {
    return 1;
  }

  input_file << "profile,case,frame,index,pcm_dec,pcm_q15_hex\n";
  fold_file << "profile,case,frame,block,sub,L,input_offset,index,q31_hex,"
               "boundary_exp,left_shape,left_len,right_shape,right_len\n";
  pre_file << "profile,case,frame,block,sub,L,index,re_q31_hex,im_q31_hex,"
              "boundary_exp\n";
  fft_file << "profile,case,frame,block,sub,L,index,re_q31_hex,im_q31_hex,"
              "boundary_exp,fft_scale_increment,scale_mask\n";
  post_file << "profile,case,frame,block,sub,L,index,q31_hex,boundary_exp\n";
  final_file << "profile,case,frame,block,index,q31_hex,final_exp\n";
  manifest_file
      << "profile,fft_profile,fdk_commit,twiddle_octant_sha256,case,frame,"
         "block,right_shape,state_before_shape,state_before_slope,"
         "state_before_tl,state_after_shape,state_after_slope,state_after_tl,"
         "transform_count,L,final_exp,input_fnv1a64,fold_fnv1a64,"
         "pre_fnv1a64,fft_fnv1a64,post_fnv1a64,final_fnv1a64\n";

  const mdct::RomFingerprints rom = mdct::GetRomFingerprints();
  contract_file
      << "profile=" << mdct::kMdctProfileName << '\n'
      << "fft_profile=" << mdct::kFftProfileName << '\n'
      << "fdk_commit=" << mdct::kFdkCommit << '\n'
      << "data=signed_q1.31\ncoeff=signed_q1.15\ninput=signed_pcm16_q1.15\n"
      << "fft_direction=forward_negative_exponent\n"
      << "scale_mask_fft64=101111\nscale_mask_fft512=101111111\n"
      << "rounding=arithmetic_truncation\nsaturation=none\noverflow=wrap32\n"
      << "window_state=previous_right_becomes_next_left\n"
      << "short_input_offset=448\nshort_input_stride=128\n"
      << "fold_boundary_exp=2\npre_boundary_exp=4\n"
      << "fft_boundary_exp_long=12\nfft_boundary_exp_short=9\n"
      << "post_boundary_exp_long=12\npost_boundary_exp_short=9\n"
      << "twiddle_octant_sha256=" << mdct::kFftTwiddleOctantSha256 << '\n'
      << "sine_window_1024_fnv1a64=" << Hex64(rom.sine_window_1024) << '\n'
      << "kbd_window_1024_fnv1a64=" << Hex64(rom.kbd_window_1024) << '\n'
      << "sine_window_128_fnv1a64=" << Hex64(rom.sine_window_128) << '\n'
      << "kbd_window_128_fnv1a64=" << Hex64(rom.kbd_window_128) << '\n'
      << "sine_table_1024_fnv1a64=" << Hex64(rom.sine_table_1024) << '\n'
      << "fft_octant_512_fnv1a64=" << Hex64(rom.fft_octant_512) << '\n';

  mdct::WindowState state;
  const std::size_t case_count = sizeof(kCases) / sizeof(kCases[0]);
  std::size_t transform_count_total = 0;
  for (std::size_t frame_index = 0; frame_index < case_count; ++frame_index) {
    const GoldenCase& test = kCases[frame_index];
    const std::vector<std::int16_t> pcm =
        MakeInput(test.pattern, seed + static_cast<std::uint32_t>(frame_index));
    mdct::FrameTrace frame;
    std::string error;
    if (!mdct::ProcessAacLc1024(pcm.data(), pcm.size(), test.block_type,
                                test.right_shape, &state, &frame, &error)) {
      std::cerr << "frame " << frame_index << " (" << test.name
                << ") failed: " << error << '\n';
      return 1;
    }

    for (int i = 0; i < mdct::kTimeSampleCount; ++i) {
      input_file << mdct::kMdctProfileName << ',' << test.name << ','
                 << frame_index << ',' << i << ',' << pcm[i] << ','
                 << Hex16(pcm[i]) << '\n';
    }

    for (const mdct::SubtransformTrace& transform : frame.transforms) {
      const char* block = mdct::BlockTypeName(frame.block_type);
      for (std::size_t i = 0; i < transform.fold.size(); ++i) {
        fold_file << mdct::kMdctProfileName << ',' << test.name << ','
                  << frame_index << ',' << block << ','
                  << transform.subtransform << ',' << transform.transform_length
                  << ',' << transform.input_offset << ',' << i << ','
                  << Hex32(transform.fold[i]) << ',' << transform.fold_exponent
                  << ',' << mdct::WindowShapeName(transform.left_shape) << ','
                  << transform.left_slope_length << ','
                  << mdct::WindowShapeName(transform.right_shape) << ','
                  << transform.right_slope_length << '\n';
      }
      for (std::size_t i = 0; i < transform.pre_fft.size(); ++i) {
        pre_file << mdct::kMdctProfileName << ',' << test.name << ','
                 << frame_index << ',' << block << ','
                 << transform.subtransform << ',' << transform.transform_length
                 << ',' << i << ',' << Hex32(transform.pre_fft[i].re) << ','
                 << Hex32(transform.pre_fft[i].im) << ','
                 << transform.pre_rotation_exponent << '\n';
      }
      const char* scale_mask =
          transform.transform_length == mdct::kLongTransformLength
              ? "101111111"
              : "101111";
      for (std::size_t i = 0; i < transform.fft.size(); ++i) {
        fft_file << mdct::kMdctProfileName << ',' << test.name << ','
                 << frame_index << ',' << block << ','
                 << transform.subtransform << ',' << transform.transform_length
                 << ',' << i << ',' << Hex32(transform.fft[i].re) << ','
                 << Hex32(transform.fft[i].im) << ',' << transform.fft_exponent
                 << ',' << transform.fft_scale_increment << ',' << scale_mask
                 << '\n';
      }
      for (std::size_t i = 0; i < transform.post.size(); ++i) {
        post_file << mdct::kMdctProfileName << ',' << test.name << ','
                  << frame_index << ',' << block << ','
                  << transform.subtransform << ',' << transform.transform_length
                  << ',' << i << ',' << Hex32(transform.post[i]) << ','
                  << transform.post_rotation_exponent << '\n';
      }
    }

    const int final_exp = frame.transforms.front().final_exponent;
    for (std::size_t i = 0; i < frame.final_spectrum.size(); ++i) {
      final_file << mdct::kMdctProfileName << ',' << test.name << ','
                 << frame_index << ',' << mdct::BlockTypeName(frame.block_type)
                 << ',' << i << ',' << Hex32(frame.final_spectrum[i]) << ','
                 << final_exp << '\n';
    }

    const int transform_length = frame.transforms.front().transform_length;
    manifest_file
        << mdct::kMdctProfileName << ',' << mdct::kFftProfileName << ','
        << mdct::kFdkCommit << ',' << mdct::kFftTwiddleOctantSha256 << ','
        << test.name << ',' << frame_index << ','
        << mdct::BlockTypeName(frame.block_type) << ','
        << mdct::WindowShapeName(frame.requested_right_shape) << ','
        << StateShape(frame.state_before) << ','
        << frame.state_before.previous_right_slope_length << ','
        << frame.state_before.previous_transform_length << ','
        << StateShape(frame.state_after) << ','
        << frame.state_after.previous_right_slope_length << ','
        << frame.state_after.previous_transform_length << ','
        << frame.transforms.size() << ',' << transform_length << ',' << final_exp
        << ',' << Hex64(HashPcm(pcm)) << ',' << Hex64(HashFold(frame)) << ','
        << Hex64(HashComplexBoundary(frame, false)) << ','
        << Hex64(HashComplexBoundary(frame, true)) << ','
        << Hex64(HashPost(frame)) << ',' << Hex64(HashFinal(frame)) << '\n';
    transform_count_total += frame.transforms.size();
  }

  if (!input_file || !fold_file || !pre_file || !fft_file || !post_file ||
      !final_file || !manifest_file || !contract_file) {
    std::cerr << "I/O error while writing golden suite\n";
    return 1;
  }

  std::cout << "profile       : " << mdct::kMdctProfileName << '\n'
            << "frames        : " << case_count << '\n'
            << "transforms    : " << transform_count_total << '\n'
            << "output        : " << out_dir.string() << '\n'
            << "ROM KBD1024   : " << Hex64(rom.kbd_window_1024) << '\n'
            << "ROM SINE128   : " << Hex64(rom.sine_window_128) << '\n';
  return 0;
}
