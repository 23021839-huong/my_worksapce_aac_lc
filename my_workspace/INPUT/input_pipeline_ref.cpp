/*
 * input_pipeline_ref.cpp
 *
 * AAC-LC mono input stimulus generator.
 *
 * Reads a PCM16 mono WAV file and reproduces the input fan-out performed by
 * FDKaacEnc_psyMain():
 *   1. 1024 new samples are exposed to Block Switching.
 *   2. psyInputBuffer[2048] is updated in the same order as FDK-AAC and is
 *      exposed to Window/MDCT before the buffer rotation.
 *
 * This program does not run Block Switching or MDCT. It only prepares their
 * time-domain inputs and the absolute-sample mapping used for verification.
 */

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace {

constexpr int kFrameLength = 1024;
constexpr int kTransformFactor = 8;
constexpr int kMdctBufferSize = 2 * kFrameLength;
constexpr int kBlockSwitchingOffset =
    kFrameLength + (9 * kFrameLength / (2 * kTransformFactor));
constexpr int kPreMdctCopySamples =
    kMdctBufferSize - kBlockSwitchingOffset;
constexpr int kPostMdctCopySamples =
    kBlockSwitchingOffset - kFrameLength;

static_assert(kBlockSwitchingOffset == 1600,
              "AAC-LC block-switching offset must be 1600");
static_assert(kPreMdctCopySamples == 448,
              "AAC-LC pre-MDCT copy must contain 448 samples");
static_assert(kPostMdctCopySamples == 576,
              "AAC-LC post-MDCT copy must contain 576 samples");

#ifdef _WIN32
constexpr char kPathSeparator = '\\';
int makeOneDirectory(const char *path) { return _mkdir(path); }
#else
constexpr char kPathSeparator = '/';
int makeOneDirectory(const char *path) { return mkdir(path, 0755); }
#endif

struct WavInfo {
  int audioFormat = 0;
  int channels = 0;
  int sampleRate = 0;
  int bitsPerSample = 0;
  std::streamoff dataOffset = 0;
  uint32_t dataBytes = 0;
};

bool readU16Le(std::istream &input, uint16_t *value) {
  unsigned char bytes[2];
  if (!input.read(reinterpret_cast<char *>(bytes), sizeof(bytes))) return false;
  *value = static_cast<uint16_t>(bytes[0]) |
           (static_cast<uint16_t>(bytes[1]) << 8);
  return true;
}

bool readU32Le(std::istream &input, uint32_t *value) {
  unsigned char bytes[4];
  if (!input.read(reinterpret_cast<char *>(bytes), sizeof(bytes))) return false;
  *value = static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
  return true;
}

bool readFourCc(std::istream &input, char id[5]) {
  if (!input.read(id, 4)) return false;
  id[4] = '\0';
  return true;
}

bool readPcm16MonoWav(const std::string &path, WavInfo *info,
                      std::vector<int16_t> *samples, std::string *error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    *error = "cannot open WAV file";
    return false;
  }

  char riff[5], wave[5];
  uint32_t riffSize = 0;
  if (!readFourCc(input, riff) || !readU32Le(input, &riffSize) ||
      !readFourCc(input, wave) || std::strcmp(riff, "RIFF") != 0 ||
      std::strcmp(wave, "WAVE") != 0) {
    *error = "input is not a RIFF/WAVE file";
    return false;
  }
  (void)riffSize;

  bool haveFmt = false;
  bool haveData = false;
  while (input && !(haveFmt && haveData)) {
    char chunkId[5];
    uint32_t chunkSize = 0;
    if (!readFourCc(input, chunkId) || !readU32Le(input, &chunkSize)) break;
    const std::streamoff payload = input.tellg();

    if (std::strcmp(chunkId, "fmt ") == 0) {
      uint16_t format = 0, channels = 0, blockAlign = 0, bits = 0;
      uint32_t sampleRate = 0, byteRate = 0;
      if (chunkSize < 16 || !readU16Le(input, &format) ||
          !readU16Le(input, &channels) || !readU32Le(input, &sampleRate) ||
          !readU32Le(input, &byteRate) || !readU16Le(input, &blockAlign) ||
          !readU16Le(input, &bits)) {
        *error = "invalid WAV fmt chunk";
        return false;
      }
      (void)byteRate;
      (void)blockAlign;
      info->audioFormat = static_cast<int>(format);
      info->channels = static_cast<int>(channels);
      info->sampleRate = static_cast<int>(sampleRate);
      info->bitsPerSample = static_cast<int>(bits);
      haveFmt = true;
    } else if (std::strcmp(chunkId, "data") == 0) {
      info->dataOffset = payload;
      info->dataBytes = chunkSize;
      haveData = true;
    }

    input.clear();
    input.seekg(payload + static_cast<std::streamoff>(chunkSize +
                                                      (chunkSize & 1U)));
  }

  if (!haveFmt || !haveData) {
    *error = "WAV file does not contain both fmt and data chunks";
    return false;
  }
  if (info->audioFormat != 1) {
    *error = "WAV must use uncompressed PCM format (format code 1)";
    return false;
  }
  if (info->channels != 1 || info->bitsPerSample != 16) {
    *error = "WAV must be mono and 16-bit";
    return false;
  }
  if ((info->dataBytes & 1U) != 0) {
    *error = "WAV data chunk contains an incomplete 16-bit sample";
    return false;
  }

  input.clear();
  input.seekg(info->dataOffset);
  std::vector<unsigned char> bytes(info->dataBytes);
  if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size())) {
    *error = "cannot read the complete WAV data chunk";
    return false;
  }

  samples->resize(bytes.size() / 2);
  for (std::size_t i = 0; i < samples->size(); ++i) {
    const uint16_t raw = static_cast<uint16_t>(bytes[2 * i]) |
                         (static_cast<uint16_t>(bytes[2 * i + 1]) << 8);
    const int32_t signedValue =
        raw < 0x8000U ? static_cast<int32_t>(raw)
                      : static_cast<int32_t>(raw) - 0x10000;
    (*samples)[i] = static_cast<int16_t>(signedValue);
  }
  return true;
}

