#!/usr/bin/env bash

set -euo pipefail

die() {
  printf 'ERROR: %s\n' "$*" >&2
  exit 1
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../../../.." && pwd)

input_wav=${1:-"$repo_root/input/abc_votay.wav"}
artifact_root=${2:-"$repo_root/my_workspace/pynq_z2/artifacts/encoder_profile"}
build_dir=${BUILD_DIR:-"$repo_root/build-pynq-profile"}
build_jobs=${BUILD_JOBS:-2}
bitrate=${BITRATE:-64000}

for command_name in cmake gcc g++ python3 sha256sum taskset; do
  command -v "$command_name" >/dev/null 2>&1 ||
    die "Missing command: $command_name"
done

machine=$(uname -m)
case "$machine" in
  armv7l | armv7*) ;;
  *) die "Expected ARMv7/PYNQ-Z2, got '$machine'" ;;
esac

[ -f "$input_wav" ] || die "Input WAV not found: $input_wav"

mkdir -p "$build_dir"
(
  cd "$build_dir"
  cmake "$repo_root" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_PROGRAMS=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DFDK_ENABLE_ENCODER_PROFILING=ON
  cmake --build . -- -j"$build_jobs"
)

encoder="$build_dir/aac-enc"
[ -x "$encoder" ] || die "Encoder was not built: $encoder"

run_id=$(date -u +%Y%m%dT%H%M%SZ)
run_dir="$artifact_root/$run_id"
mkdir -p "$run_dir"

profile_csv="$run_dir/encoder_profile.csv"
aac_output="$run_dir/profile_output.aac"
time_output="$run_dir/process_resources.txt"

printf 'Profiling AAC-LC mono at %s bit/s on CPU 1\n' "$bitrate"
if [ -x /usr/bin/time ]; then
  FDK_PROFILE_CSV="$profile_csv" /usr/bin/time -v -o "$time_output" \
    taskset -c 1 "$encoder" \
    -t 2 -r "$bitrate" -a 1 "$input_wav" "$aac_output"
else
  FDK_PROFILE_CSV="$profile_csv" taskset -c 1 "$encoder" \
    -t 2 -r "$bitrate" -a 1 "$input_wav" "$aac_output"
  printf 'GNU time was not available.\n' >"$time_output"
fi

[ -s "$aac_output" ] || die "Encoder produced an empty AAC file"
[ -s "$profile_csv" ] || die "Encoder did not produce profiling CSV"

{
  printf 'machine=%s\n' "$machine"
  printf 'kernel=%s\n' "$(uname -r)"
  printf 'bitrate_bps=%s\n' "$bitrate"
  printf 'afterburner=1\n'
  printf 'input=%s\n' "$input_wav"
  printf 'encoder=%s\n' "$encoder"
  printf 'cpu_affinity=1\n'
  if [ -r /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor ]; then
    printf 'cpu1_governor=%s\n' \
      "$(cat /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor)"
  fi
} >"$run_dir/manifest.txt"

sha256sum "$input_wav" "$aac_output" "$profile_csv" \
  >"$run_dir/SHA256SUMS"

python3 - "$profile_csv" <<'PY'
import csv
import sys

with open(sys.argv[1], newline="", encoding="utf-8") as stream:
    rows = list(csv.DictReader(stream))

print()
print(f"{'scope':<8} {'block':<29} {'% frame':>9} {'avg us':>11} {'calls/fr':>10}")
print("-" * 73)
for row in rows:
    print(
        f"{row['scope']:<8} {row['block']:<29} "
        f"{float(row['percent_of_frame']):>8.2f}% "
        f"{float(row['average_us']):>11.3f} "
        f"{float(row['calls_per_frame']):>10.3f}"
    )
PY

printf '\nPASS: encoder profiling completed.\n'
printf 'CSV: %s\n' "$profile_csv"
printf 'Process resources: %s\n' "$time_output"
printf 'AAC: %s\n' "$aac_output"
