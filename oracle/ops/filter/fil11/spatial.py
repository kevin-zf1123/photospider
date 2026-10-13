from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import sample
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros

def zero_dc_filter(image,kernel,*,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image);kh,kw=shape(kernel);ay,ax=kh//2,kw//2;out=zeros(h,w)
    k=[[frac(v) for v in row] for row in kernel]
    for y in range(h):
        for x in range(w):
            center=frac(image[y][x]);v=Q()
            for j in range(kh):
                for i in range(kw):
                    if (j,i)!=(ay,ax) and k[j][i]:v+=k[j][i]*(sample(image,y+j-ay,x+i-ax,boundary,cval)-center)
            out[y][x]=rn(v,dtype)
    return out
