from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.fil07.spatial import clipped_box

def _rgb_shape(image):
    h,w=shape(image)
    if any(len(v)!=3 for row in image for v in row):raise DomainError('exactly three explicit RGB components required')
    return h,w

def airlight(image,*,radius=1,top_fraction=0.001,request_airlight=True):
    h,w=_rgb_shape(image);f=frac(top_fraction)
    if radius<0 or not 0<f<=1:raise DomainError('airlight parameters')
    rgb=[[[frac(v) for v in pixel] for pixel in row] for row in image]
    if any(v<0 for row in rgb for pixel in row for v in pixel):raise DomainError('nonnegative RGB required')
    dark=zeros(h,w)
    for y in range(h):
        for x in range(w):dark[y][x]=min(v for j,i in clipped_box(y,x,h,w,radius,radius) for v in rgb[j][i])
    if not request_airlight:return [[float(v) for v in row] for row in dark],None,None
    amount=max(1,-(-(f*h*w).numerator//(f*h*w).denominator))
    candidates=sorted([(y,x) for y in range(h) for x in range(w)],key=lambda p:(-dark[p[0]][p[1]],p))[:amount]
    p=min(candidates,key=lambda p:(-sum(rgb[p[0]][p[1]],Q()),p));a=rgb[p[0]][p[1]]
    if min(a)<=0:raise DomainError('nonpositive selected airlight')
    return [[float(v) for v in row] for row in dark],[rn(v) for v in a],p

def transmission(image,airlight,*,radius=1,omega=0.95,dtype='float64'):
    h,w=_rgb_shape(image);omega=frac(omega)
    if radius<0 or not 0<=omega<=1 or len(airlight)!=3:raise DomainError('transmission parameters')
    if omega==0:return zeros(h,w,1.0)
    a=list(map(frac,airlight))
    if min(a)<=0:raise DomainError('positive airlight required')
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            ratios=[]
            for j,i in clipped_box(y,x,h,w,radius,radius):
                for c in range(3):
                    v=frac(image[j][i][c])
                    if v<0:raise DomainError('nonnegative RGB')
                    ratios.append(v/a[c])
            out[y][x]=rn(min(max(1-omega*min(ratios),0),1),dtype)
    return out

def apply_dehaze(image,airlight,transmission,*,t_floor=0.1,dtype='float64'):
    h,w=_rgb_shape(image);same_shape(image,transmission);floor=frac(t_floor)
    if not 0<floor<=1 or len(airlight)!=3:raise DomainError('dehaze parameters')
    out=[[None]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            t=frac(transmission[y][x])
            if not 0<=t<=1:raise DomainError('transmission range')
            te=max(t,floor);p=[]
            for c in range(3):
                v=frac(image[y][x][c])
                if v<0:raise DomainError('nonnegative input RGB')
                if t==1:p.append(image[y][x][c]);continue
                a=frac(airlight[c])
                if a<=0:raise DomainError('positive airlight')
                p.append(rn(a+(v-a)/te,dtype))
            out[y][x]=p
    return out
