from __future__ import annotations
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.fil02.spatial import separable
from oracle.ops.filter.fil04.transcend import gaussian_kernel
import copy

def detail_gain(base,detail,*,gain=1,dtype='float64'):
    h,w=same_shape(base,detail);g=frac(gain)
    if not g:return copy.deepcopy(base)
    return [[rn(frac(base[y][x])+g*frac(detail[y][x]),dtype) for x in range(w)] for y in range(h)]

def unsharp(image,*,amount=0,threshold=0,sigma_y=1,sigma_x=1,radius_y=1,radius_x=1,boundary='reflect_half',cval=0,dtype='float64'):
    h,w=shape(image);a,t=frac(amount),frac(threshold)
    if t<0:raise DomainError('threshold')
    if not a:return copy.deepcopy(image)
    kx,ky=gaussian_kernel(sigma_x,radius_x),gaussian_kernel(sigma_y,radius_y)
    b=separable(image,kx,ky,(radius_y,radius_x),normalization='sum',boundary=boundary,cval=cval,dtype=dtype)
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            v=frac(image[y][x]);d=frac(rn(v-frac(b[y][x]),dtype))
            out[y][x]=rn(v+a*d,dtype) if abs(d)>t else image[y][x]
    return out
