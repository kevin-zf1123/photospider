from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import exp_q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.restoration import _flatten
from oracle.ops.filter.common.restoration import _reshape
import copy

def diffusion(image,*,kappa,dt=Q(1,4),steps=1,dx=1,dy=1,exponential=False,dtype='float64'):
    h,w=shape(image);kap,dt,dx,dy=map(frac,(kappa,dt,dx,dy))
    if min(kap,dt,dx,dy)<=0 or steps<0 or dt>1/(2*(1/(dx*dx)+1/(dy*dy))):raise DomainError('diffusion parameters/stability')
    if not steps:return copy.deepcopy(image)
    u=_flatten(image)
    edges=[]
    for y in range(h):
        for x in range(w):
            i=y*w+x
            if x+1<w:edges.append((i,i+1,dx))
            if y+1<h:edges.append((i,i+w,dy))
    for _ in range(steps):
        flux=[Q()]*(h*w)
        for i,j,spacing in edges:
            ratio=((u[j]-u[i])/(spacing*kap))**2
            c=frac(exp_q(-ratio)) if exponential else frac(rn(1/(1+ratio)))
            f=c*(u[j]-u[i])/(spacing*spacing);flux[i]+=f;flux[j]-=f
        u=[frac(rn(v+dt*f,dtype)) for v,f in zip(u,flux)]
    return _reshape([rn(v,dtype) for v in u],h,w)
