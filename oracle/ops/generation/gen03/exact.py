from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
from oracle.ops.generation.common.exact import round_sqrt_plus
from oracle.ops.generation.common.exact import sqrt_fraction


def rectangle_coverage(bounds, pixel=(0,0)):
    x0,y0,x1,y1 = map(frac,bounds)
    if x1 < x0 or y1 < y0:
        raise ValueError('reversed bounds')
    x,y = map(frac,pixel)
    return max(F(0), min(x+1,x1)-max(x,x0))*max(F(0),min(y+1,y1)-max(y,y0))


def rectangle_sdf(p, bounds, dtype='float64'):
    x0,y0,x1,y1=map(frac,bounds)
    if x1 < x0 or y1 < y0:
        raise ValueError('reversed bounds')
    x,y=map(frac,p)
    qx=abs(x-(x0+x1)/2)-(x1-x0)/2
    qy=abs(y-(y0+y1)/2)-(y1-y0)/2
    if max(qx,qy) <= 0:
        return round_fraction(max(qx,qy),dtype)
    return sqrt_fraction(max(qx,0)**2+max(qy,0)**2,dtype)


def disk_sdf(p, center, radius, dtype='float64'):
    r=frac(radius)
    if r < 0:
        raise ValueError('negative radius')
    q=sum((frac(a)-frac(b))**2 for a,b in zip(p,center))
    return round_sqrt_plus(q,-r,dtype)
