from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.common.spatial import window_samples
from oracle.ops.filter.fil01.spatial import convolve2d

def hessian(image,*,dx=1,dy=1,boundary='reflect_half',cval=0,dtype='float64'):
    dx,dy=frac(dx),frac(dy)
    if min(dx,dy)<=0:raise DomainError('spacing')
    kernels=[[[0,0,0],[1/(dx*dx),-2/(dx*dx),1/(dx*dx)],[0,0,0]],
             [[1/(4*dx*dy),0,-1/(4*dx*dy)],[0,0,0],[-1/(4*dx*dy),0,1/(4*dx*dy)]],
             [[0,1/(dy*dy),0],[0,-2/(dy*dy),0],[0,1/(dy*dy),0]]]
    return tuple(convolve2d(image,k,(1,1),direction='correlate',boundary=boundary,cval=cval,dtype=dtype) for k in kernels)

def structure_tensor(gx,gy,height,width,anchor,*,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=same_shape(gx,gy);out=[zeros(h,w) for _ in range(3)]
    for y in range(h):
        for x in range(w):
            a=list(map(frac,window_samples(gx,y,x,height,width,anchor,boundary,cval)))
            b=list(map(frac,window_samples(gy,y,x,height,width,anchor,boundary,cval)))
            for z,terms in zip(out,[ [u*u for u in a],[u*v for u,v in zip(a,b)],[v*v for v in b] ]):
                z[y][x]=rn(sum(terms,Q())/len(a),dtype)
    return tuple(out)
