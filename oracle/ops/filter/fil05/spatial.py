from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.common.spatial import window_samples
from oracle.ops.filter.common.special import quantile

def percentile(image,footprint,anchor,*,q=Q(1,2),interpolation='linear',boundary='reflect_half',cval=0,empty='error',dtype='float64'):
    h,w=shape(image);kh,kw=shape(footprint);q=frac(q)
    if not 0<=q<=1 or not any(v for r in footprint for v in r):raise DomainError('q/footprint')
    if any(v not in (0,1) for r in footprint for v in r):raise DomainError('nonbinary footprint')
    if not(0<=anchor[0]<kh and 0<=anchor[1]<kw):raise DomainError('anchor')
    if interpolation not in ('linear','lower','higher','nearest_even','midpoint'):raise DomainError('interpolation')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a=window_samples(image,y,x,kh,kw,anchor,boundary,cval,footprint)
            if not a:
                if empty=='copy_center':out[y][x]=image[y][x];continue
                raise DomainError('empty footprint intersection')
            out[y][x]=quantile(a,q,interpolation,dtype)
    return out
