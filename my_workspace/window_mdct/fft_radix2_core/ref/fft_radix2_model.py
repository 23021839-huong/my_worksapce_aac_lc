"""Bit-exact NumPy/Python model of the FDK-compatible radix-2 FFT.

The integer path intentionally uses Python integers for intermediate values and
applies ``wrap32`` at the same boundaries as RTL/C++.  NumPy's FFT is used only
by the independent floating-point function at the bottom of this file.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Iterable

if TYPE_CHECKING:
    import numpy as np
    from numpy.typing import NDArray

from fdk_sinetable512_q15 import OCTANT_Q15, OCTANT_SHA256


PROFILE_ID = "FDK_AACLC_1024_Q31Q15_RAD2_V1"
DATA_WIDTH = 32
FRACTION_BITS = 31
Q31_SCALE = float(1 << FRACTION_BITS)
MODE_TO_NFFT = {0: 64, 1: 512}
NFFT_TO_SCALE_EXP = {64: 5, 512: 8}


# ---------------------------------------------------------------------------
# SECTION 1: typed records shared by vector I/O and the transform.
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class ComplexQ31:
    re: int
    im: int


@dataclass(frozen=True)
class InputFrame:
    mode: int
    frame: int
    samples: tuple[ComplexQ31, ...]

    @property
    def nfft(self) -> int:
        return MODE_TO_NFFT[self.mode]


@dataclass(frozen=True)
class OutputFrame:
    mode: int
    frame: int
    scale_exp: int
    samples: tuple[ComplexQ31, ...]


@dataclass(frozen=True)
class StageRow:
    mode: int
    frame: int
    stage: int
    pair: int
    addr_a: int
    addr_b: int
    a: ComplexQ31
    b: ComplexQ31


# ---------------------------------------------------------------------------
# SECTION 2: explicit two's-complement and fixed-point primitives.
# ---------------------------------------------------------------------------
def wrap32(value: int) -> int:
    """Return the low 32 bits interpreted as signed two's complement."""

    raw = int(value) & 0xFFFFFFFF
    return raw - 0x100000000 if raw & 0x80000000 else raw


def asr(value: int, shift: int) -> int:
    """Arithmetic right shift; Python defines the required floor behavior."""

    if shift < 0:
        raise ValueError("shift must be non-negative")
    return int(value) >> shift


def mul_div2_q31_q15(data: int, coefficient: int) -> int:
    return wrap32(asr(int(data) * int(coefficient), 16))


def bit_reverse(value: int, bits: int) -> int:
    result = 0
    for _ in range(bits):
        result = (result << 1) | (value & 1)
        value >>= 1
    return result


# ---------------------------------------------------------------------------
# SECTION 3: FDK twiddle folding and exact W=1/W=-j paths.
# ---------------------------------------------------------------------------
def decode_twiddle(phase: int) -> tuple[int, int, bool]:
    """Return positive |cos|/|sin| Q15 values and the sign of cos."""

    if not 0 <= phase <= 255:
        raise ValueError(f"phase index outside 0..255: {phase}")
    swap = False
    cos_negative = False
    if phase <= 64:
        base = phase
    elif phase <= 128:
        base = 128 - phase
        swap = True
    elif phase <= 192:
        base = phase - 128
        swap = True
        cos_negative = True
    else:
        base = 256 - phase
        cos_negative = True
    cos_q15, sin_q15 = OCTANT_Q15[base]
    if swap:
        cos_q15, sin_q15 = sin_q15, cos_q15
    return cos_q15, sin_q15, cos_negative


def apply_sign_after_truncation(value: int, negative: bool) -> int:
    return wrap32(-value) if negative else wrap32(value)


def multiply_forward_twiddle(value: ComplexQ31, phase: int) -> ComplexQ31:
    if phase == 0:
        return ComplexQ31(asr(value.re, 1), asr(value.im, 1))
    if phase == 128:
        # FDK's second block-1 branch implements -j with shifts and swaps.
        # Negation is applied after the arithmetic shift.
        return ComplexQ31(asr(value.im, 1), wrap32(-asr(value.re, 1)))

    cos_mag, sin_mag, cos_negative = decode_twiddle(phase)
    br_cos = apply_sign_after_truncation(
        mul_div2_q31_q15(value.re, cos_mag), cos_negative
    )
    bi_cos = apply_sign_after_truncation(
        mul_div2_q31_q15(value.im, cos_mag), cos_negative
    )
    br_sin = mul_div2_q31_q15(value.re, sin_mag)
    bi_sin = mul_div2_q31_q15(value.im, sin_mag)
    return ComplexQ31(wrap32(br_cos + bi_sin), wrap32(bi_cos - br_sin))


