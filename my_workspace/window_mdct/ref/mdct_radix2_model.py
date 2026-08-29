#!/usr/bin/env python3
"""Independent AAC-LC Window/MDCT reference model.

This module freezes the fixed-point profile used by the x86 build of FDK-AAC:

* PCM input is signed 16-bit.
* Transform data is signed Q31 stored in 32 bits.
* Window, DCT and FFT coefficients are signed Q15 stored in 16 bits.
* Arithmetic right shifts truncate; datapath operations wrap modulo 2**32.
* The complex FFT is a forward, negative-exponent radix-2 DIT transform.
* FFT-512 uses scale mask 101111111 (exponent 8).
* FFT-64 uses scale mask 101111 (exponent 5).

All bit-exact coefficient values are parsed from literal WTCP/STCP entries in
``libFDK/src/FDK_tools_rom.cpp``.  No trigonometric function is used to create
integer golden data.  NumPy trigonometric functions are used only by the
separate float64 mathematical oracle near the end of this file.
"""

from __future__ import annotations

import math
import random
import re
from dataclasses import dataclass
from enum import IntEnum
from functools import lru_cache
from pathlib import Path
from typing import Iterable, Sequence

try:
    import numpy as np
    from numpy.typing import NDArray
except ImportError:  # The integer model remains usable without NumPy.
    np = None  # type: ignore[assignment]
    NDArray = object  # type: ignore[assignment,misc]


FRAME_LENGTH = 1024
PCM_SNAPSHOT_LENGTH = 2 * FRAME_LENGTH
Q31_SCALE = 1 << 31
MASK16 = (1 << 16) - 1
MASK32 = (1 << 32) - 1
MIN_S16 = -(1 << 15)
MAX_S16 = (1 << 15) - 1
MIN_S32 = -(1 << 31)
MAX_S32 = (1 << 31) - 1

FOLD_EXP = 2
PRE_EXP = 4
FFT_SCALE_EXP = {64: 5, 512: 8}
FFT_SCALE_MASK = {64: "101111", 512: "101111111"}


class ModelError(ValueError):
    """Raised when a model input or the FDK ROM source is invalid."""


class BlockType(IntEnum):
    LONG = 0
    START = 1
    SHORT = 2
    STOP = 3


class WindowShape(IntEnum):
    SINE = 0
    KBD = 1


@dataclass(frozen=True)
class PairQ15:
    re: int
    im: int


@dataclass(frozen=True)
class ComplexQ31:
    re: int
    im: int


@dataclass(frozen=True)
class FoldParams:
    tl: int
    fl: int
    fr: int
    nl: int
    nr: int
    left_shape: WindowShape
    right_shape: WindowShape
    pcm_base: int
    output_base: int


@dataclass(frozen=True)
class FftStageTrace:
    """RAM contents after a DIT stage; stage 0 is after bit reversal."""

    stage: int
    scale_exp: int
    ram: tuple[ComplexQ31, ...]


@dataclass(frozen=True)
class FftTrace:
    nfft: int
    input_natural: tuple[ComplexQ31, ...]
    stages: tuple[FftStageTrace, ...]
    output_natural: tuple[ComplexQ31, ...]
    scale_exp: int


@dataclass(frozen=True)
class SubblockTrace:
    sub_id: int
    params: FoldParams
    fold: tuple[int, ...]
    pre: tuple[ComplexQ31, ...]
    fft: FftTrace
    post: tuple[int, ...]
    exp_fold: int
    exp_pre: int
    exp_fft_out: int
    exp_post: int


@dataclass(frozen=True)
class FrameTrace:
    frame_id: int
    block_type: BlockType
    window_shape: WindowShape
    pcm: tuple[int, ...]
    subblocks: tuple[SubblockTrace, ...]
    output: tuple[int, ...]
    exponent: int


@dataclass(frozen=True)
class ErrorMetrics:
    rel_rms: float
    rms_error: float
    max_abs_error: float
    snr_db: float


@dataclass(frozen=True)
class SubblockFloatMetrics:
    fft: ErrorMetrics
    mdct: ErrorMetrics


def to_s16(value: int) -> int:
    raw = int(value) & MASK16
    return raw - (1 << 16) if raw & (1 << 15) else raw


def wrap32(value: int) -> int:
    raw = int(value) & MASK32
    return raw - (1 << 32) if raw & (1 << 31) else raw


def add32(a: int, b: int) -> int:
    return wrap32(a + b)


def sub32(a: int, b: int) -> int:
    return wrap32(a - b)


def neg32(value: int) -> int:
    return wrap32(-value)


def asr32(value: int, shift: int) -> int:
    if shift < 0:
        raise ModelError("arithmetic right shift must be non-negative")
    return wrap32(value) >> shift


