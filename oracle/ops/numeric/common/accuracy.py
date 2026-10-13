"""Independent final-result acceptance for the accelerated FP32 quality budget."""
import math
import struct
from fractions import Fraction


def accepted(actual, expected, dtype, profile):
    if actual == expected:
        return True
    if profile == 'strict' or dtype not in (3, 4):
        return False
    try:
        a, r = int(actual, 16), int(expected, 16)
    except ValueError:
        return False
    width = 32 if dtype == 4 else 64
    sign = 1 << (width - 1)
    mask = sign - 1
    fmt = '>f' if width == 32 else '>d'
    x = struct.unpack(fmt, a.to_bytes(width // 8, 'big'))[0]
    reference = struct.unpack(fmt, r.to_bytes(width // 8, 'big'))[0]
    if not math.isfinite(x) or not math.isfinite(reference) or reference == 0:
        return False
    if not 2**-126 <= abs(reference) <= float.fromhex('0x1.fffffep127'):
        return False
    if width == 32:
        key = lambda n: sign - (n & mask) if n & sign else sign + n
        return abs(key(a) - key(r)) <= 4
    exponent = math.frexp(abs(reference))[1] - 1
    tolerance = Fraction(2) ** (exponent - 21)
    return abs(Fraction(x) - Fraction(reference)) <= tolerance


def accepted_values(actual, expected, dtype, profile):
    a, b = actual.split(), expected.split()
    return len(a) == len(b) and all(accepted(x,y,dtype,profile) for x,y in zip(a,b))