# ---------------------------------------------------------------------------
# SECTION 4: bit-exact pure radix-2 DIT transform and per-stage trace.
# ---------------------------------------------------------------------------
def fft_fixed(frame: InputFrame) -> tuple[OutputFrame, tuple[StageRow, ...]]:
    nfft = frame.nfft
    if len(frame.samples) != nfft:
        raise ValueError(
            f"frame {frame.frame}: expected {nfft} samples, got {len(frame.samples)}"
        )
    ldn = 9 if nfft == 512 else 6
    source = [ComplexQ31(0, 0) for _ in range(nfft)]
    destination = [ComplexQ31(0, 0) for _ in range(nfft)]
    for index, sample in enumerate(frame.samples):
        source[bit_reverse(index, ldn)] = sample

    trace: list[StageRow] = []
    for stage in range(1, ldn + 1):
        m = 1 << stage
        half = m >> 1
        phase_step = 1 << (9 - stage)
        pair = 0
        for group_base in range(0, nfft, m):
            phase = 0
            for j in range(half):
                addr_a = group_base + j
                addr_b = addr_a + half
                a = source[addr_a]
                b = source[addr_b]

                if stage == 1:
                    top = ComplexQ31(asr(a.re + b.re, 1), asr(a.im + b.im, 1))
                    bottom = ComplexQ31(
                        wrap32(top.re - b.re), wrap32(top.im - b.im)
                    )
                else:
                    if stage == 2:
                        u = a
                        if phase == 0:
                            twiddled = b
                        elif phase == 128:
                            twiddled = ComplexQ31(b.im, wrap32(-b.re))
                        else:
                            raise AssertionError("stage-2 phase is not 0/-j")
                    else:
                        u = ComplexQ31(asr(a.re, 1), asr(a.im, 1))
                        twiddled = multiply_forward_twiddle(b, phase)
                    top = ComplexQ31(
                        wrap32(u.re + twiddled.re), wrap32(u.im + twiddled.im)
                    )
                    bottom = ComplexQ31(
                        wrap32(u.re - twiddled.re), wrap32(u.im - twiddled.im)
                    )

                destination[addr_a] = top
                destination[addr_b] = bottom
                trace.append(
                    StageRow(
                        frame.mode,
                        frame.frame,
                        stage,
                        pair,
                        addr_a,
                        addr_b,
                        top,
                        bottom,
                    )
                )
                pair += 1
                phase += phase_step
        source, destination = destination, source

    return (
        OutputFrame(
            frame.mode,
            frame.frame,
            ldn - 1,
            tuple(source),
        ),
        tuple(trace),
    )


# ---------------------------------------------------------------------------
# SECTION 5: canonical text-vector readers and writers.
# ---------------------------------------------------------------------------
def parse_q31_hex(token: str) -> int:
    if not 1 <= len(token) <= 8:
        raise ValueError(f"Q31 token must contain 1..8 hex digits: {token!r}")
    raw = int(token, 16)
    if raw > 0xFFFFFFFF:
        raise ValueError(f"Q31 token exceeds 32 bits: {token!r}")
    return wrap32(raw)


def q31_hex(value: int) -> str:
    return f"{int(value) & 0xFFFFFFFF:08X}"


