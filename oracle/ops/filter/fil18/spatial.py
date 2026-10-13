from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import sample
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros

def variable_gather(image,radius,*,disk=False,max_radius=10,boundary='truncate',dtype='float64'):
    h,w=shape(image);mr=frac(max_radius)
    scalar=not isinstance(radius,(list,tuple))
    if not scalar:same_shape(image,radius)
    out=zeros(h,w);valid=zeros(h,w,1)
    for y in range(h):
        for x in range(w):
            r=frac(radius if scalar else radius[y][x])
            if not 0<=r<=mr:raise DomainError('radius out of static bound')
            if not r:out[y][x]=image[y][x];continue
            n=r.numerator//r.denominator;a=[]
            for dy in range(-n,n+1):
                for dx in range(-n,n+1):
                    if disk and dx*dx+dy*dy>r*r:continue
                    v=sample(image,y+dy,x+dx,boundary)
                    if v is not None:a.append(v)
            out[y][x]=rn(sum(a,Q())/len(a),dtype)
    return out,valid
