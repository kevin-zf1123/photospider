"""Independent finite IEEE/rational oracle. Standard-library only; no kernel imports.

Float values at public helper entry points are quantized to the declared input
storage first. Fractions used inside expressions are exact. Resource limits are
oracle limits, not claims about production kernels.
"""
from __future__ import annotations
from fractions import Fraction
from math import isfinite, isqrt, copysign
import struct
from typing import Sequence

F = Fraction

class OracleError(ValueError):
    """A reference-domain error (not a production photospider Status object)."""


def config(dtype: str) -> tuple[int, int, int, int]:
    if dtype == 'f32': return 24, -126, 127, 32
    if dtype == 'f64': return 53, -1022, 1023, 64
    raise OracleError('dtype must be f32 or f64')


def bits(value: float, dtype: str = 'f64') -> int:
    config(dtype)
    return int.from_bytes(struct.pack('>f' if dtype == 'f32' else '>d', value), 'big')


def from_bits(value: int, dtype: str = 'f64') -> float:
    _, _, _, n = config(dtype)
    return struct.unpack('>f' if dtype == 'f32' else '>d', value.to_bytes(n//8, 'big'))[0]


def frac(value) -> Fraction:
    if isinstance(value, float) and not isfinite(value):
        raise OracleError('InvalidDomain: non-finite semantic input')
    return F(value)


def pow2(e: int) -> Fraction:
    return F(1 << e) if e >= 0 else F(1, 1 << -e)


def floor_log2(x: Fraction) -> int:
    if x <= 0: raise OracleError('positive argument required')
    e = x.numerator.bit_length() - x.denominator.bit_length()
    return e if x >= pow2(e) else e-1


def round_int(x: Fraction) -> int:
    """Nearest-even for nonnegative exact rationals."""
    if x < 0: return -round_int(-x)
    q, r = divmod(x.numerator, x.denominator)
    return q + int(2*r > x.denominator or (2*r == x.denominator and q & 1))


def rn_bits(x, dtype: str = 'f64', negative_zero: bool = False) -> int:
    """Direct rational -> IEEE, without a double-rounding float intermediate."""
    p, emin, emax, n = config(dtype)
    x = frac(x)
    sign = int(x < 0 or (x == 0 and negative_zero)) << (n-1)
    x = abs(x)
    if x == 0: return sign
    e = floor_log2(x)
    ework = max(e, emin)
    m = round_int(x / pow2(ework-(p-1)))
    if m == 0: return sign
    if m >= 1 << p:
        m >>= 1
        ework += 1
    ebits = n-p
    if ework > emax:
        return sign | (((1 << ebits)-1) << (p-1))
    if m < 1 << (p-1): return sign | m
    return sign | ((ework-emin+1) << (p-1)) | (m-(1 << (p-1)))


def rn(x, dtype: str = 'f64', negative_zero: bool = False) -> float:
    y = from_bits(rn_bits(x, dtype, negative_zero), dtype)
    return y


def q(value, dtype: str = 'f64') -> float:
    """Fixture adapter: form the actual stored input before running a reference."""
    return rn(frac(value), dtype, isinstance(value, float) and value == 0 and copysign(1, value) < 0)


def fq(value, dtype: str = 'f64') -> Fraction:
    return frac(q(value, dtype))


def coverage(value, dtype: str = 'f64') -> Fraction:
    x = fq(value, dtype)
    if not 0 <= x <= 1: raise OracleError('InvalidDomain: coverage outside [0,1]')
    return x


def clamp(x, lo=F(0), hi=F(1)):
    return min(hi, max(lo, x))


def smooth_exact(low, high, x) -> Fraction:
    low, high, x = map(frac, (low, high, x))
    if not low < high: raise OracleError('InvalidDomain: low must be less than high')
    t = clamp((x-low)/(high-low))
    return t*t*(3-2*t)


def sqrt_affine(c, k, radicand, dtype: str = 'f64') -> float:
    """Certified RN(c + k*sqrt(r)); bounds are exact integer/rational enclosures."""
    c, k, r = map(frac, (c, k, radicand))
    if r < 0: raise OracleError('InvalidDomain: negative radicand')
    if r == 0 or k == 0: return rn(c, dtype)
    a, b = isqrt(r.numerator), isqrt(r.denominator)
    if a*a == r.numerator and b*b == r.denominator:
        return rn(c+k*F(a,b), dtype)
    initial = max(64, 64 - floor_log2(r)//2)
    for extra in (0, 64, 128, 256, 512, 1024, 2048, 4096, 8192):
        p = initial+extra
        u = isqrt((r.numerator << (2*p)) // r.denominator)
        lo, hi = F(u, 1<<p), F(u+1, 1<<p)
        low, high = sorted((c+k*lo, c+k*hi))
        l, h = rn_bits(low, dtype), rn_bits(high, dtype)
        if l == h:
            value = from_bits(l, dtype)
            return value
    raise OracleError('UncertifiedReference: exact enclosure did not decide rounding')


def solve_exact(matrix: Sequence[Sequence], rhs: Sequence) -> list[Fraction]:
    """Independent dense rational Gaussian elimination, small fixtures only."""
    n = len(rhs)
    if n > 64: raise OracleError('OracleLimit: at most 64 unknowns')
    if len(matrix) != n or any(len(row) != n for row in matrix):
        raise OracleError('matrix shape mismatch')
    a = [[frac(v) for v in row]+[frac(rhs[i])] for i,row in enumerate(matrix)]
    for j in range(n):
        pivot = next((i for i in range(j,n) if a[i][j] != 0), None)
        if pivot is None: raise OracleError('InvalidDomain: unanchored/singular system')
        a[j],a[pivot] = a[pivot],a[j]
        d = a[j][j]
        a[j] = [x/d for x in a[j]]
        for i in range(n):
            if i == j: continue
            d = a[i][j]
            if d:
                a[i] = [x-d*y for x,y in zip(a[i], a[j])]
    return [row[-1] for row in a]


def inverse3(matrix) -> list[list[Fraction]]:
    cols = [solve_exact(matrix, [int(i==j) for i in range(3)]) for j in range(3)]
    return [[cols[j][i] for j in range(3)] for i in range(3)]
