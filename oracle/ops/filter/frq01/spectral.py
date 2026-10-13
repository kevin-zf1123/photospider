from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.common.spectral import evaluate_real_poly
import math

def dft2(real,imag=None,*,inverse=False,norm='backward',dtype='float64'):
    h,w=shape(real)
    if h*w>64:raise DomainError('DFT oracle cap is 64 pixels, not a runtime limit')
    if imag is None:imag=zeros(h,w)
    same_shape(real,imag)
    if norm not in ('backward','ortho'):raise DomainError('DFT norm')
    rr=[[frac(v) for v in row] for row in real];ii=[[frac(v) for v in row] for row in imag]
    n=math.lcm(h,w,4);pr=zeros(h,w);pi=zeros(h,w)
    scale=Q(1,h*w) if inverse and norm=='backward' else Q(1)
    sq=Q(1,h*w) if norm=='ortho' else Q(1)
    sign=1 if inverse else -1
    for ky in range(h):
        for kx in range(w):
            poly=[Q() for _ in range(n)]
            for y in range(h):
                for x in range(w):
                    e=sign*(ky*y*(n//h)+kx*x*(n//w))%n
                    poly[e]+=rr[y][x];poly[(e+n//4)%n]+=ii[y][x]
            re=[Q() for _ in range(n)];im=[Q() for _ in range(n)]
            for e,a in enumerate(poly):
                re[e]+=a/2;re[-e%n]+=a/2
                im[(e-n//4)%n]+=a/2;im[(-e-n//4)%n]-=a/2
            pr[ky][kx]=evaluate_real_poly(re,n,scale=scale,sqrt_scale=sq,dtype=dtype)
            pi[ky][kx]=evaluate_real_poly(im,n,scale=scale,sqrt_scale=sq,dtype=dtype)
    return pr,pi
