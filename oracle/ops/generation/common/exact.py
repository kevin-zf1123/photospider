"""Independent finite rational / IEEE oracle. No Photospider runtime dependency.

Fractions retain the exact value of binary floating inputs. Algorithms labelled
exact return Fraction(s); round_bits/sqrt_fraction implement final IEEE RNE.
This is a test oracle, not a resource-accounted kernel implementation.
"""

from __future__ import annotations
from fractions import Fraction as F
import math
import struct


def frac(x) -> F:
    if isinstance(x, float) and not math.isfinite(x):
        raise ValueError('finite input required')
    return F(x)


def power2(e: int) -> F:
    return F(1 << e) if e >= 0 else F(1, 1 << -e)


def floor_log2(x: F) -> int:
    x = frac(x)
    if x <= 0:
        raise ValueError('positive input required')
    e = x.numerator.bit_length() - x.denominator.bit_length()
    return e - (x < power2(e))


def _round_integer(n: int, d: int) -> int:
    q, r = divmod(n, d)
    return q + (2*r > d or (2*r == d and q & 1))


def _format(dtype: str):
    if dtype in ('float32', 'f32', 32):
        return 24, -126, 127, 8, 32
    if dtype in ('float64', 'f64', 64):
        return 53, -1022, 1023, 11, 64
    raise ValueError('dtype must be float32 or float64')


def round_bits(x, dtype='float64', negative_zero=False) -> int:
    """Correctly round any finite rational to IEEE bits; overflow raises.

    negative_zero applies only to a mathematically exact zero. A negative
    nonzero value that underflows produces -0 independently of this flag.
    """
    p, emin, emax, ebits, width = _format(dtype)
    x = frac(x)
    negative = x < 0 or (not x and negative_zero)
    sign = int(negative) << (width-1)
    x = abs(x)
    if not x:
        return sign
    e = floor_log2(x)
    unit = power2(max(e, emin) - (p-1))
    z = x / unit
    q = _round_integer(z.numerator, z.denominator)
    if e < emin:
        # q can reach exactly min-normal but cannot exceed it.
        return sign | q
    if q == 1 << p:
        q >>= 1
        e += 1
    if e > emax:
        raise OverflowError('finite destination overflow')
    exponent = e + ((1 << (ebits-1))-1)
    return sign | (exponent << (p-1)) | (q - (1 << (p-1)))


def from_bits(bits: int, dtype='float64') -> float:
    width = _format(dtype)[-1]
    if bits < 0 or bits >= 1 << width:
        raise ValueError('bits out of range')
    return struct.unpack('>f' if width == 32 else '>d', bits.to_bytes(width//8, 'big'))[0]


def float_bits(x: float, dtype='float64') -> int:
    width = _format(dtype)[-1]
    return int.from_bytes(struct.pack('>f' if width == 32 else '>d', x), 'big')


def round_fraction(x, dtype='float64', negative_zero=False) -> float:
    return from_bits(round_bits(x, dtype, negative_zero), dtype)


def bit_hex(x: float, dtype='float64') -> str:
    return f'{float_bits(x,dtype):0{_format(dtype)[-1]//4}x}'


def sqrt_bounds(q, precision_bits: int = 160):
    """Certified rational enclosure of sqrt(q), via integer isqrt only."""
    q = frac(q)
    if q < 0 or precision_bits < 2:
        raise ValueError('invalid sqrt domain/precision')
    if not q:
        return F(0), F(0)
    e = floor_log2(q) // 2
    unit = power2(e - precision_bits)
    z = q / (unit*unit)
    a = math.isqrt(z.numerator // z.denominator)
    lo = a * unit
    return (lo, lo) if lo*lo == q else (lo, (a+1)*unit)


def sqrt_fraction(q, dtype='float64') -> float:
    """Correctly rounded sqrt(rational) using an exact midpoint comparison."""
    q = frac(q)
    if q < 0:
        raise ValueError('sqrt of negative rational')
    if not q:
        return 0.0
    p, emin, _, _, _ = _format(dtype)
    e = floor_log2(q)//2
    unit = power2(max(e, emin) - (p-1))
    z = q/(unit*unit)
    a = math.isqrt(z.numerator//z.denominator)
    cmp = 4*z.numerator - z.denominator*(2*a+1)**2
    a += cmp > 0 or (cmp == 0 and a & 1)
    return round_fraction(a*unit, dtype)


def round_sqrt_plus(q, b=0, dtype='float64') -> float:
    """Certified RN(sqrt(q)+b) for rational q,b, with explicit refinement cap."""
    q, b = frac(q), frac(b)
    if q < 0:
        raise ValueError('negative radicand')
    sn, sd = math.isqrt(q.numerator), math.isqrt(q.denominator)
    if sn*sn == q.numerator and sd*sd == q.denominator:
        return round_fraction(F(sn,sd)+b, dtype)
    for precision in (80,160,320,640,1280,2560,5120):
        lo, hi = sqrt_bounds(q, precision)
        a, z = round_bits(lo+b,dtype), round_bits(hi+b,dtype)
        if a == z:
            return from_bits(a,dtype)
    raise ArithmeticError('NotConverged: sqrt-plus rounding cell unresolved')


def accelerated_ok(reference: float, actual: float, dtype='float64') -> bool:
    """NUM final budget. Not a general accelerated admission proof."""
    width = _format(dtype)[-1]
    rbits, abits = float_bits(reference,dtype), float_bits(actual,dtype)
    if not math.isfinite(reference) or not math.isfinite(actual):
        return rbits == abits
    if reference == 0 or actual == 0:
        return rbits == abits
    if width == 32:
        def ordered(b):
            return ((~b) & 0xffffffff) if b & 0x80000000 else b | 0x80000000
        return abs(ordered(rbits)-ordered(abits)) <= 4
    r, a = F(reference), F(actual)
    if abs(r) < power2(-126) or abs(r) > F.from_float(float.fromhex('0x1.fffffep127')):
        return rbits == abits
    bound = 4*power2(floor_log2(abs(r))-23)
    return abs(a-r) <= bound
