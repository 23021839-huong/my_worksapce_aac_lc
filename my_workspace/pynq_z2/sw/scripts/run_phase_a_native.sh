#!/usr/bin/env bash

set -euo pipefail

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../../../.." && pwd)

input_wav=${1:-"$repo_root/input/abc_votay.wav"}
artifact_root=${2:-"$repo_root/my_workspace/pynq_z2/artifacts/phase_a"}
build_dir=${BUILD_DIR:-"$repo_root/build-pynq-a9"}
build_jobs=${BUILD_JOBS:-2}
bitrate=${BITRATE:-64000}
allow_non_arm=${ALLOW_NON_ARM:-0}

for command_name in cmake make gcc g++ python3 sha256sum; do
  command -v "$command_name" >/dev/null 2>&1 || die "Missing command: $command_name"
done

machine=$(uname -m)
case "$machine" in
  armv7l|armv7*) ;;
  *)
    if [ "$allow_non_arm" != "1" ]; then
      die "Expected ARMv7/Cortex-A9, got '$machine'. Run this baseline on PYNQ-Z2, or set ALLOW_NON_ARM=1 only for a host dry-run."
    fi
    ;;
esac

[ -f "$input_wav" ] || die "Input WAV not found: $input_wav"

python3 - "$input_wav" <<'PY'
import sys
import wave

path = sys.argv[1]
with wave.open(path, "rb") as wav:
    actual = (wav.getnchannels(), wav.getframerate(), wav.getsampwidth() * 8, wav.getcomptype())
    expected = (1, 48000, 16, "NONE")
    print(
        "Input WAV: "
        f"channels={actual[0]} sample_rate={actual[1]} "
        f"bits={actual[2]} compression={actual[3]}"
    )
    if actual != expected:
        raise SystemExit(
            "Input must be mono, 48000 Hz, signed PCM16, uncompressed WAV; "
            f"got {actual}"
        )
PY

printf 'Configuring Release build in %s\n' "$build_dir"
mkdir -p "$build_dir"
(
  cd "$build_dir"
  cmake "$repo_root" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_PROGRAMS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

printf 'Building with %s job(s)\n' "$build_jobs"
cmake --build "$build_dir" -- -j"$build_jobs"

encoder="$build_dir/aac-enc"
self_test="$build_dir/test-encode-decode"
[ -x "$encoder" ] || die "Encoder was not built: $encoder"
[ -x "$self_test" ] || die "Self-test was not built: $self_test"

run_id=$(date -u +%Y%m%dT%H%M%SZ)
run_dir="$artifact_root/$run_id"
mkdir -p "$run_dir"

source_revision=unknown
if command -v git >/dev/null 2>&1 && git -C "$repo_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  source_revision=$(git -C "$repo_root" rev-parse HEAD)
fi

{
  printf 'phase=A\n'
  printf 'backend=software\n'
  printf 'aot=AAC-LC (2)\n'
  printf 'channels=1\n'
  printf 'pcm=signed PCM16 little-endian WAV\n'
  printf 'sample_rate_hz=48000\n'
  printf 'frame_length=1024\n'
  printf 'transport=ADTS\n'
  printf 'bitrate_bps=%s\n' "$bitrate"
  printf 'afterburner=1\n'
  printf 'machine=%s\n' "$machine"
  printf 'source_revision=%s\n' "$source_revision"
  printf 'utc_run_id=%s\n' "$run_id"
  printf 'command=%q -t 2 -r %q -a 1 %q software_baseline.aac\n' \
    "$encoder" "$bitrate" "$input_wav"
  printf '\n[uname]\n'
  uname -a
  printf '\n[os-release]\n'
  if [ -r /etc/os-release ]; then
    cat /etc/os-release
  fi
  printf '\n[compiler]\n'
  gcc --version
  g++ --version
  gcc -dumpmachine
  printf '\n[cmake]\n'
  cmake --version
  printf '\n[binary]\n'
  file "$encoder" 2>/dev/null || true
  printf '\n[git-status]\n'
  if [ "$source_revision" != "unknown" ]; then
    git -C "$repo_root" status --short || true
  fi
} >"$run_dir/manifest.txt"

printf 'Running library encode/decode regression\n'
set +e
"$self_test" "$input_wav" 2>&1 | tee "$run_dir/test-encode-decode.log"
self_test_status=${PIPESTATUS[0]}
set -e
[ "$self_test_status" -eq 0 ] || die "test-encode-decode failed with status $self_test_status"

aac_output="$run_dir/software_baseline.aac"
printf 'Encoding AAC-LC ADTS at %s bit/s\n' "$bitrate"
if [ -x /usr/bin/time ]; then
  /usr/bin/time -v -o "$run_dir/encode.time.txt" \
    "$encoder" -t 2 -r "$bitrate" -a 1 "$input_wav" "$aac_output"
else
  seconds_before=$SECONDS
  "$encoder" -t 2 -r "$bitrate" -a 1 "$input_wav" "$aac_output"
  printf 'elapsed_seconds=%s\n' "$((SECONDS - seconds_before))" >"$run_dir/encode.time.txt"
fi
[ -s "$aac_output" ] || die "Encoder produced an empty file"

python3 - "$aac_output" >"$run_dir/adts_header.txt" <<'PY'
import sys

path = sys.argv[1]
with open(path, "rb") as stream:
    header = stream.read(7)
if len(header) != 7 or header[0] != 0xFF or (header[1] & 0xF0) != 0xF0:
    raise SystemExit(f"Invalid ADTS sync/header: {header.hex()}")
print(f"first_adts_header={header.hex()}")
print("adts_syncword=PASS")
PY

if command -v ffprobe >/dev/null 2>&1; then
  ffprobe -v error \
    -show_entries stream=codec_name,profile,sample_rate,channels \
    -of default=noprint_wrappers=1 "$aac_output" \
    >"$run_dir/ffprobe.txt"
fi

if command -v ffmpeg >/dev/null 2>&1; then
  decoded_wav="$run_dir/software_baseline_decoded.wav"
  ffmpeg -v error -y -i "$aac_output" -c:a pcm_s16le "$decoded_wav"
fi

sha256sum "$input_wav" >"$run_dir/input.sha256"
(
  cd "$run_dir"
  files=(software_baseline.aac manifest.txt test-encode-decode.log encode.time.txt adts_header.txt)
  [ ! -f ffprobe.txt ] || files+=(ffprobe.txt)
  [ ! -f software_baseline_decoded.wav ] || files+=(software_baseline_decoded.wav)
  sha256sum "${files[@]}" >SHA256SUMS
)

printf '\nPASS: Phase A software baseline completed.\n'
printf 'Artifacts: %s\n' "$run_dir"
printf 'AAC output: %s\n' "$aac_output"
if [ ! -f "$run_dir/ffprobe.txt" ]; then
  printf 'Note: install ffmpeg to add independent ADTS probing and decoded WAV output.\n'
fi
