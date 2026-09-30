"""Generates the oracle vectors for lib/support/tests/test_float_math.cc.

Every result is computed with mpmath (1.3.0) at well over a thousand bits and rounded once, to
nearest with ties to even, into each float format. Inputs and results are written as binary128
bit patterns.

    python tools/gen_float_math_vectors.py > lib/support/tests/data/float_math_vectors.inc
"""

import random
import sys

import mpmath
from mpmath import mp, mpf

# name: (precision, min_exponent, max_exponent)
FORMATS = {
    "HALF": (11, -14, 15),
    "SINGLE": (24, -126, 127),
    "DOUBLE": (53, -1022, 1023),
    "X87": (64, -16382, 16383),
    "QUAD": (113, -16382, 16383),
}
QUAD = FORMATS["QUAD"]

FUNCTIONS = {
    "SQRT": mpmath.sqrt,
    "SIN": mpmath.sin,
    "COS": mpmath.cos,
    "TAN": mpmath.tan,
    "EXP": mpmath.exp,
    "EXP2": lambda x: mpmath.power(2, x),
    "LOG": mpmath.log,
    "LOG2": lambda x: mpmath.log(x, 2),
    "LOG10": mpmath.log10,
    "FLOOR": mpmath.floor,
    "CEIL": mpmath.ceil,
}
POSITIVE_ONLY = {"SQRT", "LOG", "LOG2", "LOG10"}
TRIG = {"SIN", "COS", "TAN"}
EXACT = {"FLOOR", "CEIL"}

BASE_PRECISION = 1400


def round_to(negative, mantissa, exponent, fmt):
    """Rounds mantissa * 2^exponent into fmt; returns (mantissa, exponent), 'inf', or 'zero'."""
    precision, min_exponent, max_exponent = fmt
    if mantissa == 0:
        return "zero"
    length = mantissa.bit_length()
    top = exponent + length - 1
    kept = precision if top >= min_exponent else precision - (min_exponent - top)
    shift = length - kept
    if shift > 0:
        quotient = mantissa >> shift if shift <= length + 1 else 0
        remainder = mantissa - (quotient << shift)
        half = 1 << (shift - 1)
        if remainder > half or (remainder == half and quotient & 1):
            quotient += 1
        mantissa, exponent = quotient, exponent + shift
    if mantissa == 0:
        return "zero"
    if exponent + mantissa.bit_length() - 1 > max_exponent:
        return "inf"
    return mantissa, exponent


def quad_bits(negative, rounded):
    sign = (1 << 127) if negative else 0
    if rounded == "zero":
        return sign
    if rounded == "inf":
        return sign | (0x7FFF << 112)
    mantissa, exponent = rounded
    while mantissa.bit_length() > 113:
        mantissa, exponent = mantissa >> 1, exponent + 1
    top = exponent + mantissa.bit_length() - 1
    if top >= -16382:
        fraction = (mantissa << (112 - (mantissa.bit_length() - 1))) & ((1 << 112) - 1)
        return sign | ((top + 16383) << 112) | fraction
    return sign | (mantissa << (exponent + 16382 + 112))


def parts_of(value):
    """(negative, mantissa, exponent) of an mpf."""
    sign, mantissa, exponent, _ = value._mpf_
    return bool(sign), int(mantissa), int(exponent)


def make_input(negative, mantissa, exponent):
    mp.prec = max(BASE_PRECISION, mantissa.bit_length() + 8)
    value = mpmath.ldexp(mpf(mantissa), exponent)
    return -value if negative else value


def evaluate(name, x, fmt):
    """The quad bit pattern of name(x) rounded into fmt, or None when the input has no vector."""
    _, mantissa, exponent = parts_of(x)
    top = exponent + mantissa.bit_length() - 1 if mantissa else 0
    mp.prec = BASE_PRECISION + (max(top, 0) if name in TRIG else 0)
    y = FUNCTIONS[name](x)
    if not mpmath.isfinite(y):
        return None
    negative, y_mantissa, y_exponent = parts_of(y)
    if y_mantissa == 0 and name not in EXACT:
        return None
    # floor and ceil keep the argument's sign on a zero result
    if y_mantissa == 0:
        return quad_bits(parts_of(x)[0], "zero")
    return quad_bits(negative, round_to(negative, y_mantissa, y_exponent, fmt))


