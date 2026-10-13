from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import sample
from oracle.ops.filter.common.core import zeros

def post_aa(image,guide,*,threshold=0.1,blend=Q(1,4),dtype='float64'):
    h,w=same_shape(image,guide);threshold,blend=frac(threshold),frac(blend)
    if threshold<=0 or not 0<=blend<=Q(1,2):raise DomainError('AA parameters')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            if not blend:out[y][x]=image[y][x];continue
            # Centre is part of the published guide validation support.
            sample(guide,y,x,'clamp')
            ex=abs(sample(guide,y,x+1,'clamp')-sample(guide,y,x-1,'clamp'))
            ey=abs(sample(guide,y+1,x,'clamp')-sample(guide,y-1,x,'clamp'))
            if max(ex,ey)<=threshold:out[y][x]=image[y][x];continue
            d=(1,0) if ex>=ey else (0,1)
            a=sample(image,y+d[0],x+d[1],'clamp');b=sample(image,y-d[0],x-d[1],'clamp')
            out[y][x]=rn((1-blend)*frac(image[y][x])+blend*(a+b)/2,dtype)
    return out
