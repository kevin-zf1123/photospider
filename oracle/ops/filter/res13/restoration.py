from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import sample
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros

def ball_background(image,*,radius=1,height=1,dtype='float64'):
    h,w=shape(image);hh=frac(height)
    if radius<1 or hh<=0:raise DomainError('nonflat ball parameters')
    taps=[]
    for dy in range(-radius,radius+1):
        for dx in range(-radius,radius+1):
            d2=dy*dy+dx*dx
            if d2>radius*radius:continue
            if d2==0:b=0.0
            elif d2==radius*radius:b=rn(-hh)
            else:b=certify(lambda c:c.q(hh)*(c.q(1-Q(d2,radius*radius)).sqrt()-1))
            taps.append((dy,dx,frac(b)))
    erosion=zeros(h,w);out=zeros(h,w)
    for y in range(h):
        for x in range(w):erosion[y][x]=rn(min(sample(image,y+dy,x+dx,'reflect_half')-b for dy,dx,b in taps),dtype)
    for y in range(h):
        for x in range(w):out[y][x]=rn(max(sample(erosion,y-dy,x-dx,'reflect_half')+b for dy,dx,b in taps),dtype)
    return out

def subtract_background(image,background,*,restore_offset=0,dtype='float64'):
    h,w=same_shape(image,background);b=frac(restore_offset)
    return [[rn(frac(image[y][x])-frac(background[y][x])+b,dtype) for x in range(w)] for y in range(h)]

def flat_field(image,dark,flat,flat_dark,mask,*,reference_gain,invalid='error',dtype='float64'):
    h,w=same_shape(image,dark,flat,flat_dark,mask);g=frac(reference_gain)
    if g<=0 or invalid not in ('copy_input','zero','error'):raise DomainError('flat field parameters')
    out=zeros(h,w);valid=zeros(h,w,0)
    for y in range(h):
        for x in range(w):
            m=mask[y][x]
            if m not in (0,1):raise DomainError('binary valid_mask')
            den=frac(flat[y][x])-frac(flat_dark[y][x]) if m else Q()
            if m and den>0:
                out[y][x]=rn((frac(image[y][x])-frac(dark[y][x]))*g/den,dtype);valid[y][x]=1
            elif invalid=='copy_input':out[y][x]=image[y][x]
            elif invalid=='zero':out[y][x]=0.0
            else:raise DomainError('invalid flat calibration')
    return out,valid
