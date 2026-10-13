from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import sample
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros

def laplacian(image,*,eight=False,dx=1,dy=1,scale=1,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image);dx,dy,scale=map(frac,(dx,dy,scale))
    if min(dx,dy)<=0 or (eight and dx!=dy):raise DomainError('spacing')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            c=sample(image,y,x,boundary,cval)
            e,wv=sample(image,y,x+1,boundary,cval),sample(image,y,x-1,boundary,cval)
            n,s=sample(image,y-1,x,boundary,cval),sample(image,y+1,x,boundary,cval)
            if eight:
                corners=sum((sample(image,y+a,x+b,boundary,cval) for a in (-1,1) for b in (-1,1)),Q())
                v=(4*(e+wv+n+s)+corners-20*c)/(6*dx*dx)
            else:v=(e+wv-2*c)/(dx*dx)+(n+s-2*c)/(dy*dy)
            out[y][x]=rn(scale*v,dtype)
    return out