def read_input_frames(path: Path) -> list[InputFrame]:
    builders: dict[int, tuple[int, list[ComplexQ31]]] = {}
    with path.open("r", encoding="ascii") as stream:
        for line_number, raw_line in enumerate(stream, start=1):
            if not raw_line.strip():
                continue
            fields = raw_line.split()
            if len(fields) != 5:
                raise ValueError(f"{path}:{line_number}: expected 5 fields")
            mode, frame_number, index = map(int, fields[:3])
            if mode not in MODE_TO_NFFT or frame_number < 0 or index < 0:
                raise ValueError(f"{path}:{line_number}: invalid metadata")
            builder = builders.setdefault(frame_number, (mode, []))
            if builder[0] != mode or index != len(builder[1]):
                raise ValueError(f"{path}:{line_number}: non-contiguous frame")
            builder[1].append(
                ComplexQ31(parse_q31_hex(fields[3]), parse_q31_hex(fields[4]))
            )

    result: list[InputFrame] = []
    for frame_number, (mode, samples) in builders.items():
        expected = MODE_TO_NFFT[mode]
        if len(samples) != expected:
            raise ValueError(
                f"{path}: frame {frame_number} has {len(samples)}, expected {expected}"
            )
        result.append(InputFrame(mode, frame_number, tuple(samples)))
    if not result:
        raise ValueError(f"{path}: no input frames")
    return result


def read_output_frames(path: Path) -> list[OutputFrame]:
    builders: dict[int, tuple[int, int, list[ComplexQ31]]] = {}
    with path.open("r", encoding="ascii") as stream:
        for line_number, raw_line in enumerate(stream, start=1):
            if not raw_line.strip():
                continue
            fields = raw_line.split()
            if len(fields) != 6:
                raise ValueError(f"{path}:{line_number}: expected 6 fields")
            mode, frame_number, index = map(int, fields[:3])
            scale_exp = int(fields[5])
            builder = builders.setdefault(frame_number, (mode, scale_exp, []))
            if builder[:2] != (mode, scale_exp) or index != len(builder[2]):
                raise ValueError(f"{path}:{line_number}: inconsistent output frame")
            builder[2].append(
                ComplexQ31(parse_q31_hex(fields[3]), parse_q31_hex(fields[4]))
            )

    result: list[OutputFrame] = []
    for frame_number, (mode, scale_exp, samples) in builders.items():
        nfft = MODE_TO_NFFT.get(mode)
        if nfft is None or len(samples) != nfft or scale_exp != NFFT_TO_SCALE_EXP[nfft]:
            raise ValueError(f"{path}: invalid frame {frame_number}")
        result.append(OutputFrame(mode, frame_number, scale_exp, tuple(samples)))
    if not result:
        raise ValueError(f"{path}: no output frames")
    return result


def write_output_frames(path: Path, frames: Iterable[OutputFrame]) -> None:
    with path.open("w", encoding="ascii", newline="\n") as stream:
        for frame in frames:
            for index, sample in enumerate(frame.samples):
                stream.write(
                    f"{frame.mode} {frame.frame} {index} "
                    f"{q31_hex(sample.re)} {q31_hex(sample.im)} {frame.scale_exp}\n"
                )


def write_stage_trace(path: Path, rows: Iterable[StageRow]) -> None:
    with path.open("w", encoding="ascii", newline="\n") as stream:
        for row in rows:
            stream.write(
                f"{row.mode} {row.frame} {row.stage} {row.pair} "
                f"{row.addr_a} {row.addr_b} {q31_hex(row.a.re)} "
                f"{q31_hex(row.a.im)} {q31_hex(row.b.re)} {q31_hex(row.b.im)}\n"
            )


# ---------------------------------------------------------------------------
# SECTION 6: independent NumPy diagnostic (not the bit-exact acceptance gate).
# ---------------------------------------------------------------------------
def fft_float(frame: InputFrame) -> "NDArray[np.complex128]":
    """Independent mathematical FFT, scaled by FDK's fixed FFT exponent."""

    # NumPy is optional for vector generation and bit-exact FDK equivalence.
    # Only the independent floating-point diagnostic below requires it.
    import numpy as np

    values = np.asarray(
        [complex(x.re / Q31_SCALE, x.im / Q31_SCALE) for x in frame.samples],
        dtype=np.complex128,
    )
    return np.asarray(np.fft.fft(values) / (1 << NFFT_TO_SCALE_EXP[frame.nfft]))


__all__ = [
    "ComplexQ31",
    "InputFrame",
    "OutputFrame",
    "StageRow",
    "PROFILE_ID",
    "OCTANT_SHA256",
    "MODE_TO_NFFT",
    "NFFT_TO_SCALE_EXP",
    "Q31_SCALE",
    "fft_fixed",
    "fft_float",
    "q31_hex",
    "read_input_frames",
    "read_output_frames",
    "write_output_frames",
    "write_stage_trace",
    "wrap32",
]
