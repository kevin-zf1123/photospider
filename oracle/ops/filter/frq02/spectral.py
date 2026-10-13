from __future__ import annotations
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.frq01.spectral import dft2

def rfft2(image,*,norm='backward',dtype='float64'):
    h,w=shape(image);real,imag=dft2(image,norm=norm,dtype=dtype)
    return [r[:w//2+1] for r in real],[r[:w//2+1] for r in imag]

def validate_half(real,imag,original_width):
    h,k=same_shape(real,imag);w=original_width
    if not isinstance(w,int) or w<=0 or k!=w//2+1:raise DomainError('original_width mismatch')
    columns=[0]+([w//2] if w%2==0 else [])
    for y in range(h):
        for x in range(k):frac(real[y][x]);frac(imag[y][x])
    for x in columns:
        for y in range(h):
            yy=(-y)%h
            if frac(real[y][x])!=frac(real[yy][x]) or frac(imag[y][x])!=-frac(imag[yy][x]):
                raise DomainError('non-Hermitian boundary column')
    return h,w

def irfft2(real,imag,original_width,*,norm='backward',dtype='float64'):
    h,w=validate_half(real,imag,original_width);k=w//2+1
    rr=zeros(h,w);ii=zeros(h,w)
    for y in range(h):
        for x in range(w):
            if x<k:rr[y][x]=real[y][x];ii[y][x]=imag[y][x]
            else:rr[y][x]=real[-y%h][w-x];ii[y][x]=-imag[-y%h][w-x]
    return dft2(rr,ii,inverse=True,norm=norm,dtype=dtype)[0]

def project_hermitian(real,imag,*,dtype='float64'):
    h,w=same_shape(real,imag);rr=zeros(h,w);ii=zeros(h,w);seen=set()
    for y in range(h):
        for x in range(w):
            if (y,x) in seen:continue
            yy,xx=-y%h,-x%w;seen|={(y,x),(yy,xx)}
            a,b,c,d=map(frac,(real[y][x],imag[y][x],real[yy][xx],imag[yy][xx]))
            if (y,x)==(yy,xx):
                rr[y][x]=real[y][x];ii[y][x]=0.0;continue
            re=rn((a+c)/2,dtype);im=rn((b-d)/2,dtype)
            rr[y][x]=rr[yy][xx]=re
            ii[y][x]=0.0 if im==0 else im;ii[yy][xx]=0.0 if im==0 else -im
    return rr,ii
