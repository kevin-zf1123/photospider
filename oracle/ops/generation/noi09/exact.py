from __future__ import annotations
from oracle.ops.generation.common.exact import frac


def linear_grain(rgb,alpha,strength,noise):
    s=frac(strength)
    if s<0:raise ValueError('negative strength')
    if len(rgb)!=3:raise ValueError('three RGB components')
    n=noise if isinstance(noise,(list,tuple)) else [noise]*3
    if len(n)!=3:raise ValueError('three noise components')
    if s==0:return list(rgb),alpha
    return [frac(c)+s*frac(g) for c,g in zip(rgb,n)],alpha
