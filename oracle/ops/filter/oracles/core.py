"""Exact rational arithmetic and *direct* IEEE binary32/binary64 rounding.

Inputs are already-decoded finite tensor samples. Python float represents both
binary32 and binary64 exactly; use from_bits for unambiguous fixtures. Fraction
is also accepted for internal mathematical expressions, not as a public dtype.
No old filter implementation or NumPy/SciPy arithmetic is used here.
"""
from __future__ import annotations
from fractions import Fraction as Q
import math
import struct
from typing import Iterable

class DomainError(ValueError):
    pass
class Inconclusive(ArithmeticError):
    """No certificate was obtained within the oracle's explicit work cap."""

FORMATS = {'float32': (23, 8, 127, -126, 127),
           'float64': (52, 11, 1023, -1022, 1023)}

def frac(x) -> Q:
    if isinstance(x, Q): return x
    if isinstance(x, int): return Q(x)
    x = float(x)
    if not math.isfinite(x): raise Inconclusive('finite-rational reference does not cover this nonfinite value')
    return Q.from_float(x)

def pow2(e: int) -> Q:
    return Q(1 << e) if e >= 0 else Q(1, 1 << -e)

def floor_log2(x: Q) -> int:
    if x <= 0: raise DomainError('log2 needs positive rational')
    n, d = x.numerator, x.denominator
    e = n.bit_length()-d.bit_length()
    if x < pow2(e): e -= 1
    return e

def round_integer_even(x: Q) -> int:
    k, r = divmod(x.numerator, x.denominator)
    return k + int(2*r > x.denominator or (2*r == x.denominator and k % 2))

def round_bits(x, dtype='float64') -> int:
    """Direct IEEE RN-even, including NUM overflow and NaN conversion."""
    if isinstance(x, float) and x == 0:
        f, eb, _, _, _ = FORMATS[dtype]
        return int(math.copysign(1, x) < 0) << (f+eb)
    if isinstance(x, float) and not math.isfinite(x):
        raw = bits(x, 'float64')
        sign = raw >> 63
        f, eb, _, _, _ = FORMATS[dtype]
        if math.isinf(x): return (sign << (f+eb)) | (((1 << eb)-1) << f)
        payload = raw & ((1 << 51)-1)
        if dtype == 'float32':
            retained = payload >> 29
            payload = retained or int(payload != 0)
        return (sign << (f+eb)) | (((1 << eb)-1) << f) | (1 << (f-1)) | payload
    x = frac(x)
    f, eb, bias, emin, emax = FORMATS[dtype]
    width = 1+eb+f
    sign = (1 << (width-1)) if x < 0 else 0
    a = abs(x)
    if not a: return 0
    e = floor_log2(a)
    quantum = max(e, emin)-f
    significand = round_integer_even(a / pow2(quantum))
    if significand == 0: return sign  # negative nonzero underflow preserves -0
    if e < emin:
        if significand < (1 << f): return sign | significand
        e = emin
    if significand >= (1 << (f+1)):
        significand >>= 1
        e += 1
    if e > emax:
        return sign | (((1 << eb)-1) << f)
    return sign | ((e+bias) << f) | (significand-(1 << f))

def from_bits(bits: int, dtype='float64') -> float:
    n = 4 if dtype == 'float32' else 8
    return struct.unpack('>f' if n == 4 else '>d', int(bits).to_bytes(n, 'big'))[0]

def bits(x: float, dtype='float64') -> int:
    return int.from_bytes(struct.pack('>f' if dtype=='float32' else '>d', x), 'big')

def rn(x, dtype='float64') -> float:
    return from_bits(round_bits(x, dtype), dtype)

def hex_bits(x, dtype='float64') -> str:
    return f'{bits(x,dtype):0{8 if dtype=="float32" else 16}x}'

def input_value(x, dtype='float64') -> float:
    """Explicit authoring conversion; do not insert inside an E expression."""
    return rn(x, dtype)

def zeros(h: int, w: int, value=0.0):
    return [[value for _ in range(w)] for _ in range(h)]

def shape(a):
    if not a or not a[0]: raise DomainError('positive 2D shape required')
    w=len(a[0])
    if any(len(row)!=w for row in a): raise DomainError('ragged array')
    return len(a),w

def same_shape(*arrays):
    s=shape(arrays[0])
    if any(shape(a)!=s for a in arrays[1:]): raise DomainError('shape mismatch')
    return s

def map_index(i: int, n: int, mode: str):
    if 0 <= i < n: return i
    if mode in ('constant','truncate'): return None
    if mode == 'clamp': return min(max(i,0),n-1)
    if mode == 'wrap': return i % n
    if mode == 'reflect_half':
        t=i%(2*n); return t if t<n else 2*n-1-t
    if mode == 'reflect_whole':
        if n==1: return 0
        t=i%(2*n-2); return t if t<n else 2*n-2-t
    raise DomainError('unknown boundary')

def coordinate(y,x,h,w,mode):
    yy,xx=map_index(y,h,mode),map_index(x,w,mode)
    return None if yy is None or xx is None else (yy,xx)

def sample(a,y,x,mode='reflect_half',cval=0):
    h,w=shape(a); p=coordinate(y,x,h,w,mode)
    if p is None:
        if mode=='truncate': return None
        return frac(cval)
    return frac(a[p[0]][p[1]])

def ordered_finite_key(x,dtype):
    b=bits(x,dtype); n=32 if dtype=='float32' else 64
    return (~b & ((1<<n)-1)) if b>>(n-1) else b | (1<<(n-1))

def accelerated_accept(reference, candidate, dtype='float64') -> dict:
    """NUM final FP32-scaled four ULP rule; not an implementation proof.

    Branch/copy/special-value exactness must be checked by separate fixtures.
    Outside the Float32 normal range, strict bits are mandatory.
    """
    if not math.isfinite(reference) or not math.isfinite(candidate):
        return {'accepted': bits(reference,dtype)==bits(candidate,dtype), 'rule':'exact_special'}
    r=frac(reference); c=frac(candidate)
    if not r or abs(r)<pow2(-126) or abs(r)>Q(2**24-1)*pow2(104):
        return {'accepted':bits(reference,dtype)==bits(candidate,dtype),'rule':'strict_fallback'}
    if dtype=='float32':
        distance=abs(ordered_finite_key(reference,dtype)-ordered_finite_key(candidate,dtype))
        return {'accepted':distance<=4,'rule':'ordered_float32_ulp','distance':distance}
    allowance=4*pow2(floor_log2(abs(r))-23)
    return {'accepted':abs(c-r)<=allowance,'rule':'float32_scaled_absolute','bound':str(allowance)}

def solve_linear(a,b):
    """Exact Gaussian elimination, deterministic first nonzero pivot."""
    n=len(b)
    if len(a)!=n or any(len(row)!=n for row in a): raise DomainError('square system required')
    m=[[frac(v) for v in row]+[frac(b[i])] for i,row in enumerate(a)]
    for k in range(n):
        pivot=next((i for i in range(k,n) if m[i][k]),None)
        if pivot is None: raise DomainError('singular exact system')
        m[k],m[pivot]=m[pivot],m[k]
        scale=m[k][k]; m[k]=[v/scale for v in m[k]]
        for i in range(n):
            if i!=k and m[i][k]:
                s=m[i][k];m[i]=[x-s*y for x,y in zip(m[i],m[k])]
    return [row[-1] for row in m]

def median_exact(values):
    a=sorted(map(frac,values));n=len(a)
    if not n: raise DomainError('empty order statistic')
    return a[n//2] if n%2 else (a[n//2-1]+a[n//2])/2
