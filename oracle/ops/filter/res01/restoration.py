from __future__ import annotations
from fractions import Fraction as Q
from functools import lru_cache
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

def noise_mad(image,mask,*,min_samples=1):
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

def calibrated_variance(image,*,gain,offset=0,read_sigma=0,below_offset='clamp',dtype='float64'):
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