def q15_from_q31_literal(raw_literal: int) -> int:
    """Reproduce FX_DBL2FXCONST_SGL for one 32-bit ROM literal."""

    value = wrap32(raw_literal)
    shifted = (value >> 15) + 1
    if shifted > 0xFFFF and value > 0:
        return MAX_S16
    return to_s16(shifted >> 1)


def mul_div2_q31_q15(a: int, b: int) -> int:
    """FDK fMultDiv2(FIXP_DBL, FIXP_SGL): high product with no rounding."""

    if not MIN_S16 <= b <= MAX_S16:
        raise ModelError(f"Q15 coefficient is outside int16: {b}")
    return wrap32((wrap32(a) * int(b)) >> 16)


def mul_q31_q15(a: int, b: int) -> int:
    """FDK fMult: fMultDiv2 followed by a wrapping one-bit left shift."""

    return wrap32(mul_div2_q31_q15(a, b) << 1)


def cmul_div2_q31_q15(
    a_re: int, a_im: int, coeff: PairQ15
) -> tuple[int, int]:
    re_out = sub32(
        mul_div2_q31_q15(a_re, coeff.re),
        mul_div2_q31_q15(a_im, coeff.im),
    )
    im_out = add32(
        mul_div2_q31_q15(a_re, coeff.im),
        mul_div2_q31_q15(a_im, coeff.re),
    )
    return re_out, im_out


def cmul_q31_q15(a_re: int, a_im: int, coeff: PairQ15) -> tuple[int, int]:
    re_out = sub32(mul_q31_q15(a_re, coeff.re), mul_q31_q15(a_im, coeff.im))
    im_out = add32(mul_q31_q15(a_re, coeff.im), mul_q31_q15(a_im, coeff.re))
    return re_out, im_out


def hex_s16(value: int) -> str:
    return f"{int(value) & MASK16:04X}"


def hex_s32(value: int) -> str:
    return f"{int(value) & MASK32:08X}"


class FdkRom:
    """Q15 tables parsed from the checked-in FDK C++ ROM literals."""

    REQUIRED_WINDOWS = (
        "SineWindow1024",
        "KBDWindow1024",
        "SineWindow128",
        "KBDWindow128",
    )

    def __init__(self, source_path: Path | str | None = None) -> None:
        if source_path is None:
            repo_root = Path(__file__).resolve().parents[3]
            source_path = repo_root / "libFDK" / "src" / "FDK_tools_rom.cpp"
        self.source_path = Path(source_path).resolve()
        try:
            source = self.source_path.read_text(encoding="utf-8")
        except OSError as exc:
            raise ModelError(f"cannot read FDK ROM source {self.source_path}: {exc}") from exc

        self._windows: dict[tuple[int, WindowShape], tuple[PairQ15, ...]] = {
            (1024, WindowShape.SINE): self._parse_array(source, "SineWindow1024", "WTCP"),
            (1024, WindowShape.KBD): self._parse_array(source, "KBDWindow1024", "WTCP"),
            (128, WindowShape.SINE): self._parse_array(source, "SineWindow128", "WTCP"),
            (128, WindowShape.KBD): self._parse_array(source, "KBDWindow128", "WTCP"),
        }
        self.sine_table_512 = self._parse_array(source, "SineTable512", "STCP")
        self.sine_table_1024 = self._parse_array(source, "SineTable1024", "STCP")
        self.sqrt_half_q15 = q15_from_q31_literal(0x5A82799A)
        self._validate()

    @staticmethod
    def _parse_array(source: str, name: str, macro: str) -> tuple[PairQ15, ...]:
        array_pattern = re.compile(
            rf"const\s+FIXP_[A-Z]+\s+{re.escape(name)}\s*\[\s*\]\s*=\s*\{{(.*?)\}}\s*;",
            re.DOTALL,
        )
        match = array_pattern.search(source)
        if match is None:
            raise ModelError(f"cannot find literal array {name}")
        pair_pattern = re.compile(
            rf"{re.escape(macro)}\s*\(\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\)"
        )
        pairs = tuple(
            PairQ15(
                q15_from_q31_literal(int(re_hex, 16)),
                q15_from_q31_literal(int(im_hex, 16)),
            )
            for re_hex, im_hex in pair_pattern.findall(match.group(1))
        )
        if not pairs:
            raise ModelError(f"array {name} contains no {macro} literal pairs")
        return pairs

    def _validate(self) -> None:
        for (length, shape), values in self._windows.items():
            if len(values) != length // 2:
                raise ModelError(
                    f"{shape.name} window {length} has {len(values)} entries, expected {length // 2}"
                )
        if len(self.sine_table_512) != 257:
            raise ModelError(
                f"SineTable512 has {len(self.sine_table_512)} entries, expected 257"
            )
        if len(self.sine_table_1024) != 513:
            raise ModelError(
                f"SineTable1024 has {len(self.sine_table_1024)} entries, expected 513"
            )
        if self.sqrt_half_q15 != 0x5A82:
            raise ModelError(
                f"1/sqrt(2) conversion produced {hex_s16(self.sqrt_half_q15)}, expected 5A82"
            )

    def window(self, length: int, shape: WindowShape | int) -> tuple[PairQ15, ...]:
        try:
            key = (int(length), WindowShape(shape))
            return self._windows[key]
        except (KeyError, ValueError) as exc:
            raise ModelError(f"unsupported window length/shape: {length}/{shape}") from exc

    @property
    def windows(self) -> dict[tuple[int, WindowShape], tuple[PairQ15, ...]]:
        return dict(self._windows)


