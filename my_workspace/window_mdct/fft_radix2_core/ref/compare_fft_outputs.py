#!/usr/bin/env python3
"""Compare FFT reference vectors with an RTL simulation dump.

Both input files use this whitespace-separated format:

    mode frame idx re_hex im_hex scale_exp

The generated CSV has one Reference column, one RTL column, and one ErrorLsb
column.  Error sign convention is RTL - Reference in signed DATA_W-bit LSBs.
The production profile is signed Q1.31 and requires exact zero-LSB agreement.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class VectorRow:
    mode: int
    frame: int
    idx: int
    re_hex: str
    im_hex: str
    scale_exp: int


def project_path(value: str) -> Path:
    """Resolve CLI paths relative to the current terminal directory."""
    path = Path(value).expanduser()
    return path.resolve()


def parse_vector(path: Path, data_w: int) -> list[VectorRow]:
    if not path.is_file():
        raise FileNotFoundError(f"vector file not found: {path}")

    hex_digits = (data_w + 3) // 4
    rows: list[VectorRow] = []
    with path.open("r", encoding="ascii") as stream:
        for line_number, raw_line in enumerate(stream, start=1):
            line = raw_line.strip()
            if not line:
                continue
            field = line.split()
            if len(field) != 6:
                raise ValueError(
                    f"{path}:{line_number}: expected 6 fields, got {len(field)}"
                )
            try:
                if len(field[3]) > hex_digits or len(field[4]) > hex_digits:
                    raise ValueError("hex field is wider than DATA_W")
                re_hex = field[3].upper().zfill(hex_digits)
                im_hex = field[4].upper().zfill(hex_digits)
                int(re_hex, 16)
                int(im_hex, 16)
                row = VectorRow(
                    mode=int(field[0]),
                    frame=int(field[1]),
                    idx=int(field[2]),
                    re_hex=re_hex,
                    im_hex=im_hex,
                    scale_exp=int(field[5]),
                )
            except ValueError as exc:
                raise ValueError(f"{path}:{line_number}: invalid vector field") from exc
            rows.append(row)
    return rows


def hex_to_signed(value: str, data_w: int) -> int:
    raw = int(value, 16)
    mask = (1 << data_w) - 1
    raw &= mask
    sign_bit = 1 << (data_w - 1)
    return raw - (1 << data_w) if raw & sign_bit else raw


def metadata(row: VectorRow) -> tuple[int, int, int, int]:
    return row.mode, row.frame, row.idx, row.scale_exp


def compare(
    reference_rows: Iterable[VectorRow],
    rtl_rows: Iterable[VectorRow],
    report_path: Path,
    data_w: int,
) -> tuple[int, int, int, int, float, float]:
    reference = list(reference_rows)
    rtl = list(rtl_rows)
    if len(reference) != len(rtl):
        raise ValueError(
            f"record count differs: reference={len(reference)}, RTL={len(rtl)}"
        )

    report_path.parent.mkdir(parents=True, exist_ok=True)
    mismatches = 0
    max_abs_re = 0
    max_abs_im = 0
    sum_sq_re = 0
    sum_sq_im = 0

    with report_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=(
                "Mode",
                "Frame",
                "Idx",
                "Reference",
                "RTL",
                "ErrorLsb",
                "Match",
            ),
        )
        writer.writeheader()

        for line_number, (ref, actual) in enumerate(zip(reference, rtl), start=1):
            if metadata(ref) != metadata(actual):
                raise ValueError(
                    f"metadata differs at record {line_number}: "
                    f"reference={metadata(ref)}, RTL={metadata(actual)}"
                )

            error_re = hex_to_signed(actual.re_hex, data_w) - hex_to_signed(
                ref.re_hex, data_w
            )
            error_im = hex_to_signed(actual.im_hex, data_w) - hex_to_signed(
                ref.im_hex, data_w
            )
            match = error_re == 0 and error_im == 0
            if not match:
                mismatches += 1

            max_abs_re = max(max_abs_re, abs(error_re))
            max_abs_im = max(max_abs_im, abs(error_im))
            sum_sq_re += error_re * error_re
            sum_sq_im += error_im * error_im

            writer.writerow(
                {
                    "Mode": ref.mode,
                    "Frame": ref.frame,
                    "Idx": ref.idx,
                    "Reference": f"RE={ref.re_hex} IM={ref.im_hex}",
                    "RTL": f"RE={actual.re_hex} IM={actual.im_hex}",
                    "ErrorLsb": f"dRE={error_re} dIM={error_im}",
                    "Match": match,
                }
            )

    count = len(reference)
    rms_re = math.sqrt(sum_sq_re / count) if count else 0.0
    rms_im = math.sqrt(sum_sq_im / count) if count else 0.0
    return count, mismatches, max_abs_re, max_abs_im, rms_re, rms_im


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--reference",
        default="vectors/output_fft.txt",
        help="reference vector file (default: vectors/output_fft.txt)",
    )
    parser.add_argument(
        "--rtl",
        default="vectors/output_fft_rtl.txt",
        help="RTL dump file (default: vectors/output_fft_rtl.txt)",
    )
    parser.add_argument(
        "--report",
        default="vectors/compare_fft_outputs.csv",
        help="output CSV report (default: vectors/compare_fft_outputs.csv)",
    )
    parser.add_argument(
        "--data-w",
        type=int,
        default=32,
        help="signed data width in bits (default: 32/Q1.31)",
    )
    return parser.parse_args()


def main() -> int:
    args = arguments()
    if not 2 <= args.data_w <= 62:
        print("error: --data-w must be in the range 2..62", file=sys.stderr)
        return 2

    reference_path = project_path(args.reference)
    rtl_path = project_path(args.rtl)
    report_path = project_path(args.report)

    try:
        result = compare(
            parse_vector(reference_path, args.data_w),
            parse_vector(rtl_path, args.data_w),
            report_path,
            args.data_w,
        )
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    count, mismatches, max_abs_re, max_abs_im, rms_re, rms_im = result
    print(f"Compared records : {count}")
    print(f"Mismatched bins  : {mismatches}")
    print(f"Max abs error RE : {max_abs_re} LSB")
    print(f"Max abs error IM : {max_abs_im} LSB")
    print(f"RMS error RE     : {rms_re:.6f} LSB")
    print(f"RMS error IM     : {rms_im:.6f} LSB")
    print(f"1 LSB (Q1.{args.data_w - 1})   : {2.0 ** -(args.data_w - 1):.12E}")
    print(f"CSV report       : {report_path}")
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
