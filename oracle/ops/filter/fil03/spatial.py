from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.common.spatial import window_samples

def box(image,height,width,anchor,*,mean=True,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image)
    if min(height,width)<1 or not(0<=anchor[0]<height and 0<=anchor[1]<width):raise DomainError('box geometry')
    if boundary=='truncate' and not mean:raise DomainError('sum does not support truncate')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            a=window_samples(image,y,x,height,width,anchor,boundary,cval)
            out[y][x]=rn(sum(map(frac,a),Q())/(len(a) if mean else 1),dtype)
    return out
