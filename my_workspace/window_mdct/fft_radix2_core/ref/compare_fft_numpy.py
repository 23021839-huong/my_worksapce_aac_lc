#!/usr/bin/env python3
"""Check Q31 FFT vectors with both exact-integer and float64 NumPy models.

Integer comparison is the acceptance gate and must have zero mismatches.
The float64 report is diagnostic: it exposes sign, gain and ordering errors but
does not replace the fixed-point oracle.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

import numpy as np

from fft_radix2_model import (
    OCTANT_SHA256,
    PROFILE_ID,
    Q31_SCALE,
    fft_fixed,
    fft_float,
    read_input_frames,
    read_output_frames,
    write_output_frames,
    write_stage_trace,
)

CORE_ROOT = Path(__file__).resolve().parents[1]


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input", type=Path, default=CORE_ROOT / "vectors/input_fft.txt"
    )
    parser.add_argument(
        "--rtl", type=Path, default=CORE_ROOT / "vectors/output_fft_rtl.txt"
    )
    parser.add_argument(
        "--integer-output",
        type=Path,
        default=CORE_ROOT / "vectors/output_fft_numpy.txt",
    )
    parser.add_argument(
        "--stage-output",
        type=Path,
        default=CORE_ROOT / "vectors/output_fft_numpy_stage.txt",
    )
    parser.add_argument(
        "--float-output",
        type=Path,
        default=CORE_ROOT / "vectors/output_fft_numpy.csv",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=CORE_ROOT / "vectors/compare_fft_numpy.csv",
    )
    return parser.parse_args()


def main() -> int:
    args = arguments()
    try:
        input_frames = read_input_frames(args.input.resolve())
        rtl_frames = read_output_frames(args.rtl.resolve())
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    rtl_by_frame = {frame.frame: frame for frame in rtl_frames}
    integer_frames = []
    trace_rows = []
    mismatch_count = 0
    max_integer_error = 0
    float_error_sq = 0.0
    signal_sq = 0.0
    max_float_error_lsb = 0.0
    total_components = 0

    args.integer_output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.stage_output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.float_output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.report.resolve().parent.mkdir(parents=True, exist_ok=True)

    with args.float_output.resolve().open("w", encoding="utf-8", newline="") as ffloat, \
         args.report.resolve().open("w", encoding="utf-8", newline="") as freport:
        float_writer = csv.writer(ffloat)
        report_writer = csv.writer(freport)
        float_writer.writerow(
            ("Profile", "Frame", "NFFT", "Idx", "FloatRe", "FloatIm")
        )
        report_writer.writerow(
            (
                "Profile",
                "Frame",
                "NFFT",
                "Idx",
                "RtlReI32",
                "RtlImI32",
                "IntegerReI32",
                "IntegerImI32",
                "IntegerErrorReLsb",
                "IntegerErrorImLsb",
                "FloatErrorReLsb",
                "FloatErrorImLsb",
                "ExactMatch",
            )
        )

        for input_frame in input_frames:
            expected, stage_trace = fft_fixed(input_frame)
            integer_frames.append(expected)
            trace_rows.extend(stage_trace)
            actual = rtl_by_frame.get(input_frame.frame)
            if actual is None:
                print(f"error: missing RTL frame {input_frame.frame}", file=sys.stderr)
                return 2
            if (
                actual.mode != expected.mode
                or actual.scale_exp != expected.scale_exp
                or len(actual.samples) != len(expected.samples)
            ):
                print(f"error: metadata mismatch in frame {input_frame.frame}", file=sys.stderr)
                return 2

            floating = fft_float(input_frame)
            for index, (rtl_value, integer_value, float_value) in enumerate(
                zip(actual.samples, expected.samples, floating)
            ):
                err_re = rtl_value.re - integer_value.re
                err_im = rtl_value.im - integer_value.im
                exact = err_re == 0 and err_im == 0
                if not exact:
                    mismatch_count += 1
                max_integer_error = max(max_integer_error, abs(err_re), abs(err_im))

                rtl_re_float = rtl_value.re / Q31_SCALE
                rtl_im_float = rtl_value.im / Q31_SCALE
                float_err_re_lsb = (rtl_re_float - float_value.real) * Q31_SCALE
                float_err_im_lsb = (rtl_im_float - float_value.imag) * Q31_SCALE
                max_float_error_lsb = max(
                    max_float_error_lsb,
                    abs(float_err_re_lsb),
                    abs(float_err_im_lsb),
                )
                float_error_sq += float_err_re_lsb**2 + float_err_im_lsb**2
                signal_sq += (
                    (float_value.real * Q31_SCALE) ** 2
                    + (float_value.imag * Q31_SCALE) ** 2
                )
                total_components += 2

                float_writer.writerow(
                    (
                        PROFILE_ID,
                        input_frame.frame,
                        input_frame.nfft,
                        index,
                        f"{float_value.real:.17e}",
                        f"{float_value.imag:.17e}",
                    )
                )
                report_writer.writerow(
                    (
                        PROFILE_ID,
                        input_frame.frame,
                        input_frame.nfft,
                        index,
                        rtl_value.re,
                        rtl_value.im,
                        integer_value.re,
                        integer_value.im,
                        err_re,
                        err_im,
                        f"{float_err_re_lsb:.9f}",
                        f"{float_err_im_lsb:.9f}",
                        exact,
                    )
                )

    extra_frames = set(rtl_by_frame) - {frame.frame for frame in input_frames}
    if extra_frames:
        print(f"error: extra RTL frames: {sorted(extra_frames)}", file=sys.stderr)
        return 2

    write_output_frames(args.integer_output.resolve(), integer_frames)
    write_stage_trace(args.stage_output.resolve(), trace_rows)

    rms_float_lsb = math.sqrt(float_error_sq / total_components) if total_components else 0.0
    snr_db = (
        10.0 * math.log10(signal_sq / float_error_sq)
        if float_error_sq > 0.0 and signal_sq > 0.0
        else math.inf
    )
    print(f"Profile                : {PROFILE_ID}")
    print(f"Twiddle octant SHA-256 : {OCTANT_SHA256}")
    print(f"Frames                 : {len(input_frames)}")
    print(f"Integer mismatched bins: {mismatch_count}")
    print(f"Integer max error      : {max_integer_error} LSB")
    print(f"Float RMS error        : {rms_float_lsb:.6f} LSB")
    print(f"Float max error        : {max_float_error_lsb:.6f} LSB")
    print(f"Float SNR              : {snr_db:.3f} dB")
    print(f"Report                 : {args.report.resolve()}")
    return 1 if mismatch_count else 0


if __name__ == "__main__":
    raise SystemExit(main())