def tie_distance(name, x, fmt):
    """How far the bits past the rounding position are from a tie, in [0, 0.5]."""
    precision = fmt[0]
    mp.prec = precision + 160
    y = FUNCTIONS[name](x)
    if not mpmath.isfinite(y) or y == 0:
        return 1.0
    _, mantissa, _ = parts_of(y)
    mantissa <<= max(0, precision + 100 - mantissa.bit_length())
    dropped = mantissa.bit_length() - precision
    if dropped <= 0:
        return 1.0
    fraction = (mantissa & ((1 << dropped) - 1)) / (1 << dropped)
    return abs(fraction - 0.5)


def random_in_format(rng, fmt, low_exponent, high_exponent):
    precision, min_exponent, max_exponent = fmt
    top = rng.randint(max(low_exponent, min_exponent), min(high_exponent, max_exponent))
    mantissa = rng.getrandbits(precision - 1) | (1 << (precision - 1))
    return mantissa, top - (precision - 1)


def inputs_for(name, fmt_name, rng):
    fmt = FORMATS[fmt_name]
    precision, min_exponent, max_exponent = fmt
    cases = []

    def add(negative, mantissa, exponent):
        if name in POSITIVE_ONLY and negative:
            negative = False
        cases.append((negative, mantissa, exponent))

    # Moderate magnitudes, where most programs live
    for _ in range(40):
        add(rng.random() < 0.5, *random_in_format(rng, fmt, -8, 8))

    if name in EXACT:
        for _ in range(20):
            add(rng.random() < 0.5, *random_in_format(rng, fmt, -3, precision + 2))
        return cases

    # The whole exponent range, which for the trigonometric functions means exact reduction of
    # huge arguments
    if name in ("EXP", "EXP2"):
        limit = max_exponent.bit_length() + 1
        for _ in range(20):
            add(rng.random() < 0.5, *random_in_format(rng, fmt, -precision - 4, limit))
    else:
        for _ in range(20):
            add(rng.random() < 0.5, *random_in_format(rng, fmt, min_exponent, max_exponent))
        add(False, (1 << precision) - 1, max_exponent - (precision - 1))
        add(False, 1, min_exponent - (precision - 1))

    # Just off the points where a result is exact
    if name in ("LOG", "LOG2", "LOG10"):
        for k in (1, 2, 3, 7):
            add(False, (1 << (precision - 1)) + k, -(precision - 1))
            add(False, (1 << precision) - k, -precision)
    for shift in (precision, precision * 2, 70, 130, 250):
        if -shift >= min_exponent:
            add(rng.random() < 0.5, *random_in_format(rng, fmt, -shift, -shift))

    if name in TRIG and fmt_name in ("DOUBLE", "X87", "QUAD"):
        # The double closest to a multiple of pi/2, and other classic huge arguments
        add(False, 6381956970095103, 797)
        add(False, *parts_of(make_input(False, 10**22, 0))[1:])
        add(False, 1, 1023)

    # The hardest roundings a search turns up
    if fmt_name == "HALF":
        pool = [
            (False, (1 << 10) | fraction, top - 10)
            for top in range(-14, 16)
            for fraction in range(1 << 10)
        ]
        if name in ("EXP", "EXP2"):
            pool += [(True, m, e) for _, m, e in pool]
    else:
        pool = [
            (name not in POSITIVE_ONLY and rng.random() < 0.5, *random_in_format(rng, fmt, -6, 6))
            for _ in range(2500)
        ]
    scored = sorted(pool, key=lambda c: tie_distance(name, make_input(*c), fmt))
    cases.extend(scored[:20])
    return cases


def main():
    rng = random.Random(0x67686F7469)
    lines = []
    for name in FUNCTIONS:
        for fmt_name, fmt in FORMATS.items():
            seen = set()
            for case in inputs_for(name, fmt_name, rng):
                if case in seen:
                    continue
                seen.add(case)
                x = make_input(*case)
                expected = evaluate(name, x, fmt)
                if expected is None:
                    continue
                x_bits = quad_bits(case[0], round_to(case[0], case[1], case[2], QUAD))
                lines.append(
                    "{{F::{}, M::{}, 0x{:016x}, 0x{:016x}, 0x{:016x}, 0x{:016x}}},".format(
                        name,
                        fmt_name,
                        x_bits >> 64,
                        x_bits & (2**64 - 1),
                        expected >> 64,
                        expected & (2**64 - 1),
                    )
                )
    sys.stdout.reconfigure(newline="\n")
    sys.stdout.write("// Generated by tools/gen_float_math_vectors.py; do not edit\n")
    sys.stdout.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
