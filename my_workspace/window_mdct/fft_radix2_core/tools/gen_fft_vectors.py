#!/usr/bin/env python3
"""Generate non-zero Q31 regression vectors for FFT64 and FFT512.

The input suite is deterministic and keeps the same /4 headroom supplied by
the MDCT pre-twiddle.  The bit-exact Python model writes provisional golden and
stage traces; the C++ reference is required to reproduce them with zero LSB.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path


CORE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CORE_ROOT / "ref"))

from fft_radix2_model import (  # noqa: E402
    ComplexQ31,
    InputFrame,
    OCTANT_SHA256,
    PROFILE_ID,
    fft_fixed,
    q31_hex,
    write_output_frames,
    write_stage_trace,
)


FRAME_SIZES = (512, 64, 64, 64, 64, 64, 64, 64, 64, 512, 512, 512)
CASE_NAMES = (
    "fft512_zero",
    "fft64_impulse",
    "fft64_complex_tone",
    "fft64_alternating",
    "fft64_seeded_noise",
    "fft64_two_tone",
    "fft64_dc",
    "fft64_edge_impulse",
    "fft64_off_bin",
    "fft512_complex_tone",
    "fft512_seeded_noise",
    "fft512_edge_mix",
)


def q31(value: float) -> int:
    scaled = value * (1 << 31)
    rounded = math.floor(scaled + 0.5) if scaled >= 0 else math.ceil(scaled - 0.5)
    return max(-(1 << 31), min((1 << 31) - 1, rounded))


def sample(frame: int, index: int, nfft: int) -> ComplexQ31:
    t = index / nfft
    re = 0.0
    im = 0.0
    if frame == 1:
        re = 0.24 if index == 0 else 0.0
        im = -0.20 if index == 3 else 0.0
    elif frame == 2:
        re = 0.18 * math.cos(2 * math.pi * 7 * t)
        im = 0.17 * math.sin(2 * math.pi * 7 * t)
    elif frame == 3:
        re = -0.24 if index & 1 else 0.24
        im = 0.125 if index & 2 else -0.125
    elif frame in (4, 10):
        state = (0x9E3779B9 ^ (frame * 0x1021 + index * 0x45D9F3B)) & 0xFFFFFFFF
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        state &= 0xFFFFFFFF
        re = ((state & 0xFFFF) - 32768) / 32768.0 * 0.22
        im = (((state >> 16) & 0xFFFF) - 32768) / 32768.0 * 0.22
    elif frame == 5:
        re = 0.13 * math.cos(2 * math.pi * 3 * t) + 0.08 * math.sin(
            2 * math.pi * 11 * t
        )
        im = -0.12 * math.sin(2 * math.pi * 5 * t)
    elif frame == 6:
        re, im = 0.18, -0.09
    elif frame == 7:
        re = -0.249999 if index == nfft // 2 else 0.0
        im = 0.249999 if index == nfft // 2 + 1 else 0.0
    elif frame == 8:
        re = 0.20 * math.cos(2 * math.pi * 6.375 * t)
        im = 0.15 * math.sin(2 * math.pi * 9.125 * t)
    elif frame == 9:
        re = 0.16 * math.cos(2 * math.pi * 31 * t) + 0.07 * math.sin(
            2 * math.pi * 79 * t
        )
        im = 0.14 * math.sin(2 * math.pi * 17 * t)
    elif frame == 11:
        re = 0.24 if index == 0 else (-0.015 if index & 1 else 0.015)
        im = -0.23 if index == nfft // 3 else 0.0
    return ComplexQ31(q31(re), q31(im))


def generate(output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    frames: list[InputFrame] = []
    with (output_dir / "input_fft.txt").open(
        "w", encoding="ascii", newline="\n"
    ) as stream:
        for frame_number, nfft in enumerate(FRAME_SIZES):
            mode = 1 if nfft == 512 else 0
            samples = tuple(sample(frame_number, index, nfft) for index in range(nfft))
            frames.append(InputFrame(mode, frame_number, samples))
            for index, value in enumerate(samples):
                stream.write(
                    f"{mode} {frame_number} {index} "
                    f"{q31_hex(value.re)} {q31_hex(value.im)}\n"
                )

    outputs = []
    trace = []
    for frame in frames:
        output, rows = fft_fixed(frame)
        outputs.append(output)
        trace.extend(rows)
    write_output_frames(output_dir / "output_fft.txt", outputs)
    write_stage_trace(output_dir / "output_fft_stage.txt", trace)

    with (output_dir / "manifest.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.writer(stream)
        writer.writerow(
            (
                "profile",
                "twiddle_sha256",
                "frame",
                "case",
                "mode",
                "nfft",
                "scale_exp",
                "golden_generator",
            )
        )
        for frame_number, nfft in enumerate(FRAME_SIZES):
            writer.writerow(
                (
                    PROFILE_ID,
                    OCTANT_SHA256,
                    frame_number,
                    CASE_NAMES[frame_number],
                    1 if nfft == 512 else 0,
                    nfft,
                    8 if nfft == 512 else 5,
                    "python_integer; must equal C++ with 0 LSB",
                )
            )

    (output_dir / "contract.meta").write_text(
        "\n".join(
            (
                f"profile={PROFILE_ID}",
                "fdk_commit=35f9c13cb6df0c5d4e7ba958ef2d251c48b8d1d9",
                f"twiddle_octant_sha256={OCTANT_SHA256}",
                "data=signed_q1.31",
                "coeff=signed_q1.15",
                "direction=forward_negative_exponent",
                "scale_mask_fft64=101111",
                "scale_mask_fft512=101111111",
                "rounding=arithmetic_truncation",
                "saturation=none",
                "overflow=wrap32",
                "",
            )
        ),
        encoding="ascii",
        newline="\n",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=CORE_ROOT / "vectors",
        help="vector directory (default: fft_radix2_core/vectors)",
    )
    args = parser.parse_args()
    generate(args.output_dir.resolve())
    print(f"generated: {args.output_dir.resolve()}")


if __name__ == "__main__":
    main()
