#!/usr/bin/env python3
"""Prove the pure radix-2 model equals FDK's fused radix-4/radix-2 kernel.

The second implementation below is a direct, statement-ordered Python port of
``libFDK/src/fft_rad2.cpp::dit_fft`` for lengths 64 and 512.  Comparing it with
``ref/fft_radix2_model.py`` protects the deliberate architectural change
(pure radix-2 + ping-pong memory) from changing FDK fixed-point results.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path


CORE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CORE_ROOT / "ref"))

from fdk_sinetable512_q15 import OCTANT_Q15  # noqa: E402
from fft_radix2_model import (  # noqa: E402
    ComplexQ31,
    InputFrame,
    fft_fixed,
    mul_div2_q31_q15,
    wrap32,
)


def cplx_mul_div2(a_re: int, a_im: int, c: int, s: int) -> tuple[int, int]:
    return (
        wrap32(mul_div2_q31_q15(a_re, c) - mul_div2_q31_q15(a_im, s)),
        wrap32(mul_div2_q31_q15(a_re, s) + mul_div2_q31_q15(a_im, c)),
    )


def bit_reverse(value: int, bits: int) -> int:
    result = 0
    for _ in range(bits):
        result = (result << 1) | (value & 1)
        value >>= 1
    return result


def fdk_dit_fft(
    samples: tuple[ComplexQ31, ...],
    snapshots: list[tuple[ComplexQ31, ...]] | None = None,
) -> tuple[ComplexQ31, ...]:
    n = len(samples)
    ldn = 6 if n == 64 else 9
    x = [0] * (2 * n)
    for index, value in enumerate(samples):
        reversed_index = bit_reverse(index, ldn)
        x[2 * reversed_index] = value.re
        x[2 * reversed_index + 1] = value.im

    # FDK lines 143..163: fused stages 1+2 with one total right shift.
    for i in range(0, 2 * n, 8):
        a00 = (x[i] + x[i + 2]) >> 1
        a10 = (x[i + 4] + x[i + 6]) >> 1
        a20 = (x[i + 1] + x[i + 3]) >> 1
        a30 = (x[i + 5] + x[i + 7]) >> 1

        x[i] = wrap32(a00 + a10)
        x[i + 4] = wrap32(a00 - a10)
        x[i + 1] = wrap32(a20 + a30)
        x[i + 5] = wrap32(a20 - a30)

        a00 = wrap32(a00 - x[i + 2])
        a10 = wrap32(a10 - x[i + 6])
        a20 = wrap32(a20 - x[i + 3])
        a30 = wrap32(a30 - x[i + 7])

        x[i + 2] = wrap32(a00 + a30)
        x[i + 6] = wrap32(a00 - a30)
        x[i + 3] = wrap32(a20 - a10)
        x[i + 7] = wrap32(a20 + a10)

    if snapshots is not None:
        snapshots.append(tuple(ComplexQ31(x[2 * i], x[2 * i + 1]) for i in range(n)))

    for ldm in range(3, ldn + 1):
        m = 1 << ldm
        mh = m >> 1
        trig_step = (512 << 2) >> ldm

        # FDK block 1: j=0 and the corresponding -j positions.
        for r in range(0, n, m):
            t1 = 2 * r
            t2 = t1 + 2 * mh
            vi = x[t2 + 1] >> 1
            vr = x[t2] >> 1
            ur = x[t1] >> 1
            ui = x[t1 + 1] >> 1
            x[t1], x[t1 + 1] = wrap32(ur + vr), wrap32(ui + vi)
            x[t2], x[t2 + 1] = wrap32(ur - vr), wrap32(ui - vi)

            t1 += mh
            t2 = t1 + 2 * mh
            vr = x[t2 + 1] >> 1
            vi = x[t2] >> 1
            ur = x[t1] >> 1
            ui = x[t1 + 1] >> 1
            x[t1], x[t1 + 1] = wrap32(ur + vr), wrap32(ui - vi)
            x[t2], x[t2 + 1] = wrap32(ur - vr), wrap32(ui + vi)

        for j in range(1, mh // 4):
            c, s = OCTANT_Q15[(j * trig_step) // 4]
            for r in range(0, n, m):
                t1 = 2 * (r + j)
                t2 = t1 + 2 * mh
                vi, vr = cplx_mul_div2(x[t2 + 1], x[t2], c, s)
                ur, ui = x[t1] >> 1, x[t1 + 1] >> 1
                x[t1], x[t1 + 1] = wrap32(ur + vr), wrap32(ui + vi)
                x[t2], x[t2 + 1] = wrap32(ur - vr), wrap32(ui - vi)

                t1 += mh
                t2 = t1 + 2 * mh
                vr, vi = cplx_mul_div2(x[t2 + 1], x[t2], c, s)
                ur, ui = x[t1] >> 1, x[t1 + 1] >> 1
                x[t1], x[t1 + 1] = wrap32(ur + vr), wrap32(ui - vi)
                x[t2], x[t2 + 1] = wrap32(ur - vr), wrap32(ui + vi)

                t1 = 2 * (r + mh // 2 - j)
                t2 = t1 + 2 * mh
                vi, vr = cplx_mul_div2(x[t2], x[t2 + 1], c, s)
                ur, ui = x[t1] >> 1, x[t1 + 1] >> 1
                x[t1], x[t1 + 1] = wrap32(ur + vr), wrap32(ui - vi)
                x[t2], x[t2 + 1] = wrap32(ur - vr), wrap32(ui + vi)

                t1 += mh
                t2 = t1 + 2 * mh
                vr, vi = cplx_mul_div2(x[t2], x[t2 + 1], c, s)
                ur, ui = x[t1] >> 1, x[t1 + 1] >> 1
                x[t1], x[t1 + 1] = wrap32(ur - vr), wrap32(ui - vi)
                x[t2], x[t2 + 1] = wrap32(ur + vr), wrap32(ui + vi)

        # FDK block 2: exact pi/4 coefficient.
        c, s = OCTANT_Q15[64]
        j = mh // 4
        for r in range(0, n, m):
            t1 = 2 * (r + j)
            t2 = t1 + 2 * mh
            vi, vr = cplx_mul_div2(x[t2 + 1], x[t2], c, s)
            ur, ui = x[t1] >> 1, x[t1 + 1] >> 1
            x[t1], x[t1 + 1] = wrap32(ur + vr), wrap32(ui + vi)
            x[t2], x[t2 + 1] = wrap32(ur - vr), wrap32(ui - vi)

            t1 += mh
            t2 = t1 + 2 * mh
            vr, vi = cplx_mul_div2(x[t2 + 1], x[t2], c, s)
            ur, ui = x[t1] >> 1, x[t1 + 1] >> 1
            x[t1], x[t1 + 1] = wrap32(ur + vr), wrap32(ui - vi)
            x[t2], x[t2 + 1] = wrap32(ur - vr), wrap32(ui + vi)

        if snapshots is not None:
            snapshots.append(
                tuple(ComplexQ31(x[2 * i], x[2 * i + 1]) for i in range(n))
            )

    return tuple(ComplexQ31(x[2 * i], x[2 * i + 1]) for i in range(n))


def verify(trials: int) -> None:
    generator = random.Random(0xF0D2AAC)
    checked_bins = 0
    for nfft in (64, 512):
        mode = 0 if nfft == 64 else 1
        for trial in range(trials):
            # /4 headroom is the actual DCT-IV pre-twiddle contract.
            samples = tuple(
                ComplexQ31(
                    generator.randint(-(1 << 29), (1 << 29) - 1),
                    generator.randint(-(1 << 29), (1 << 29) - 1),
                )
                for _ in range(nfft)
            )
            pure_radix2, pure_trace = fft_fixed(InputFrame(mode, trial, samples))
            fdk_snapshots: list[tuple[ComplexQ31, ...]] = []
            fdk = fdk_dit_fft(samples, fdk_snapshots)
            if pure_radix2.samples != fdk:
                pure_stage = [ComplexQ31(0, 0) for _ in range(nfft)]
                trace_offset = 0
                first_bad_stage = -1
                first_bad_stage_index = -1
                first_bad_stage_values = None
                for stage in range(1, (6 if nfft == 64 else 9) + 1):
                    for row in pure_trace[trace_offset : trace_offset + nfft // 2]:
                        pure_stage[row.addr_a] = row.a
                        pure_stage[row.addr_b] = row.b
                    trace_offset += nfft // 2
                    if stage >= 2 and tuple(pure_stage) != fdk_snapshots[stage - 2]:
                        first_bad_stage = stage
                        for stage_index, (lhs_stage, rhs_stage) in enumerate(
                            zip(pure_stage, fdk_snapshots[stage - 2])
                        ):
                            if lhs_stage != rhs_stage:
                                first_bad_stage_index = stage_index
                                first_bad_stage_values = (lhs_stage, rhs_stage)
                                break
                        break
                for index, (lhs, rhs) in enumerate(zip(pure_radix2.samples, fdk)):
                    if lhs != rhs:
                        raise AssertionError(
                            f"N={nfft} trial={trial} first_bad_stage={first_bad_stage} "
                            f"stage_index={first_bad_stage_index} "
                            f"stage_values={first_bad_stage_values} "
                            f"bin={index}: "
                            f"radix2={lhs}, fdk={rhs}"
                        )
            checked_bins += nfft
    print(f"PASS: pure radix-2 equals direct FDK dit_fft port ({checked_bins} bins)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trials", type=int, default=32)
    args = parser.parse_args()
    if args.trials < 1:
        parser.error("--trials must be positive")
    verify(args.trials)


if __name__ == "__main__":
    main()
