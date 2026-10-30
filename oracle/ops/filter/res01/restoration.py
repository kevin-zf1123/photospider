from __future__ import annotations
from fractions import Fraction as Q
from functools import lru_cache
from math import isinf
from oracle.ops.filter.common.bounds import Context
from oracle.ops.filter.common.bounds import Interval
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.bounds import sqrt_q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import median_exact
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros

@lru_cache(None)
def _normal_quantile_bracket(precision):
    # Prove erf(c/sqrt(2)) straddles 1/2 by directed MPFR evaluation.
    lo,hi=Q(),Q(1)
    with Context(precision+96) as ctx:
        for _ in range(precision+8):
            m=(lo+hi)/2;v=(ctx.q(m)/ctx.q(2).sqrt()).monotone('erf');half=ctx.q(Q(1,2))
            if ctx.cmp(v.hi,half.lo)<0:lo=m
            elif ctx.cmp(v.lo,half.hi)>0:hi=m
            else:raise ArithmeticError('normal quantile bracket unresolved')
    return lo,hi

def noise_mad_exact_reference(image,mask,*,min_samples=1):
    h,w=same_shape(image,mask)
    if h<2 or w<2 or min_samples<1:raise DomainError('MAD shape/sample count')
    if any(v not in (0,1) for row in mask for v in row):raise DomainError('mask binary')
    residual=[]
    for y in range(h-1):
        for x in range(w-1):
            if all(mask[y+j][x+i] for j in (0,1) for i in (0,1)):
                residual.append(rn((frac(image[y][x])-frac(image[y][x+1])-frac(image[y+1][x])+frac(image[y+1][x+1]))/2))
    if len(residual)<min_samples:raise DomainError('insufficient selected residuals')
    m=frac(rn(median_exact(residual)));mad=median_exact([abs(frac(v)-m) for v in residual])
    if not mad:return 0.0
    def expr(c):
        lo,hi=_normal_quantile_bracket(c.precision+16)
        return c.q(mad)/Interval(c,c.q(lo).lo,c.q(hi).hi)
    return certify(expr)

def calibrated_variance_exact_reference(image,*,gain,offset=0,read_sigma=0,below_offset='clamp',dtype='float64'):
    h,w=shape(image);a,b,s=map(frac,(gain,offset,read_sigma))
    if a<=0 or s<0 or below_offset not in ('clamp','error'):raise DomainError('calibration')
    variance=zeros(h,w);sigma=zeros(h,w)
    for y in range(h):
        for x in range(w):
            v=frac(image[y][x])-b
            if v<0 and below_offset=='error':raise DomainError('below offset')
            v=a*max(v,0)+s*s
            variance[y][x]=rn(v,dtype);sigma[y][x]=sqrt_q(v,dtype)
    return variance,sigma


@lru_cache(None)
def _native_scale():
    # Independent certified rounding of 1/Phi^-1(3/4), not the C++ literal.
    def expression(ctx):
        lo, hi = _normal_quantile_bracket(ctx.precision + 16)
        return ctx.q(Q(1)) / Interval(ctx, ctx.q(lo).lo, ctx.q(hi).hi)
    return certify(expression)


def _native_median(values, dtype):
    ordered = sorted(map(frac, values))
    k = len(ordered) // 2
    if len(ordered) & 1:
        return ordered[k]
    a, b = ordered[k - 1], ordered[k]
    maximum = Q((2**24 - 1) * 2**104) if dtype == 'float32' else Q((2**53 - 1) * 2**971)
    if abs(a) <= maximum / 2 and abs(b) <= maximum / 2:
        return frac(rn(frac(rn(a + b, dtype)) / 2, dtype))
    return frac(rn(frac(rn(a / 2, dtype)) + frac(rn(b / 2, dtype)), dtype))


def noise_mad(image, mask, *, min_samples=1, dtype='float64'):
    """Native staged RES01A reference; validation here is oracle-only."""
    h, w = same_shape(image, mask)
    if min_samples < 1:
        raise DomainError('MAD sample count')
    if any(v not in (0, 1) for row in mask for v in row):
        raise DomainError('mask binary')
    residual = []
    for y in range(h - 1):
        for x in range(w - 1):
            if all(mask[y + j][x + i] for j in (0, 1) for i in (0, 1)):
                a, b, c, d = [frac(rn(frac(image[y + j][x + i]), dtype))
                              for j, i in ((0, 0), (0, 1), (1, 0), (1, 1))]
                ab, cd = frac(rn(a - b, dtype)), frac(rn(c - d, dtype))
                residual.append(frac(rn(frac(rn(ab - cd, dtype)) / 2, dtype)))
    if len(residual) < min_samples:
        raise DomainError('insufficient selected residuals')
    center = _native_median(residual, dtype)
    deviations = [abs(frac(rn(value - center, dtype))) for value in residual]
    spread = _native_median(deviations, dtype)
    return rn(spread * frac(_native_scale()), 'float64')


def calibrated_variance(image, *, gain, offset=0, read_sigma=0,
                        below_offset='clamp', dtype='float64'):
    """Round variance before sqrt; preserve the ideal reference above."""
    h, w = shape(image)
    a, b, s = [frac(rn(frac(value), dtype)) for value in (gain, offset, read_sigma)]
    if a <= 0 or s < 0 or below_offset not in ('clamp', 'error'):
        raise DomainError('calibration')
    read2_value = rn(s * s, dtype)
    read2 = None if isinf(read2_value) else frac(read2_value)
    variance, sigma = zeros(h, w), zeros(h, w)
    for y in range(h):
        for x in range(w):
            value = frac(rn(frac(image[y][x]), dtype))
            if value < b and below_offset == 'error':
                raise DomainError('below offset caller premise')
            delta_value = rn(value - b, dtype)
            if read2 is None or (isinf(delta_value) and delta_value > 0):
                v = float('inf')
            else:
                delta = Q() if delta_value <= 0 else frac(delta_value)
                v = rn(a * delta + read2, dtype)  # exact native FMA rounding
            variance[y][x] = v
            sigma[y][x] = v if isinf(v) else sqrt_q(frac(v), dtype)
    return variance, sigma
