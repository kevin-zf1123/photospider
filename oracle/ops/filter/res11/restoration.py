from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.common.spatial import window_samples
import copy

def low_contrast_smoothing(image,mask,*,radius=1,range_threshold=1,strength=Q(1,2),dtype='float64'):
    h,w=same_shape(image,mask);tau,s=frac(range_threshold),frac(strength)
    if radius<1 or tau<=0 or not 0<=s<=1:raise DomainError('deband parameters')
    if not s:return copy.deepcopy(image)
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            m=frac(mask[y][x])
            if not 0<=m<=1:raise DomainError('mask range')
            if not m:out[y][x]=image[y][x];continue
            a=list(map(frac,window_samples(image,y,x,2*radius+1,2*radius+1,(radius,radius),'reflect_half')))
            center=frac(image[y][x])
            out[y][x]=rn(center+m*s*(sum(a,Q())/len(a)-center),dtype) if max(a)-min(a)<=tau else image[y][x]
    return out

def pairwise_block_boundary_smoothing(image,mask,*,block_height=8,block_width=8,origin_y=0,origin_x=0,threshold=1,strength=Q(1,4),dtype='float64'):
    h,w=same_shape(image,mask);tau,s=frac(threshold),frac(strength)
    if min(block_height,block_width)<2 or tau<=0 or not 0<=s<=Q(1,2):raise DomainError('deblock parameters')
    m=[[frac(v) for v in row] for row in mask]
    if any(not 0<=v<=1 for row in m for v in row):raise DomainError('mask range')
    current=copy.deepcopy(image)
    for axis in (1,0):
        out=copy.deepcopy(current)
        for y in range(h):
            for x in range(w):
                if axis==1:
                    if x==0 or (x-origin_x)%block_width:continue
                    p,q=(y,x-1),(y,x)
                else:
                    if y==0 or (y-origin_y)%block_height:continue
                    p,q=(y-1,x),(y,x)
                if not s or min(m[p[0]][p[1]],m[q[0]][q[1]])==0:continue
                a,b=frac(current[p[0]][p[1]]),frac(current[q[0]][q[1]])
                if abs(b-a)<=tau:
                    d=s*min(m[p[0]][p[1]],m[q[0]][q[1]])*(b-a)
                    out[p[0]][p[1]]=rn(a+d,dtype);out[q[0]][q[1]]=rn(b-d,dtype)
        current=out
    return current
