#!/usr/bin/env python3
"""Plot FFT input and C++/RTL/NumPy outputs for every verification frame."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.backends.backend_pdf import PdfPages


CORE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CORE_ROOT / "ref"))

from fft_radix2_model import Q31_SCALE, fft_float, read_input_frames, read_output_frames


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=CORE_ROOT / "vectors/input_fft.txt")
    parser.add_argument("--rtl", type=Path, default=CORE_ROOT / "vectors/output_fft_rtl.txt")
    parser.add_argument("--cpp", type=Path, default=CORE_ROOT / "vectors/output_fft_cpp.txt")
    parser.add_argument(
        "--numpy-integer",
        type=Path,
        default=CORE_ROOT / "vectors/output_fft_numpy.txt",
    )
    parser.add_argument("--out-dir", type=Path, default=CORE_ROOT / "build/fft_plots")
    parser.add_argument(
        "--frames",
        type=int,
        nargs="*",
        help="Frame numbers to plot; omit to plot every frame.",
    )
    return parser.parse_args()


def output_map(path: Path):
    return {frame.frame: frame for frame in read_output_frames(path.resolve())}


def complex_q31(samples) -> np.ndarray:
    return np.asarray([complex(value.re, value.im) for value in samples], dtype=np.complex128)


def dbfs(values: np.ndarray) -> np.ndarray:
    floor = 1.0 / Q31_SCALE
    return 20.0 * np.log10(np.maximum(np.abs(values), floor))


def require_matching(reference, candidate, name: str) -> None:
    if candidate is None:
        raise ValueError(f"missing {name} frame {reference.frame}")
    if candidate.mode != reference.mode or len(candidate.samples) != reference.nfft:
        raise ValueError(f"metadata mismatch for {name} frame {reference.frame}")


def plot_frame(input_frame, rtl_frame, cpp_frame, numpy_frame):
    require_matching(input_frame, rtl_frame, "RTL")
    require_matching(input_frame, cpp_frame, "C++")
    require_matching(input_frame, numpy_frame, "NumPy integer")

    nfft = input_frame.nfft
    x = np.arange(nfft)
    input_q31 = complex_q31(input_frame.samples) / Q31_SCALE
    rtl_i32 = complex_q31(rtl_frame.samples)
    cpp_i32 = complex_q31(cpp_frame.samples)
    numpy_i32 = complex_q31(numpy_frame.samples)
    rtl = rtl_i32 / Q31_SCALE
    cpp = cpp_i32 / Q31_SCALE
    numpy_integer = numpy_i32 / Q31_SCALE
    numpy_float = np.asarray(fft_float(input_frame), dtype=np.complex128)

    integer_error_cpp = cpp_i32 - rtl_i32
    integer_error_numpy = numpy_i32 - rtl_i32
    float_error_lsb = (rtl - numpy_float) * Q31_SCALE

    fig, axes = plt.subplots(3, 2, figsize=(16, 12), constrained_layout=True)
    fig.suptitle(
        f"FFT frame {input_frame.frame}: N={nfft}, mode={input_frame.mode}, "
        f"scale_exp={rtl_frame.scale_exp}"
    )

    ax = axes[0, 0]
    ax.plot(x, input_q31.real, label="Input real", linewidth=1.0)
    ax.plot(x, input_q31.imag, label="Input imag", linewidth=1.0)
    ax.set_title("Input samples")
    ax.set_ylabel("Amplitude (Q31 normalized)")
    ax.legend()

    ax = axes[0, 1]
    ax.plot(x, dbfs(numpy_float), label="NumPy float64", linewidth=1.4)
    ax.plot(x, dbfs(cpp), "--", label="C++ fixed", linewidth=1.0)
    ax.plot(x, dbfs(rtl), ":", label="RTL", linewidth=1.2)
    ax.plot(x, dbfs(numpy_integer), "-.", label="NumPy integer", linewidth=0.9)
    ax.set_title("Output magnitude")
    ax.set_ylabel("Magnitude (dBFS)")
    ax.legend()

    ax = axes[1, 0]
    ax.plot(x, numpy_float.real, label="NumPy float64", linewidth=1.4)
    ax.plot(x, cpp.real, "--", label="C++ fixed", linewidth=1.0)
    ax.plot(x, rtl.real, ":", label="RTL", linewidth=1.2)
    ax.plot(x, numpy_integer.real, "-.", label="NumPy integer", linewidth=0.9)
    ax.set_title("FFT output: real component")
    ax.set_ylabel("Amplitude (Q31 normalized)")
    ax.legend()

    ax = axes[1, 1]
    ax.plot(x, numpy_float.imag, label="NumPy float64", linewidth=1.4)
    ax.plot(x, cpp.imag, "--", label="C++ fixed", linewidth=1.0)
    ax.plot(x, rtl.imag, ":", label="RTL", linewidth=1.2)
    ax.plot(x, numpy_integer.imag, "-.", label="NumPy integer", linewidth=0.9)
    ax.set_title("FFT output: imaginary component")
    ax.set_ylabel("Amplitude (Q31 normalized)")
    ax.legend()

    ax = axes[2, 0]
    ax.plot(x, integer_error_cpp.real, label="C++−RTL real", linewidth=1.0)
    ax.plot(x, integer_error_cpp.imag, label="C++−RTL imag", linewidth=1.0)
    ax.plot(x, integer_error_numpy.real, "--", label="NumPy int−RTL real", linewidth=1.0)
    ax.plot(x, integer_error_numpy.imag, "--", label="NumPy int−RTL imag", linewidth=1.0)
    ax.set_title("Exact integer error")
    ax.set_ylabel("Error (LSB)")
    ax.legend()

    ax = axes[2, 1]
    ax.plot(x, float_error_lsb.real, label="RTL−NumPy float real", linewidth=1.0)
    ax.plot(x, float_error_lsb.imag, label="RTL−NumPy float imag", linewidth=1.0)
    rms = math.sqrt(np.mean(np.abs(float_error_lsb) ** 2) / 2.0)
    maximum = max(np.max(np.abs(float_error_lsb.real)), np.max(np.abs(float_error_lsb.imag)))
    ax.set_title(f"Float64 difference: RMS={rms:.3f} LSB, max={maximum:.3f} LSB")
    ax.set_ylabel("Error (LSB)")
    ax.legend()

    for ax in axes.flat:
        ax.set_xlabel("Sample/bin index")
        ax.grid(True, alpha=0.25)

    stats = {
        "Frame": input_frame.frame,
        "Mode": input_frame.mode,
        "NFFT": nfft,
        "ScaleExp": rtl_frame.scale_exp,
        "CppRtlMaxErrorLsb": int(
            max(np.max(np.abs(integer_error_cpp.real)), np.max(np.abs(integer_error_cpp.imag)))
        ),
        "NumpyIntegerRtlMaxErrorLsb": int(
            max(
                np.max(np.abs(integer_error_numpy.real)),
                np.max(np.abs(integer_error_numpy.imag)),
            )
        ),
        "FloatRmsErrorLsb": rms,
        "FloatMaxErrorLsb": maximum,
    }
    return fig, stats


def main() -> int:
    args = arguments()
    try:
        inputs = read_input_frames(args.input.resolve())
        rtl = output_map(args.rtl)
        cpp = output_map(args.cpp)
        numpy_integer = output_map(args.numpy_integer)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    selected = set(args.frames) if args.frames else {frame.frame for frame in inputs}
    unknown = selected - {frame.frame for frame in inputs}
    if unknown:
        print(f"error: unknown frame numbers: {sorted(unknown)}", file=sys.stderr)
        return 2

    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = out_dir / "fft_all_frames.pdf"
    summary_path = out_dir / "fft_error_summary.csv"
    stats_rows = []

    try:
        with PdfPages(pdf_path) as pdf:
            for input_frame in inputs:
                if input_frame.frame not in selected:
                    continue
                fig, stats = plot_frame(
                    input_frame,
                    rtl.get(input_frame.frame),
                    cpp.get(input_frame.frame),
                    numpy_integer.get(input_frame.frame),
                )
                png_path = out_dir / f"fft_frame_{input_frame.frame:02d}_n{input_frame.nfft}.png"
                fig.savefig(png_path, dpi=150)
                pdf.savefig(fig)
                plt.close(fig)
                stats_rows.append(stats)
                print(f"Wrote {png_path}")
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    with summary_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(stats_rows[0]))
        writer.writeheader()
        writer.writerows(stats_rows)

    print(f"Wrote {pdf_path}")
    print(f"Wrote {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
