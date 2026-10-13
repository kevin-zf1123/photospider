from __future__ import annotations
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import zeros

def multiply(fr,fi,hr,hi,*,dtype='float64'):
    h,w=same_shape(fr,fi,hr,hi);rr=zeros(h,w);ii=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a,b,c,d=map(frac,(fr[y][x],fi[y][x],hr[y][x],hi[y][x]))
            rr[y][x]=rn(a*c-b*d,dtype);ii[y][x]=rn(a*d+b*c,dtype)
    return rr,ii
