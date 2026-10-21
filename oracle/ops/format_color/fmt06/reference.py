"""Independent FMT-06 oracle using exact Fraction arithmetic and direct RN-even.

This module imports no production code and uses no intermediate floating-point
arithmetic. ``convert`` accepts raw sample bits and dtype names, and returns raw
target bits. None denotes a violated numerical caller precondition, not a required
runtime diagnostic. Bounds are four Endpoint values: source low/high, target
low/high. Static parameter validation belongs to the caller of this oracle.
"""
from dataclasses import dataclass
from fractions import Fraction

WIDTH = {"uint8": 8, "int8": 8, "uint16": 16, "int16": 16,
         "int64": 64, "float32": 32, "float64": 64}
LIMITS = {"uint8": (0, 255), "int8": (-128, 127), "uint16": (0, 65535),
          "int16": (-32768, 32767), "int64": (-2**63, 2**63 - 1),
          "float32": (0, 1), "float64": (0, 1)}
FLOATS = {"float32": (23, 8, 127), "float64": (52, 11, 1023)}


@dataclass(frozen=True)
class Endpoint:
    value: Fraction
    negative_zero: bool = False


def finite_value(bits, dtype):
    """Import a finite sample exactly, or return None for NaN/infinity."""
    if dtype not in FLOATS:
        if LIMITS[dtype][0] < 0 and bits >> (WIDTH[dtype] - 1):
            bits -= 1 << WIDTH[dtype]
        return Fraction(bits)
    fraction, exponent_bits, bias = FLOATS[dtype]
    exponent = (bits >> fraction) & ((1 << exponent_bits) - 1)
    significand = bits & ((1 << fraction) - 1)
    if exponent == (1 << exponent_bits) - 1:
        return None
    if exponent:
        significand |= 1 << fraction
    sign = -1 if bits >> (WIDTH[dtype] - 1) else 1
    return sign * Fraction(significand) * Fraction(2) ** (
        (exponent or 1) - bias - fraction)


def endpoint(text):
    """Read one production typed endpoint token without production parsing."""
    if text.startswith("i:"):
        return Endpoint(Fraction(int(text[2:])))
    if text.startswith("f64:"):
        bits = int(text[4:], 16)
        value = finite_value(bits, "float64")
        if value is None:
            raise ValueError("nonfinite endpoint")
        return Endpoint(value, not value and bool(bits >> 63))
    raise ValueError("unknown typed endpoint")


def round_integer(value):
    """Round an exact signed rational to the nearest integer, ties to even."""
    quotient, remainder = divmod(value.numerator, value.denominator)
    return quotient + (2 * remainder > value.denominator or
                       (2 * remainder == value.denominator and quotient % 2))


def floating_bits(value, dtype, negative_zero=False):
    """Round directly to binary32/64, including gradual underflow and infinity."""
    fraction, exponent_bits, bias = FLOATS[dtype]
    minimum = 1 - bias - fraction
    sign = int(value < 0 or (not value and negative_zero)) << (WIDTH[dtype] - 1)
    value = abs(value)
    if not value:
        return sign
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if value < Fraction(2) ** exponent:
        exponent -= 1
    quantum = max(minimum, exponent - fraction)
    significand = round_integer(value / Fraction(2) ** quantum)
    if significand >= 1 << (fraction + 1):
        significand >>= 1
        quantum += 1
    field = quantum - minimum + 1 if significand >= 1 << fraction else 0
    if field:
        significand -= 1 << fraction
    maximum = (1 << exponent_bits) - 1
    if field >= maximum:
        field, significand = maximum, 0
    return sign | (field << fraction) | significand


def convert(bits, source, target, rescale=True, clip=False, bounds=None):
    """Return exact specified bits for valid inputs, or None outside the domain."""
    if bounds is None:
        bounds = tuple(Endpoint(Fraction(x)) for x in (*LIMITS[source], *LIMITS[target]))
    s0, s1, t0, t1 = bounds
    if source == target and (not rescale or (s0 == t0 and s1 == t1)):
        return bits
    value = finite_value(bits, source)
    if value is None:
        if target not in FLOATS:
            return None
        sf, _, _ = FLOATS[source]
        tf, te, _ = FLOATS[target]
        sign = bits >> (WIDTH[source] - 1)
        payload = bits & ((1 << sf) - 1)
        if payload:
            payload &= (1 << (sf - 1)) - 1
            payload = payload << (tf - sf) if tf >= sf else payload >> (sf - tf)
            return ((sign << (WIDTH[target] - 1)) | (((1 << te) - 1) << tf) |
                    (1 << (tf - 1)) | payload)
        sign ^= bool(rescale and t1.value < t0.value)
        return (sign << (WIDTH[target] - 1)) | (((1 << te) - 1) << tf)
    negative_zero = bool(source in FLOATS and not value and bits >> (WIDTH[source] - 1))
    if rescale:
        if value == s0.value:
            value, negative_zero = t0.value, t0.negative_zero
        elif value == s1.value:
            value, negative_zero = t1.value, t1.negative_zero
        else:
            value = t0.value + (value - s0.value) * (t1.value - t0.value) / (s1.value - s0.value)
            negative_zero = False
    if target in FLOATS:
        result = floating_bits(value, target, negative_zero)
        fraction, exponent_bits, _ = FLOATS[target]
        infinity = ((1 << exponent_bits) - 1) << fraction
        if result & infinity == infinity:
            if not clip:
                return None
            result = (result & (1 << (WIDTH[target] - 1))) | (infinity - 1)
        return result
    rounded = round_integer(value)
    low, high = LIMITS[target]
    if not low <= rounded <= high:
        if not clip:
            return None
        rounded = max(low, min(high, rounded))
    return rounded & ((1 << WIDTH[target]) - 1)
