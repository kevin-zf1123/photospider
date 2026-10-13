from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.common.spectral import evaluate_real_poly
import math

def _dct_entry(n,k,j,kind,norm,inverse):
    if norm=='ortho':
        if inverse:k,j=j,k
        if kind=='I':return Q(k*j,n-1),Q(1),int(k in (0,n-1))+int(j in (0,n-1))
        if kind=='II':return Q(k*(2*j+1),2*n),Q(1),int(k==0)
        if kind=='III':return Q(j*(2*k+1),2*n),Q(1),int(j==0)
        return Q((2*k+1)*(2*j+1),4*n),Q(1),0
    t={'I':'I','II':'III','III':'II','IV':'IV'}[kind] if inverse else kind
    if t=='I':return Q(k*j,n-1),Q(1 if j in (0,n-1) else 2),0
    if t=='II':return Q(k*(2*j+1),2*n),Q(2),0
    if t=='III':return Q(j*(2*k+1),2*n),Q(1 if j==0 else 2),0
    return Q((2*k+1)*(2*j+1),4*n),Q(2),0

def dct2(image,*,kind='II',norm='backward',inverse=False,dtype='float64'):
    h,w=shape(image)
    if h*w>36:raise DomainError('DCT oracle cap 36 pixels')
    if kind not in ('I','II','III','IV') or norm not in ('backward','ortho'):raise DomainError('DCT profile')
    if kind=='I' and min(h,w)<2:raise DomainError('DCT-I length')
    image=[[frac(v) for v in row] for row in image];out=zeros(h,w)
    sqrt_scale=Q(4,(h-1)*(w-1) if kind=='I' else h*w) if norm=='ortho' else Q(1)
    scale=Q(1,4*(h-1)*(w-1) if kind=='I' else 4*h*w) if inverse and norm=='backward' else Q(1)
    for ky in range(h):
        for kx in range(w):
            terms=[]
            for y in range(h):
                for x in range(w):
                    ph1,a,s1=_dct_entry(h,ky,y,kind,norm,inverse);ph2,b,s2=_dct_entry(w,kx,x,kind,norm,inverse)
                    s=s1+s2;v=image[y][x]*a*b/(2*(2**(s//2)))
                    terms.extend([(ph1+ph2,v,s%2),(ph1-ph2,v,s%2)])
            n=math.lcm(8,*[2*p.denominator for p,_,_ in terms]);poly=[Q() for _ in range(n)]
            if n>2048:raise DomainError('DCT root order cap')
            for phase,v,odd in terms:
                e=int(phase*n/2)%n
                if odd:
                    for a in (e,-e):
                        for b in (n//8,-n//8):poly[(a+b)%n]+=v/4
                else:
                    poly[e]+=v/2;poly[-e%n]+=v/2
            out[ky][kx]=evaluate_real_poly(poly,n,scale=scale,sqrt_scale=sqrt_scale,dtype=dtype)
    return out