bool makeDirectories(const std::string &path) {
  if (path.empty() || path.size() >= 512) return false;

  char temp[512];
  std::memcpy(temp, path.c_str(), path.size() + 1);
  for (char *cursor = temp + 1; *cursor; ++cursor) {
    if (*cursor != '/' && *cursor != '\\') continue;
    const char saved = *cursor;
    *cursor = '\0';
    if (!(std::strlen(temp) == 2 && temp[1] == ':') &&
        makeOneDirectory(temp) != 0 && errno != EEXIST) {
      return false;
    }
    *cursor = saved;
  }
  return makeOneDirectory(temp) == 0 || errno == EEXIST;
}

std::string joinPath(const std::string &directory, const std::string &name) {
  if (directory.empty()) return name;
  const char last = directory.back();
  if (last == '/' || last == '\\') return directory + name;
  return directory + kPathSeparator + name;
}

void writeS16Le(std::ostream &output, int16_t sample) {
  const uint16_t raw = static_cast<uint16_t>(sample);
  const char bytes[2] = {static_cast<char>(raw & 0xffU),
                         static_cast<char>((raw >> 8) & 0xffU)};
  output.write(bytes, sizeof(bytes));
}

void printUsage(const char *program) {
  std::cout << "Usage:\n"
            << "  " << program
            << " [--wav <pcm16_mono.wav>] [--out <directory>]"
               " [--frames N]\n\n"
            << "Defaults:\n"
            << "  --wav ../../input/abc_votay.wav\n"
            << "  --out out/abc_votay\n";
}

}  // namespace

