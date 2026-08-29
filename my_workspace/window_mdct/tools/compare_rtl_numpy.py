#!/usr/bin/env python3
"""Compare every RTL MDCT output with a NumPy float64 reference."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path
from typing import Sequence

import numpy as np


WINDOW_ROOT = Path(__file__).resolve().parents[1]
REF_DIR = WINDOW_ROOT / "ref"
if str(REF_DIR) not in sys.path:
    sys.path.insert(0, str(REF_DIR))

from mdct_radix2_model import (  # noqa: E402
    BlockType,
    FdkRom,
    MdctRadix2Model,
    Q31_SCALE,
    WindowShape,
    dct4_float64,
)


DEFAULT_VECTORS = WINDOW_ROOT / "build" / "vectors"
DEFAULT_REPORT = WINDOW_ROOT / "build" / "rtl_numpy_compare"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vectors", type=Path, default=DEFAULT_VECTORS)
    parser.add_argument("--rtl", type=Path, help="RTL dump; defaults inside --vectors")
    parser.add_argument("--out", type=Path, default=DEFAULT_REPORT)
    return parser.parse_args(argv)


def signed_hex(token: str, bits: int) -> int:
    value = int(token, 16)
    if value >= 1 << (bits - 1):
        value -= 1 << bits
    return value


def read_cases(path: Path) -> dict[int, str]:
    with path.open(newline="", encoding="utf-8") as stream:
        return {int(row["case_id"]): row["name"] for row in csv.DictReader(stream)}


def read_frame_metadata(path: Path) -> list[dict[str, int]]:
    frames: list[dict[str, int]] = []
    seen: set[tuple[int, int]] = set()
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            key = (int(row["case_id"]), int(row["frame_id"]))
            if key in seen:
                continue
            seen.add(key)
            frames.append(
                {
                    "case_id": key[0],
                    "frame_id": key[1],
                    "reset_before": int(row["reset_before"]),
                    "block_type": int(row["block_type"]),
                    "right_shape": int(row["right_shape"]),
                }
            )
    return frames


def read_pcm(path: Path) -> dict[tuple[int, int], list[int]]:
    frames: dict[tuple[int, int], list[int]] = {}
    with path.open(encoding="ascii") as stream:
        for line_number, line in enumerate(stream, start=1):
            fields = line.split()
            if len(fields) != 4:
                raise ValueError(f"{path}:{line_number}: expected 4 fields")
            case_id, frame_id, index = map(int, fields[:3])
            values = frames.setdefault((case_id, frame_id), [])
            if index != len(values):
                raise ValueError(f"{path}:{line_number}: unexpected PCM index {index}")
            values.append(signed_hex(fields[3], 16))
    return frames


def read_rtl(path: Path) -> dict[tuple[int, int], tuple[list[int], int]]:
    values: dict[tuple[int, int], list[int]] = {}
    exponents: dict[tuple[int, int], int] = {}
    with path.open(encoding="ascii") as stream:
        for line_number, line in enumerate(stream, start=1):
            fields = line.split()
            if len(fields) != 5:
                raise ValueError(f"{path}:{line_number}: expected 5 fields")
            case_id, frame_id, index = map(int, fields[:3])
            key = (case_id, frame_id)
            frame_values = values.setdefault(key, [])
            if index != len(frame_values):
                raise ValueError(f"{path}:{line_number}: unexpected RTL output index {index}")
            frame_values.append(signed_hex(fields[3], 32))
            exponent = int(fields[4])
            if key in exponents and exponents[key] != exponent:
                raise ValueError(f"{path}:{line_number}: exponent changed within one frame")
            exponents[key] = exponent
    return {key: (frame_values, exponents[key]) for key, frame_values in values.items()}


def error_metrics(actual: np.ndarray, reference: np.ndarray) -> tuple[float, float, float, float]:
    error = actual - reference
    error_power = float(np.mean(error * error, dtype=np.float64))
    reference_power = float(np.mean(reference * reference, dtype=np.float64))
    rms_error = math.sqrt(error_power)
    reference_rms = math.sqrt(reference_power)
    relative_rms = rms_error / reference_rms if reference_rms else rms_error
    max_abs_error = float(np.max(np.abs(error)))
    if error_power == 0.0:
        snr_db = math.inf
    elif reference_power == 0.0:
        snr_db = -math.inf
    else:
        snr_db = 10.0 * math.log10(reference_power / error_power)
    return rms_error, relative_rms, max_abs_error, snr_db


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    vectors = args.vectors.resolve()
    rtl_path = (args.rtl or vectors / "rtl_mdct_out_q31.txt").resolve()
    report_dir = args.out.resolve()
    required = ("cases.csv", "manifest.csv", "pcm_in_s16.txt")
    missing = [vectors / name for name in required if not (vectors / name).is_file()]
    if missing:
        raise SystemExit("ERROR: missing vector file(s): " + ", ".join(map(str, missing)))
    if not rtl_path.is_file():
        raise SystemExit(f"ERROR: RTL dump not found: {rtl_path}")

    case_names = read_cases(vectors / "cases.csv")
    metadata = read_frame_metadata(vectors / "manifest.csv")
    pcm_frames = read_pcm(vectors / "pcm_in_s16.txt")
    rtl_frames = read_rtl(rtl_path)
    report_dir.mkdir(parents=True, exist_ok=True)

    input_path = report_dir / "all_inputs.csv"
    output_path = report_dir / "all_output_errors.csv"
    frame_path = report_dir / "all_frame_errors.csv"
    model = MdctRadix2Model(FdkRom())
    total_outputs = 0
    summaries: list[dict[str, object]] = []

    with (
        input_path.open("w", newline="", encoding="utf-8") as input_stream,
        output_path.open("w", newline="", encoding="utf-8") as output_stream,
        frame_path.open("w", newline="", encoding="utf-8") as frame_stream,
    ):
        input_writer = csv.writer(input_stream, lineterminator="\n")
        output_writer = csv.writer(output_stream, lineterminator="\n")
        frame_writer = csv.writer(frame_stream, lineterminator="\n")
        input_writer.writerow(
            (
                "case_id", "case_name", "frame_id", "input_index", "rtl_pcm_s16",
                "numpy_pcm_s16", "signed_error_s16", "pcm_normalized",
            )
        )
        output_writer.writerow(
            (
                "case_id", "case_name", "frame_id", "block_type", "sub_id",
                "output_index", "sub_index", "rtl_exponent", "rtl_q31",
                "rtl_physical", "numpy_float64", "signed_error", "abs_error",
                "numpy_q31_equivalent", "signed_error_lsb", "relative_error",
            )
        )
        frame_writer.writerow(
            (
                "case_id", "case_name", "frame_id", "block_type", "input_count",
                "output_count", "input_min", "input_max", "input_rms",
                "input_max_abs_error_s16", "rtl_exponent", "rms_error", "relative_rms",
                "max_abs_error", "max_abs_error_lsb", "worst_output_index", "snr_db",
            )
        )

        active_case = None
        for meta in metadata:
            case_id = meta["case_id"]
            frame_id = meta["frame_id"]
            key = (case_id, frame_id)
            if key not in pcm_frames or key not in rtl_frames:
                raise ValueError(f"missing PCM or RTL data for case/frame {key}")
            pcm = pcm_frames[key]
            rtl_raw, rtl_exp = rtl_frames[key]
            if len(pcm) != 2048 or len(rtl_raw) != 1024:
                raise ValueError(
                    f"case/frame {key}: expected 2048 inputs and 1024 outputs, "
                    f"got {len(pcm)} and {len(rtl_raw)}"
                )
            if active_case != case_id or meta["reset_before"]:
                model.reset()
                active_case = case_id

            trace = model.process_frame(
                pcm,
                BlockType(meta["block_type"]),
                WindowShape(meta["right_shape"]),
                frame_id,
            )
            numpy_parts = []
            for sub in trace.subblocks:
                folded_physical = np.asarray(sub.fold, dtype=np.float64) * (
                    2.0**sub.exp_fold / Q31_SCALE
                )
                numpy_parts.append(np.asarray(dct4_float64(folded_physical)))
            numpy_output = np.concatenate(numpy_parts)
            rtl_physical = np.asarray(rtl_raw, dtype=np.float64) * (2.0**rtl_exp / Q31_SCALE)
            error = rtl_physical - numpy_output
            abs_error = np.abs(error)
            numpy_q31 = numpy_output * (Q31_SCALE / 2.0**rtl_exp)
            error_lsb = np.asarray(rtl_raw, dtype=np.float64) - numpy_q31
            relative_error = np.divide(
                abs_error,
                np.abs(numpy_output),
                out=np.zeros_like(abs_error),
                where=numpy_output != 0.0,
            )
            relative_error[(numpy_output == 0.0) & (abs_error != 0.0)] = np.inf

            case_name = case_names.get(case_id, "unknown")
            block_name = BlockType(meta["block_type"]).name
            sub_length = 128 if meta["block_type"] == int(BlockType.SHORT) else 1024
            for index, sample in enumerate(pcm):
                input_writer.writerow(
                    (
                        case_id, case_name, frame_id, index, sample, sample, 0,
                        format(sample / 32768.0, ".17g"),
                    )
                )
            for index in range(1024):
                output_writer.writerow(
                    (
                        case_id, case_name, frame_id, block_name, index // sub_length,
                        index, index % sub_length, rtl_exp, rtl_raw[index],
                        format(rtl_physical[index], ".17g"),
                        format(numpy_output[index], ".17g"), format(error[index], ".17g"),
                        format(abs_error[index], ".17g"), format(numpy_q31[index], ".17g"),
                        format(error_lsb[index], ".17g"), format(relative_error[index], ".17g"),
                    )
                )

            rms, rel_rms, max_abs, snr = error_metrics(rtl_physical, numpy_output)
            worst_index = int(np.argmax(abs_error))
            pcm_array = np.asarray(pcm, dtype=np.float64)
            input_rms = math.sqrt(float(np.mean(pcm_array * pcm_array, dtype=np.float64)))
            summary = {
                "case_id": case_id,
                "case_name": case_name,
                "frame_id": frame_id,
                "block_type": block_name,
                "input_count": len(pcm),
                "output_count": len(rtl_raw),
                "input_min": min(pcm),
                "input_max": max(pcm),
                "input_rms": input_rms,
                "input_max_abs_error_s16": 0,
                "rtl_exponent": rtl_exp,
                "rms_error": rms,
                "relative_rms": rel_rms,
                "max_abs_error": max_abs,
                "max_abs_error_lsb": float(np.max(np.abs(error_lsb))),
                "worst_output_index": worst_index,
                "snr_db": snr,
            }
            summaries.append(summary)
            frame_writer.writerow(
                tuple(
                    format(value, ".17g") if isinstance(value, float) else value
                    for value in summary.values()
                )
            )
            total_outputs += len(rtl_raw)

    worst = max(summaries, key=lambda row: float(row["max_abs_error"]))
    print(f"PASS: compared {len(summaries)} frames, {total_outputs} RTL outputs")
    print(f"Inputs             : {input_path}")
    print(f"All output errors  : {output_path}")
    print(f"Per-frame summary  : {frame_path}")
    print(
        "Worst frame        : "
        f"case={worst['case_id']} ({worst['case_name']}), frame={worst['frame_id']}, "
        f"block={worst['block_type']}, output={worst['worst_output_index']}"
    )
    print(f"Max absolute error : {float(worst['max_abs_error']):.6e}")
    print(f"Relative RMS       : {float(worst['relative_rms']):.6e}")
    print(f"SNR                : {float(worst['snr_db']):.3f} dB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
