from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.common.spatial import window_samples

def local_moments(image,height,width,anchor,*,other=None,ddof=0,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image)
    if other is not None:same_shape(image,other)
    if ddof<0:raise DomainError('ddof')
    means=zeros(h,w);moments=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a=list(map(frac,window_samples(image,y,x,height,width,anchor,boundary,cval)))
            b=a if other is None else list(map(frac,window_samples(other,y,x,height,width,anchor,boundary,cval)))
            n=len(a)
            if n<=ddof:raise DomainError('N <= ddof')
            sa,sb=sum(a,Q()),sum(b,Q())
            means[y][x]=rn(sa/n,dtype)
            moments[y][x]=rn((n*sum((u*v for u,v in zip(a,b)),Q())-sa*sb)/(n*(n-ddof)),dtype)
    return means,moments
