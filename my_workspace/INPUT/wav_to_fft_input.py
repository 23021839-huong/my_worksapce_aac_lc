#!/usr/bin/env python3
"""Convert a PCM16 mono WAV file into complex Q1.23 FFT input vectors.

The generated text file is shared by:
  * fft_radix2_core/tb/tb_fft_radix2_core.vhd
  * fft_radix2_core/compare_fft_numpy.py

Each complex sample is built from two consecutive PCM samples:
  re_q23 = pcm[2*i]   << pcm_shift
  im_q23 = pcm[2*i+1] << pcm_shift

The default pcm_shift=7 keeps one bit of headroom.  This reduces saturation in
the fixed-point RTL and makes the comparison with a non-saturating NumPy FFT
more meaningful.  Use pcm_shift=8 only for a full-scale stress test.
"""

from __future__ import annotations

import argparse
import csv
import sys
import wave
from array import array
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


DATA_W = 24
FRAME_PCM = 1024
TB_MIXED_SIZES = (512, 64, 64, 64, 64, 64, 64, 64, 64, 512, 512)

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
DEFAULT_WAV = REPO_ROOT / "input" / "abc_votay.wav"
DEFAULT_OUTPUT = (
    REPO_ROOT
    / "my_workspace"
    / "window_mdct"
    / "fft_radix2_core"
    / "vectors"
    / "input_fft.txt"
)


class ConversionError(RuntimeError):
    """Raised when the WAV or requested vector layout is invalid."""


@dataclass(frozen=True)
class WavData:
    sample_rate: int
    samples: Sequence[int]


@dataclass(frozen=True)
class TransformSpec:
    frame: int
    nfft: int
    pcm_start: int
    audio_frame: int
    short_index: int

    @property
    def mode(self) -> int:
        return 1 if self.nfft == 512 else 0

    @property
    def pcm_count(self) -> int:
        return 2 * self.nfft


def read_pcm16_mono_wav(path: Path) -> WavData:
    try:
        with wave.open(str(path), "rb") as wav_file:
            channels = wav_file.getnchannels()
            sample_width = wav_file.getsampwidth()
            compression = wav_file.getcomptype()
            sample_rate = wav_file.getframerate()
            frame_count = wav_file.getnframes()
            raw = wav_file.readframes(frame_count)
    except (OSError, EOFError, wave.Error) as exc:
        raise ConversionError(f"cannot read WAV {path}: {exc}") from exc

    if compression != "NONE":
        raise ConversionError(
            f"WAV must be uncompressed PCM; compression={compression!r}"
        )
    if channels != 1:
        raise ConversionError(f"WAV must be mono; channels={channels}")
    if sample_width != 2:
        raise ConversionError(
            f"WAV must contain signed 16-bit samples; sample_width={sample_width}"
        )
    if len(raw) != frame_count * sample_width:
        raise ConversionError(
            f"incomplete WAV data: expected {frame_count * sample_width} bytes, "
            f"got {len(raw)}"
        )

    samples = array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    return WavData(sample_rate=sample_rate, samples=samples)


def make_tb_mixed_specs(sample_count: int) -> list[TransformSpec]:
    required_pcm = sum(2 * nfft for nfft in TB_MIXED_SIZES)
    if sample_count < required_pcm:
        raise ConversionError(
            f"layout tb-mixed needs {required_pcm} PCM samples, got {sample_count}"
        )

    specs: list[TransformSpec] = []
    pcm_start = 0
    short_index = 0
    for frame, nfft in enumerate(TB_MIXED_SIZES):
        audio_frame = pcm_start // FRAME_PCM
        current_short = short_index if nfft == 64 else -1
        specs.append(
            TransformSpec(frame, nfft, pcm_start, audio_frame, current_short)
        )
        pcm_start += 2 * nfft
        if nfft == 64:
            short_index += 1
        else:
            short_index = 0
    return specs


