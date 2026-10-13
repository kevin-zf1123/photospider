from __future__ import annotations
from collections import deque
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.fil02.spatial import separable
from oracle.ops.filter.fil08.spatial import gradient

def hysteresis(weak,strong,connectivity=8):
    h,w=same_shape(weak,strong)
    if connectivity not in (4,8):raise DomainError('connectivity')
    if any(v not in (0,1) for a in (weak,strong) for r in a for v in r):raise DomainError('binary required')
    if any(strong[y][x] and not weak[y][x] for y in range(h) for x in range(w)):raise DomainError('strong not subset of weak')
    out=zeros(h,w,0);queue=deque()
    for y in range(h):
        for x in range(w):
            if strong[y][x]:out[y][x]=1;queue.append((y,x))
    neighbors=[(-1,0),(1,0),(0,-1),(0,1)]
    if connectivity==8:neighbors+=[(-1,-1),(-1,1),(1,-1),(1,1)]
    while queue:
        y,x=queue.popleft()
        for dy,dx in neighbors:
            yy,xx=y+dy,x+dx
            if 0<=yy<h and 0<=xx<w and weak[yy][xx] and not out[yy][xx]:
                out[yy][xx]=1;queue.append((yy,xx))
    return out

def canny(image,*,sigma=1,radius=1,low=0.1,high=0.2,norm='l2',connectivity=8,dtype='float64'):
    from oracle.ops.filter.fil04.transcend import gaussian_kernel
    from oracle.ops.filter.fil09.transcend import magnitude
    low,high=frac(low),frac(high)
    if not 0<low<high:raise DomainError('canny thresholds')
    h,w=shape(image);k=gaussian_kernel(sigma,radius)
    b=separable(image,k,k,(radius,radius),normalization='sum',dtype=dtype)
    gx,gy=gradient(b,dtype=dtype);m=magnitude(gx,gy,norm=norm,dtype=dtype)
    weak=zeros(h,w,0);strong=zeros(h,w,0)
    for y in range(h):
        for x in range(w):
            a,bv=frac(gx[y][x]),frac(gy[y][x])
            if not a and not bv:continue
            if abs(a)>=2*abs(bv):dy,dx=0,1
            elif abs(bv)>=2*abs(a):dy,dx=1,0
            else:dy,dx=1,(1 if a*bv>=0 else -1)
            if not(0<=y+dy<h and 0<=x+dx<w and 0<=y-dy<h and 0<=x-dx<w):continue
            v=frac(m[y][x])
            if v>=frac(m[y+dy][x+dx]) and v>frac(m[y-dy][x-dx]):
                weak[y][x]=int(v>=low);strong[y][x]=int(v>=high)
    return hysteresis(weak,strong,connectivity)