def fold_q31(
    pcm_2tl: Sequence[int], params: FoldParams, left: Sequence[PairQ15], right: Sequence[PairQ15]
) -> tuple[int, ...]:
    """Bit-exact translation of the four folding loops in mdct_block()."""

    tl, fl, fr, nl, nr = params.tl, params.fl, params.fr, params.nl, params.nr
    if len(pcm_2tl) != 2 * tl:
        raise ModelError(f"fold needs {2 * tl} PCM samples, got {len(pcm_2tl)}")
    if len(left) < fl // 2 or len(right) < fr // 2:
        raise ModelError("window slope table is shorter than fold parameters")
    pcm = [int(value) for value in pcm_2tl]
    if any(value < MIN_S16 or value > MAX_S16 for value in pcm):
        raise ModelError("PCM sample is outside signed 16-bit range")

    result: list[int | None] = [None] * tl

    for i in range(nl):
        result[tl // 2 + i] = neg32(pcm[tl - i - 1] << 15)

    for i in range(fl // 2):
        tmp0 = pcm[i + nl] * left[i].im
        result[tl // 2 + i + nl] = sub32(
            tmp0, pcm[tl - nl - i - 1] * left[i].re
        )

    for i in range(nr):
        result[tl // 2 - 1 - i] = neg32(pcm[tl + i] << 15)

    for i in range(fr // 2):
        tmp1 = pcm[tl + nr + i] * right[i].re
        summed = add32(tmp1, pcm[2 * tl - nr - i - 1] * right[i].im)
        result[tl // 2 - nr - i - 1] = neg32(summed)

    if any(value is None for value in result):
        missing = [index for index, value in enumerate(result) if value is None]
        raise AssertionError(f"fold did not assign output indices {missing[:8]}")
    return tuple(int(value) for value in result)


def dct4_pre_q31(folded: Sequence[int], sine_window: Sequence[PairQ15]) -> tuple[ComplexQ31, ...]:
    """Translate the pre-twiddle/in-place packing part of FDK dct_IV()."""

    length = len(folded)
    if length not in (128, 1024):
        raise ModelError(f"unsupported DCT-IV length: {length}")
    half = length // 2
    if len(sine_window) != half:
        raise ModelError(f"pre-twiddle needs {half} sine entries, got {len(sine_window)}")

    data = [wrap32(value) for value in folded]
    p0 = 0
    p1 = length - 2
    for i in range(0, half - 1, 2):
        accu1 = data[p1 + 1]
        accu2 = data[p0]
        accu3 = data[p0 + 1]
        accu4 = data[p1]

        accu1, accu2 = cmul_div2_q31_q15(accu1, accu2, sine_window[i])
        accu3, accu4 = cmul_div2_q31_q15(accu4, accu3, sine_window[i + 1])

        data[p0] = asr32(accu2, 1)
        data[p0 + 1] = asr32(accu1, 1)
        data[p1] = asr32(accu4, 1)
        data[p1 + 1] = neg32(asr32(accu3, 1))
        p0 += 2
        p1 -= 2

    return tuple(ComplexQ31(data[2 * i], data[2 * i + 1]) for i in range(half))


def _bit_reverse(value: int, bits: int) -> int:
    result = 0
    for _ in range(bits):
        result = (result << 1) | (value & 1)
        value >>= 1
    return result


def _complex_to_interleaved(values: Sequence[ComplexQ31]) -> list[int]:
    result: list[int] = []
    for value in values:
        result.extend((wrap32(value.re), wrap32(value.im)))
    return result


def _interleaved_to_complex(values: Sequence[int]) -> tuple[ComplexQ31, ...]:
    if len(values) & 1:
        raise AssertionError("interleaved complex vector has odd length")
    return tuple(
        ComplexQ31(wrap32(values[index]), wrap32(values[index + 1]))
        for index in range(0, len(values), 2)
    )


def _bit_reverse_ram(values: Sequence[ComplexQ31]) -> list[int]:
    nfft = len(values)
    ldn = nfft.bit_length() - 1
    if (1 << ldn) != nfft:
        raise ModelError("FFT length must be a power of two")
    result = [0] * (2 * nfft)
    for index, value in enumerate(values):
        address = _bit_reverse(index, ldn)
        result[2 * address] = wrap32(value.re)
        result[2 * address + 1] = wrap32(value.im)
    return result


def _radix2_stage1_scaled(data: list[int]) -> None:
    """First DIT stage, split exactly from FDK's fused radix-4 kernel."""

    nfft = len(data) // 2
    for base in range(0, nfft, 2):
        ia = 2 * base
        ib = ia + 2
        b_re = data[ib]
        b_im = data[ib + 1]
        top_re = asr32(add32(data[ia], b_re), 1)
        top_im = asr32(add32(data[ia + 1], b_im), 1)
        # FDK derives the lower branch as ((a+b)>>1)-b.  Preserve that exact
        # ordering instead of replacing it with (a-b)>>1.
        bot_re = sub32(top_re, b_re)
        bot_im = sub32(top_im, b_im)
        data[ia], data[ia + 1] = top_re, top_im
        data[ib], data[ib + 1] = bot_re, bot_im


def _radix2_stage2_unscaled(data: list[int]) -> None:
    """Second DIT stage, W=1 and W=-j, with no additional scaling."""

    nfft = len(data) // 2
    for base in range(0, nfft, 4):
        i0, i1, i2, i3 = (2 * (base + offset) for offset in range(4))
        a_re, a_im = data[i0], data[i0 + 1]
        b_re, b_im = data[i1], data[i1 + 1]
        c_re, c_im = data[i2], data[i2 + 1]
        d_re, d_im = data[i3], data[i3 + 1]

        data[i0], data[i0 + 1] = add32(a_re, c_re), add32(a_im, c_im)
        data[i2], data[i2 + 1] = sub32(a_re, c_re), sub32(a_im, c_im)

        # b + (-j)d and b - (-j)d.
        data[i1], data[i1 + 1] = add32(b_re, d_im), sub32(b_im, d_re)
        data[i3], data[i3 + 1] = sub32(b_re, d_im), add32(b_im, d_re)


def _radix2_stage_fdk(data: list[int], ldm: int, rom: FdkRom) -> None:
    """One ldm>=3 radix-2 stage, preserving fft_rad2.cpp operation order."""

    nfft = len(data) // 2
    m = 1 << ldm
    mh = m >> 1
    trigstep = (512 << 2) >> ldm

    # j=0 and, in the second half, the exact W=-j axis butterflies.
    for r in range(0, nfft, m):
        t1 = 2 * r
        t2 = t1 + 2 * mh
        vi = asr32(data[t2 + 1], 1)
        vr = asr32(data[t2], 1)
        ur = asr32(data[t1], 1)
        ui = asr32(data[t1 + 1], 1)
        data[t1], data[t1 + 1] = add32(ur, vr), add32(ui, vi)
        data[t2], data[t2 + 1] = sub32(ur, vr), sub32(ui, vi)

        t1 += mh
        t2 = t1 + 2 * mh
        vr = asr32(data[t2 + 1], 1)
        vi = asr32(data[t2], 1)
        ur = asr32(data[t1], 1)
        ui = asr32(data[t1 + 1], 1)
        data[t1], data[t1 + 1] = add32(ur, vr), sub32(ui, vi)
        data[t2], data[t2 + 1] = sub32(ur, vr), add32(ui, vi)

    for j in range(1, mh // 4):
        coeff = rom.sine_table_512[j * trigstep]
        for r in range(0, nfft, m):
            t1 = 2 * (r + j)
            t2 = t1 + 2 * mh
            vi, vr = cmul_div2_q31_q15(data[t2 + 1], data[t2], coeff)
            ur, ui = asr32(data[t1], 1), asr32(data[t1 + 1], 1)
            data[t1], data[t1 + 1] = add32(ur, vr), add32(ui, vi)
            data[t2], data[t2 + 1] = sub32(ur, vr), sub32(ui, vi)

            t1 += mh
            t2 = t1 + 2 * mh
            vr, vi = cmul_div2_q31_q15(data[t2 + 1], data[t2], coeff)
            ur, ui = asr32(data[t1], 1), asr32(data[t1 + 1], 1)
            data[t1], data[t1 + 1] = add32(ur, vr), sub32(ui, vi)
            data[t2], data[t2 + 1] = sub32(ur, vr), add32(ui, vi)

            t1 = 2 * (r + mh // 2 - j)
            t2 = t1 + 2 * mh
            vi, vr = cmul_div2_q31_q15(data[t2], data[t2 + 1], coeff)
            ur, ui = asr32(data[t1], 1), asr32(data[t1 + 1], 1)
            data[t1], data[t1 + 1] = add32(ur, vr), sub32(ui, vi)
            data[t2], data[t2 + 1] = sub32(ur, vr), add32(ui, vi)

            t1 += mh
            t2 = t1 + 2 * mh
            vr, vi = cmul_div2_q31_q15(data[t2], data[t2 + 1], coeff)
            ur, ui = asr32(data[t1], 1), asr32(data[t1 + 1], 1)
            data[t1], data[t1 + 1] = sub32(ur, vr), sub32(ui, vi)
            data[t2], data[t2 + 1] = add32(ur, vr), add32(ui, vi)

    # pi/4 and 3*pi/4 use the frozen Q15 1/sqrt(2) literal.
    coeff = PairQ15(rom.sqrt_half_q15, rom.sqrt_half_q15)
    j = mh // 4
    for r in range(0, nfft, m):
        t1 = 2 * (r + j)
        t2 = t1 + 2 * mh
        vi, vr = cmul_div2_q31_q15(data[t2 + 1], data[t2], coeff)
        ur, ui = asr32(data[t1], 1), asr32(data[t1 + 1], 1)
        data[t1], data[t1 + 1] = add32(ur, vr), add32(ui, vi)
        data[t2], data[t2 + 1] = sub32(ur, vr), sub32(ui, vi)

        t1 += mh
        t2 = t1 + 2 * mh
        vr, vi = cmul_div2_q31_q15(data[t2 + 1], data[t2], coeff)
        ur, ui = asr32(data[t1], 1), asr32(data[t1 + 1], 1)
        data[t1], data[t1 + 1] = add32(ur, vr), sub32(ui, vi)
        data[t2], data[t2 + 1] = sub32(ur, vr), add32(ui, vi)


def _fused_first_two_fdk(data: list[int]) -> None:
    """Original fused FDK kernel, used only to verify the radix-2 split."""

    nfft = len(data) // 2
    for base in range(0, nfft, 4):
        i = 2 * base
        original = data[i : i + 8]
        a00 = asr32(add32(original[0], original[2]), 1)
        a10 = asr32(add32(original[4], original[6]), 1)
        a20 = asr32(add32(original[1], original[3]), 1)
        a30 = asr32(add32(original[5], original[7]), 1)

        data[i + 0] = add32(a00, a10)
        data[i + 4] = sub32(a00, a10)
        data[i + 1] = add32(a20, a30)
        data[i + 5] = sub32(a20, a30)

        a00 = sub32(a00, original[2])
        a10 = sub32(a10, original[6])
        a20 = sub32(a20, original[3])
        a30 = sub32(a30, original[7])

        data[i + 2] = add32(a00, a30)
        data[i + 6] = sub32(a00, a30)
        data[i + 3] = sub32(a20, a10)
        data[i + 7] = add32(a20, a10)


def fft_radix2_q31(values: Sequence[ComplexQ31], rom: FdkRom) -> FftTrace:
    """Pure radix-2 FFT with FDK-Q15 twiddles and FDK scale schedule."""

    nfft = len(values)
    if nfft not in (64, 512):
        raise ModelError(f"FFT supports only 64 or 512 points, got {nfft}")
    ldn = nfft.bit_length() - 1
    data = _bit_reverse_ram(values)
    stages: list[FftStageTrace] = [
        FftStageTrace(0, 0, _interleaved_to_complex(data))
    ]

    _radix2_stage1_scaled(data)
    stages.append(FftStageTrace(1, 1, _interleaved_to_complex(data)))

    _radix2_stage2_unscaled(data)
    stages.append(FftStageTrace(2, 1, _interleaved_to_complex(data)))

    for stage in range(3, ldn + 1):
        _radix2_stage_fdk(data, stage, rom)
        stages.append(FftStageTrace(stage, stage - 1, _interleaved_to_complex(data)))

    output = _interleaved_to_complex(data)
    return FftTrace(
        nfft=nfft,
        input_natural=tuple(values),
        stages=tuple(stages),
        output_natural=output,
        scale_exp=ldn - 1,
    )


def _fft_fused_fdk_q31(values: Sequence[ComplexQ31], rom: FdkRom) -> tuple[ComplexQ31, ...]:
    data = _bit_reverse_ram(values)
    _fused_first_two_fdk(data)
    ldn = len(values).bit_length() - 1
    for stage in range(3, ldn + 1):
        _radix2_stage_fdk(data, stage, rom)
    return _interleaved_to_complex(data)


def dct4_post_q31(fft_output: Sequence[ComplexQ31], length: int, rom: FdkRom) -> tuple[int, ...]:
    """Translate the post-twiddle/unpacking part of FDK dct_IV()."""

    if length not in (128, 1024) or len(fft_output) != length // 2:
        raise ModelError("DCT post size mismatch")
    half = length // 2
    sin_step = 2 if length == 1024 else 16
    data = _complex_to_interleaved(fft_output)
    p0 = 0
    p1 = length - 2

    accu1 = data[p1]
    accu2 = data[p1 + 1]
    data[p1 + 1] = neg32(data[p0 + 1])

    idx = sin_step
    for _i in range(1, (half + 1) >> 1):
        coeff = rom.sine_table_1024[idx]
        accu3, accu4 = cmul_q31_q15(accu1, accu2, coeff)
        data[p0 + 1] = accu3
        data[p1] = accu4

        p0 += 2
        p1 -= 2

        accu3, accu4 = cmul_q31_q15(data[p0 + 1], data[p0], coeff)
        accu1 = data[p1]
        accu2 = data[p1 + 1]
        data[p1 + 1] = neg32(accu3)
        data[p0] = accu4
        idx += sin_step

    if (half & 1) == 0:
        accu1 = mul_q31_q15(accu1, rom.sqrt_half_q15)
        accu2 = mul_q31_q15(accu2, rom.sqrt_half_q15)
        data[p1] = add32(accu1, accu2)
        data[p0 + 1] = sub32(accu1, accu2)

    return tuple(wrap32(value) for value in data)


class MdctRadix2Model:
    """Stateful AAC-LC analysis Window/MDCT model."""

    def __init__(self, rom: FdkRom | None = None) -> None:
        self.rom = rom if rom is not None else FdkRom()
        self.reset()

    def reset(self) -> None:
        self.prev_fr = 0
        self.prev_shape = WindowShape.SINE
        self.prev_tl = 0

    @staticmethod
    def _block_parameters(block_type: BlockType) -> tuple[int, int, int]:
        if block_type == BlockType.SHORT:
            return 8, 128, 128
        if block_type in (BlockType.LONG, BlockType.STOP):
            return 1, 1024, 1024
        if block_type == BlockType.START:
            return 1, 1024, 128
        raise ModelError(f"unsupported AAC-LC block type: {block_type}")

    def process_frame(
        self,
        pcm_snapshot: Sequence[int],
        block_type: BlockType | int,
        window_shape: WindowShape | int,
        frame_id: int = 0,
    ) -> FrameTrace:
        try:
            block = BlockType(block_type)
            shape = WindowShape(window_shape)
        except ValueError as exc:
            raise ModelError("invalid block type or window shape") from exc
        if len(pcm_snapshot) != PCM_SNAPSHOT_LENGTH:
            raise ModelError(
                f"AAC-LC MDCT snapshot must contain {PCM_SNAPSHOT_LENGTH} samples"
            )
        pcm = tuple(int(value) for value in pcm_snapshot)
        if any(value < MIN_S16 or value > MAX_S16 for value in pcm):
            raise ModelError("PCM snapshot contains a value outside signed 16-bit range")

        n_spec, tl, fr = self._block_parameters(block)
        right = self.rom.window(fr, shape)
        time_base = (FRAME_LENGTH - tl) // 2
        subblocks: list[SubblockTrace] = []
        frame_output: list[int] = []

        for sub_id in range(n_spec):
            if self.prev_fr == 0:
                self.prev_fr = fr
                self.prev_shape = shape
                self.prev_tl = tl

            fl = self.prev_fr
            left_shape = self.prev_shape
            nl = (tl - fl) // 2
            nr = (tl - fr) // 2
            if nl < 0 or nr < 0:
                raise ModelError(
                    f"illegal transition produces negative slope offset: tl={tl}, fl={fl}, fr={fr}"
                )
            pcm_base = time_base + sub_id * tl
            output_base = sub_id * tl
            params = FoldParams(
                tl=tl,
                fl=fl,
                fr=fr,
                nl=nl,
                nr=nr,
                left_shape=left_shape,
                right_shape=shape,
                pcm_base=pcm_base,
                output_base=output_base,
            )

            left = self.rom.window(fl, left_shape)
            folded = fold_q31(pcm[pcm_base : pcm_base + 2 * tl], params, left, right)
            pre = dct4_pre_q31(folded, self.rom.window(tl, WindowShape.SINE))
            fft_trace = fft_radix2_q31(pre, self.rom)
            post = dct4_post_q31(fft_trace.output_natural, tl, self.rom)
            fft_out_exp = PRE_EXP + fft_trace.scale_exp

            subblocks.append(
                SubblockTrace(
                    sub_id=sub_id,
                    params=params,
                    fold=folded,
                    pre=pre,
                    fft=fft_trace,
                    post=post,
                    exp_fold=FOLD_EXP,
                    exp_pre=PRE_EXP,
                    exp_fft_out=fft_out_exp,
                    exp_post=fft_out_exp,
                )
            )
            frame_output.extend(post)

            self.prev_fr = fr
            self.prev_shape = shape
            self.prev_tl = tl

        expected_exp = 9 if block == BlockType.SHORT else 12
        if len(frame_output) != FRAME_LENGTH:
            raise AssertionError(
                f"frame output has {len(frame_output)} coefficients, expected {FRAME_LENGTH}"
            )
        if any(sub.exp_post != expected_exp for sub in subblocks):
            raise AssertionError("subblock exponent does not match AAC-LC budget")

        return FrameTrace(
            frame_id=frame_id,
            block_type=block,
            window_shape=shape,
            pcm=pcm,
            subblocks=tuple(subblocks),
            output=tuple(frame_output),
            exponent=expected_exp,
        )


def _require_numpy() -> None:
    if np is None:
        raise ModelError("NumPy is required for the float64 oracle")


@lru_cache(maxsize=2)
def _dct4_kernel(length: int):
    _require_numpy()
    if length not in (128, 1024):
        raise ModelError(f"unsupported DCT-IV oracle length: {length}")
    n = np.arange(length, dtype=np.float64)[:, None]
    k = np.arange(length, dtype=np.float64)[None, :]
    return np.cos((np.pi / length) * (n + 0.5) * (k + 0.5))


def dct4_float64(values: Sequence[float]):
    """Independent O(N**2) float64 DCT-IV oracle."""

    _require_numpy()
    vector = np.asarray(values, dtype=np.float64)
    if vector.ndim != 1:
        raise ModelError("DCT-IV oracle expects a one-dimensional vector")
    return vector @ _dct4_kernel(int(vector.size))


def _error_metrics(actual, reference) -> ErrorMetrics:
    _require_numpy()
    actual_array = np.asarray(actual)
    reference_array = np.asarray(reference)
    error = actual_array - reference_array
    error_power = float(np.mean(np.abs(error) ** 2, dtype=np.float64))
    reference_power = float(np.mean(np.abs(reference_array) ** 2, dtype=np.float64))
    rms_error = math.sqrt(error_power)
    reference_rms = math.sqrt(reference_power)
    rel_rms = rms_error / reference_rms if reference_rms > 0.0 else rms_error
    if error_power == 0.0:
        snr_db = math.inf
    elif reference_power == 0.0:
        snr_db = -math.inf
    else:
        snr_db = 10.0 * math.log10(reference_power / error_power)
    max_abs_error = float(np.max(np.abs(error))) if error.size else 0.0
    return ErrorMetrics(rel_rms, rms_error, max_abs_error, snr_db)


def float_metrics(subblock: SubblockTrace) -> SubblockFloatMetrics:
    """Compare fixed FFT/MDCT values with independent NumPy float64 math."""

    _require_numpy()
    pre = np.asarray(
        [complex(value.re, value.im) for value in subblock.pre], dtype=np.complex128
    )
    pre_physical = pre * (2.0**subblock.exp_pre / Q31_SCALE)
    fft_reference = np.fft.fft(pre_physical)
    fft_fixed = np.asarray(
        [complex(value.re, value.im) for value in subblock.fft.output_natural],
        dtype=np.complex128,
    ) * (2.0**subblock.exp_fft_out / Q31_SCALE)

    folded_physical = np.asarray(subblock.fold, dtype=np.float64) * (
        2.0**subblock.exp_fold / Q31_SCALE
    )
    mdct_reference = dct4_float64(folded_physical)
    mdct_fixed = np.asarray(subblock.post, dtype=np.float64) * (
        2.0**subblock.exp_post / Q31_SCALE
    )
    return SubblockFloatMetrics(
        fft=_error_metrics(fft_fixed, fft_reference),
        mdct=_error_metrics(mdct_fixed, mdct_reference),
    )


def run_self_test(rom_source: Path | str | None = None) -> dict[str, float | int]:
    """Run deterministic arithmetic, FFT-split, state and float-oracle tests."""

    rom = FdkRom(rom_source)
    if q15_from_q31_literal(0x7FFFFFFF) != 0x7FFF:
        raise AssertionError("Q31-to-Q15 positive saturation failed")
    if mul_div2_q31_q15(-5 << 20, 0x4000) != ((-5 << 20) * 0x4000) >> 16:
        raise AssertionError("Q31/Q15 truncating multiply failed")

    rng = random.Random(0x4D444354)
    fft_cases = 0
    max_fft_rel = 0.0
    for nfft in (64, 512):
        for _case in range(3):
            values = tuple(
                ComplexQ31(rng.randrange(-(1 << 27), 1 << 27), rng.randrange(-(1 << 27), 1 << 27))
                for _ in range(nfft)
            )
            split = fft_radix2_q31(values, rom)
            fused = _fft_fused_fdk_q31(values, rom)
            if split.output_natural != fused:
                raise AssertionError(f"pure radix-2 split differs from fused FDK FFT-{nfft}")
            if split.scale_exp != FFT_SCALE_EXP[nfft]:
                raise AssertionError("FFT scale exponent mismatch")
            if np is not None:
                input_float = np.asarray(
                    [complex(value.re, value.im) for value in values], dtype=np.complex128
                ) / Q31_SCALE
                fixed_float = np.asarray(
                    [complex(value.re, value.im) for value in split.output_natural],
                    dtype=np.complex128,
                ) * (2.0**split.scale_exp / Q31_SCALE)
                metric = _error_metrics(fixed_float, np.fft.fft(input_float))
                max_fft_rel = max(max_fft_rel, metric.rel_rms)
            fft_cases += 1

    model = MdctRadix2Model(rom)
    blocks = (
        BlockType.LONG,
        BlockType.LONG,
        BlockType.START,
        BlockType.SHORT,
        BlockType.STOP,
        BlockType.LONG,
    )
    shapes = (
        WindowShape.SINE,
        WindowShape.KBD,
        WindowShape.SINE,
        WindowShape.KBD,
        WindowShape.SINE,
        WindowShape.KBD,
    )
    frames: list[FrameTrace] = []
    for frame_id, (block, shape) in enumerate(zip(blocks, shapes)):
        pcm = [rng.randrange(MIN_S16, MAX_S16 + 1) for _ in range(PCM_SNAPSHOT_LENGTH)]
        frames.append(model.process_frame(pcm, block, shape, frame_id))

    expected_fl = (1024, 1024, 1024, 128, 128, 1024)
    for frame, fl in zip(frames, expected_fl):
        if frame.subblocks[0].params.fl != fl:
            raise AssertionError(
                f"left-slope state mismatch at frame {frame.frame_id}: "
                f"{frame.subblocks[0].params.fl} != {fl}"
            )
        if len(frame.output) != FRAME_LENGTH:
            raise AssertionError("MDCT frame did not produce 1024 coefficients")
    if len(frames[3].subblocks) != 8 or frames[3].exponent != 9:
        raise AssertionError("SHORT frame structure/exponent mismatch")
    if any(frame.exponent != 12 for index, frame in enumerate(frames) if index != 3):
        raise AssertionError("long-family exponent mismatch")

    max_mdct_rel = 0.0
    if np is not None:
        # One long and one short sub-transform are sufficient to validate the
        # independent formula without making the self-test unnecessarily slow.
        for sub in (frames[1].subblocks[0], frames[3].subblocks[0]):
            metrics = float_metrics(sub)
            max_fft_rel = max(max_fft_rel, metrics.fft.rel_rms)
            max_mdct_rel = max(max_mdct_rel, metrics.mdct.rel_rms)
        if not math.isfinite(max_fft_rel) or max_fft_rel > 5e-3:
            raise AssertionError(f"FFT float64 relative RMS too large: {max_fft_rel}")
        if not math.isfinite(max_mdct_rel) or max_mdct_rel > 5e-3:
            raise AssertionError(f"MDCT float64 relative RMS too large: {max_mdct_rel}")

    zero_model = MdctRadix2Model(rom)
    zero_frame = zero_model.process_frame(
        [0] * PCM_SNAPSHOT_LENGTH, BlockType.LONG, WindowShape.SINE
    )
    if any(zero_frame.output):
        raise AssertionError("zero input did not produce zero output")

    return {
        "fft_cases": fft_cases,
        "frames": len(frames),
        "max_fft_rel_rms": max_fft_rel,
        "max_mdct_rel_rms": max_mdct_rel,
    }


__all__ = [
    "BlockType",
    "ComplexQ31",
    "ErrorMetrics",
    "FOLD_EXP",
    "FRAME_LENGTH",
    "FFT_SCALE_EXP",
    "FFT_SCALE_MASK",
    "FdkRom",
    "FoldParams",
    "FrameTrace",
    "MdctRadix2Model",
    "ModelError",
    "PCM_SNAPSHOT_LENGTH",
    "PRE_EXP",
    "PairQ15",
    "SubblockFloatMetrics",
    "SubblockTrace",
    "WindowShape",
    "dct4_float64",
    "dct4_post_q31",
    "dct4_pre_q31",
    "fft_radix2_q31",
    "float_metrics",
    "fold_q31",
    "hex_s16",
    "hex_s32",
    "run_self_test",
    "wrap32",
]


if __name__ == "__main__":
    summary = run_self_test()
    print("PASS mdct_radix2_model self-test")
    for key, value in summary.items():
        print(f"{key}={value}")