def make_fft512_specs(sample_count: int) -> list[TransformSpec]:
    transform_count = sample_count // (2 * 512)
    return [
        TransformSpec(frame, 512, frame * 1024, frame, -1)
        for frame in range(transform_count)
    ]


def make_fft64_specs(sample_count: int) -> list[TransformSpec]:
    transform_count = sample_count // (2 * 64)
    return [
        TransformSpec(
            frame=frame,
            nfft=64,
            pcm_start=frame * 128,
            audio_frame=(frame * 128) // FRAME_PCM,
            short_index=frame % 8,
        )
        for frame in range(transform_count)
    ]


def make_both_specs(sample_count: int) -> list[TransformSpec]:
    """Create one FFT-512 and eight FFT-64 transforms per 1024 PCM samples.

    The two sizes reuse the same source PCM block.  This layout is intended for
    a future EOF-driven testbench; it does not match the current hard-coded
    FRAME_N sequence in tb_fft_radix2_core.vhd.
    """

    audio_frame_count = sample_count // FRAME_PCM
    specs: list[TransformSpec] = []
    transform_frame = 0
    for audio_frame in range(audio_frame_count):
        block_start = audio_frame * FRAME_PCM
        specs.append(
            TransformSpec(transform_frame, 512, block_start, audio_frame, -1)
        )
        transform_frame += 1
        for short_index in range(8):
            specs.append(
                TransformSpec(
                    transform_frame,
                    64,
                    block_start + short_index * 128,
                    audio_frame,
                    short_index,
                )
            )
            transform_frame += 1
    return specs


def make_specs(layout: str, sample_count: int) -> list[TransformSpec]:
    if layout == "tb-mixed":
        return make_tb_mixed_specs(sample_count)
    if layout == "fft512":
        return make_fft512_specs(sample_count)
    if layout == "fft64":
        return make_fft64_specs(sample_count)
    if layout == "both":
        return make_both_specs(sample_count)
    raise AssertionError(f"unhandled layout: {layout}")


def encode_q1_23(pcm: int, pcm_shift: int) -> int:
    value = int(pcm) << pcm_shift
    minimum = -(1 << (DATA_W - 1))
    maximum = (1 << (DATA_W - 1)) - 1
    if value < minimum or value > maximum:
        raise ConversionError(
            f"PCM conversion produced {value}, outside signed {DATA_W}-bit range"
        )
    return value


def hex24(value: int) -> str:
    return f"{value & 0xFFFFFF:06X}"


def ensure_writable(paths: Iterable[Path], force: bool) -> None:
    existing = [path for path in paths if path.exists()]
    if existing and not force:
        names = ", ".join(str(path) for path in existing)
        raise ConversionError(f"output already exists: {names}; pass --force to replace")
    for path in paths:
        path.parent.mkdir(parents=True, exist_ok=True)


def write_vectors(
    output_path: Path,
    samples: Sequence[int],
    specs: Sequence[TransformSpec],
    pcm_shift: int,
) -> int:
    record_count = 0
    with output_path.open("w", encoding="ascii", newline="\n") as output:
        for spec in specs:
            for index in range(spec.nfft):
                pcm_index = spec.pcm_start + 2 * index
                real = encode_q1_23(samples[pcm_index], pcm_shift)
                imag = encode_q1_23(samples[pcm_index + 1], pcm_shift)
                output.write(
                    f"{spec.mode} {spec.frame} {index} "
                    f"{hex24(real)} {hex24(imag)}\n"
                )
                record_count += 1
    return record_count


def write_map(
    map_path: Path, specs: Sequence[TransformSpec], pcm_shift: int
) -> None:
    with map_path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(
            (
                "frame",
                "mode",
                "nfft",
                "pcm_start",
                "pcm_end",
                "audio_frame",
                "short_index",
                "pcm_shift",
            )
        )
        for spec in specs:
            writer.writerow(
                (
                    spec.frame,
                    spec.mode,
                    spec.nfft,
                    spec.pcm_start,
                    spec.pcm_start + spec.pcm_count - 1,
                    spec.audio_frame,
                    spec.short_index,
                    pcm_shift,
                )
            )