int main(int argc, char **argv) {
  std::string wavPath = "../../input/abc_votay.wav";
  std::string outputDirectory = "out/abc_votay";
  int frameLimit = -1;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--wav" && i + 1 < argc) {
      wavPath = argv[++i];
    } else if (argument == "--out" && i + 1 < argc) {
      outputDirectory = argv[++i];
    } else if (argument == "--frames" && i + 1 < argc) {
      try {
        frameLimit = std::stoi(argv[++i]);
      } catch (...) {
        std::cerr << "ERROR: --frames requires a positive integer\n";
        return 2;
      }
      if (frameLimit <= 0) {
        std::cerr << "ERROR: --frames requires a positive integer\n";
        return 2;
      }
    } else if (argument == "--help" || argument == "-h") {
      printUsage(argv[0]);
      return 0;
    } else {
      std::cerr << "ERROR: unknown or incomplete argument: " << argument
                << '\n';
      printUsage(argv[0]);
      return 2;
    }
  }

  WavInfo wavInfo;
  std::vector<int16_t> pcm;
  std::string readError;
  if (!readPcm16MonoWav(wavPath, &wavInfo, &pcm, &readError)) {
    std::cerr << "ERROR: " << readError << ": " << wavPath << '\n';
    return 1;
  }

  const std::size_t framesAvailable = pcm.size() / kFrameLength;
  const std::size_t tailSamples = pcm.size() % kFrameLength;
  std::size_t framesToDump = framesAvailable;
  if (frameLimit > 0) {
    framesToDump = std::min(framesToDump,
                            static_cast<std::size_t>(frameLimit));
  }
  if (framesToDump == 0) {
    std::cerr << "ERROR: input does not contain one complete 1024-sample frame\n";
    return 1;
  }

  if (!makeDirectories(outputDirectory)) {
    std::cerr << "ERROR: cannot create output directory: " << outputDirectory
              << '\n';
    return 1;
  }

  const std::string metaPath = joinPath(outputDirectory, "meta.txt");
  const std::string mapPath = joinPath(outputDirectory, "frame_map.csv");
  const std::string blockTextPath =
      joinPath(outputDirectory, "block_switch_input.txt");
  const std::string blockRawPath =
      joinPath(outputDirectory, "block_switch_input_s16le.raw");
  const std::string mdctTextPath =
      joinPath(outputDirectory, "mdct_input_buffer.txt");
  const std::string mdctRawPath =
      joinPath(outputDirectory, "mdct_input_buffer_s16le.raw");

  std::ofstream meta(metaPath);
  std::ofstream frameMap(mapPath);
  std::ofstream blockText(blockTextPath);
  std::ofstream blockRaw(blockRawPath, std::ios::binary);
  std::ofstream mdctText(mdctTextPath);
  std::ofstream mdctRaw(mdctRawPath, std::ios::binary);
  if (!meta || !frameMap || !blockText || !blockRaw || !mdctText ||
      !mdctRaw) {
    std::cerr << "ERROR: cannot open all output files in: "
              << outputDirectory << '\n';
    return 1;
  }

  meta << "source=" << wavPath << '\n'
       << "format=PCM16 mono little-endian\n"
       << "sampleRate=" << wavInfo.sampleRate << '\n'
       << "channels=" << wavInfo.channels << '\n'
       << "bitsPerSample=" << wavInfo.bitsPerSample << '\n'
       << "wavDataBytes=" << wavInfo.dataBytes << '\n'
       << "totalSamples=" << pcm.size() << '\n'
       << "frameLength=" << kFrameLength << '\n'
       << "framesAvailable=" << framesAvailable << '\n'
       << "framesDumped=" << framesToDump << '\n'
       << "unprocessedFullFrames=" << (framesAvailable - framesToDump) << '\n'
       << "tailSamples=" << tailSamples << '\n'
       << "mdctBufferSize=" << kMdctBufferSize << '\n'
       << "transformFactor=" << kTransformFactor << '\n'
       << "blockSwitchingOffset=" << kBlockSwitchingOffset << '\n'
       << "preMdctCopySamples=" << kPreMdctCopySamples << '\n'
       << "postMdctCopySamples=" << kPostMdctCopySamples << '\n'
       << "mdctVsBlockSwitchLagSamples=" << kPostMdctCopySamples << '\n'
       << "startupPadding=zeros\n";

  frameMap
      << "frame,block_switch_abs_start,block_switch_abs_end,mdct_abs_start,"
         "mdct_abs_end,mdct_zero_prefix_samples,mdct_valid_samples\n";
  blockText << "# format: frame sample_in_frame abs_sample pcm_s16\n";
  mdctText
      << "# format: frame buffer_index abs_sample is_padding pcm_s16\n";

  std::vector<int16_t> psyInputBuffer(kMdctBufferSize, 0);

  for (std::size_t frame = 0; frame < framesToDump; ++frame) {
    const std::size_t blockStart = frame * kFrameLength;
    const int64_t mdctStart =
        static_cast<int64_t>(blockStart) - kBlockSwitchingOffset;
    const int64_t mdctEnd = mdctStart + kMdctBufferSize - 1;
    const int zeroPrefix =
        mdctStart < 0
            ? static_cast<int>(std::min<int64_t>(-mdctStart, kMdctBufferSize))
            : 0;

    blockText << "# frame " << frame << '\n';
    for (int sampleIndex = 0; sampleIndex < kFrameLength; ++sampleIndex) {
      const std::size_t absolute = blockStart + sampleIndex;
      const int16_t sample = pcm[absolute];
      blockText << frame << ' ' << sampleIndex << ' ' << absolute << ' '
                << static_cast<int>(sample) << '\n';
      writeS16Le(blockRaw, sample);
    }

    std::copy_n(pcm.begin() + static_cast<std::ptrdiff_t>(blockStart),
                kPreMdctCopySamples,
                psyInputBuffer.begin() + kBlockSwitchingOffset);

    mdctText << "# frame " << frame << '\n';
    for (int bufferIndex = 0; bufferIndex < kMdctBufferSize; ++bufferIndex) {
      const int64_t absolute = mdctStart + bufferIndex;
      const bool isPadding = absolute < 0;
      const int16_t expected =
          isPadding ? 0 : pcm[static_cast<std::size_t>(absolute)];
      const int16_t actual = psyInputBuffer[bufferIndex];
      if (actual != expected) {
        std::cerr << "ERROR: internal psyInputBuffer mapping mismatch at frame "
                  << frame << ", index " << bufferIndex << '\n';
        return 1;
      }
      mdctText << frame << ' ' << bufferIndex << ' ' << absolute << ' '
               << (isPadding ? 1 : 0) << ' ' << static_cast<int>(actual)
               << '\n';
      writeS16Le(mdctRaw, actual);
    }

    frameMap << frame << ',' << blockStart << ','
             << (blockStart + kFrameLength - 1) << ',' << mdctStart << ','
             << mdctEnd << ',' << zeroPrefix << ','
             << (kMdctBufferSize - zeroPrefix) << '\n';

    std::memmove(psyInputBuffer.data(),
                 psyInputBuffer.data() + kFrameLength,
                 kFrameLength * sizeof(int16_t));
    std::copy_n(
        pcm.begin() + static_cast<std::ptrdiff_t>(blockStart +
                                                  kPreMdctCopySamples),
        kPostMdctCopySamples,
        psyInputBuffer.begin() + kFrameLength);
  }

  meta.flush();
  frameMap.flush();
  blockText.flush();
  blockRaw.flush();
  mdctText.flush();
  mdctRaw.flush();

  if (!meta || !frameMap || !blockText || !blockRaw || !mdctText ||
      !mdctRaw) {
    std::cerr << "ERROR: failed while writing output files\n";
    return 1;
  }

  std::cout << "OK: generated " << framesToDump
            << " AAC-LC input frame(s)\n"
            << "  " << metaPath << '\n'
            << "  " << mapPath << '\n'
            << "  " << blockTextPath << '\n'
            << "  " << blockRawPath << '\n'
            << "  " << mdctTextPath << '\n'
            << "  " << mdctRawPath << '\n';
  return 0;
}
