from __future__ import annotations
from oracle.ops.generation.common.exact import frac


def rectangle_erosion(bounds, radius):
    a,b,c,d=map(frac,bounds);r=frac(radius)
    if r<0: raise ValueError('negative erosion radius')
    return None if c-a<=2*r or d-b<=2*r else (a+r,b+r,c-r,d-r)