def write_metadata(
    metadata_path: Path,
    wav_path: Path,
    wav_data: WavData,
    layout: str,
    specs: Sequence[TransformSpec],
    pcm_shift: int,
    record_count: int,
) -> None:
    count_512 = sum(spec.nfft == 512 for spec in specs)
    count_64 = sum(spec.nfft == 64 for spec in specs)
    metadata_path.write_text(
        "\n".join(
            (
                f"source={wav_path}",
                "format=PCM16 mono little-endian",
                f"sampleRate={wav_data.sample_rate}",
                f"totalPcmSamples={len(wav_data.samples)}",
                f"layout={layout}",
                "complexPacking=consecutive PCM samples: even->real, odd->imag",
                f"pcmShift={pcm_shift}",
                f"qFormat=Q1.23 signed {DATA_W}-bit",
                f"transformCount={len(specs)}",
                f"fft512Count={count_512}",
                f"fft64Count={count_64}",
                f"vectorRecordCount={record_count}",
            )
        )
        + "\n",
        encoding="utf-8",
    )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert PCM16 mono WAV samples to complex signed Q1.23 vectors "
            "for the FFT-64/FFT-512 NumPy and RTL models."
        )
    )
    parser.add_argument("--wav", type=Path, default=DEFAULT_WAV)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--layout",
        choices=("tb-mixed", "fft512", "fft64", "both"),
        default="tb-mixed",
        help=(
            "tb-mixed matches the current VHDL testbench; fft512 and fft64 "
            "consume the full WAV; both emits 1x512 plus 8x64 per audio frame"
        ),
    )
    parser.add_argument(
        "--pcm-shift",
        type=int,
        choices=range(0, 9),
        default=7,
        metavar="0..8",
        help="left shift PCM16 into Q1.23; 7 keeps one headroom bit",
    )
    parser.add_argument(
        "--force", action="store_true", help="replace existing output files"
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    wav_path = args.wav.resolve()
    output_path = args.output.resolve()
    map_path = output_path.with_name(f"{output_path.stem}_map.csv")
    metadata_path = output_path.with_name(f"{output_path.stem}_meta.txt")

    try:
        wav_data = read_pcm16_mono_wav(wav_path)
        specs = make_specs(args.layout, len(wav_data.samples))
        if not specs:
            raise ConversionError(
                f"WAV does not contain enough PCM samples for layout {args.layout}"
            )
        ensure_writable((output_path, map_path, metadata_path), args.force)
        record_count = write_vectors(
            output_path, wav_data.samples, specs, args.pcm_shift
        )
        write_map(map_path, specs, args.pcm_shift)
        write_metadata(
            metadata_path,
            wav_path,
            wav_data,
            args.layout,
            specs,
            args.pcm_shift,
            record_count,
        )
    except ConversionError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"ERROR: file operation failed: {exc}", file=sys.stderr)
        return 2

    count_512 = sum(spec.nfft == 512 for spec in specs)
    count_64 = sum(spec.nfft == 64 for spec in specs)
    print(f"OK: read {len(wav_data.samples)} PCM samples at {wav_data.sample_rate} Hz")
    print(
        f"Generated {len(specs)} transforms: "
        f"FFT-512={count_512}, FFT-64={count_64}, records={record_count}"
    )
    print(f"  vectors : {output_path}")
    print(f"  map     : {map_path}")
    print(f"  metadata: {metadata_path}")
    if args.layout != "tb-mixed":
        print(
            "NOTE: this layout requires an EOF-driven VHDL testbench; the current "
            "tb_fft_radix2_core.vhd accepts only layout=tb-mixed."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
