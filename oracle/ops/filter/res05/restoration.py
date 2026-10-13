from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.restoration import _flatten
from oracle.ops.filter.common.restoration import _gradient_matrix
from oracle.ops.filter.common.restoration import _matvec
from oracle.ops.filter.common.restoration import _reshape
from oracle.ops.filter.common.restoration import _transpose
import copy

def tv_cp(image,*,lam,iterations=100,tau=0.24,sigma=0.24,theta=1,dx=1,dy=1,isotropic=True,dtype='float64'):
    h,w=shape(image);lam,tau,sigma,theta,dx,dy=map(frac,(lam,tau,sigma,theta,dx,dy))
    if lam<0 or min(tau,sigma,dx,dy)<=0 or not 0<=theta<=1 or iterations<0:raise DomainError('TV parameters')
    if tau*sigma*4*(1/(dx*dx)+1/(dy*dy))>=1:raise DomainError('TV step bound')
    if not lam or not iterations:return copy.deepcopy(image)
    if h*w>36:raise DomainError('TV exact matrix oracle cap 36 pixels')
    n=h*w;k=_gradient_matrix(h,w,dx,dy);kt=_transpose(k);f=_flatten(image);u=f[:];ubar=f[:];p=[Q()]*(2*n)
    for _ in range(iterations):
        ph=[frac(rn(a+sigma*b,dtype)) for a,b in zip(p,_matvec(k,ubar))]
        pn=[Q()]*(2*n)
        for i in range(n):
            a,b=ph[i],ph[n+i]
            if isotropic:
                norm2=a*a+b*b
                if norm2<=lam*lam:pn[i],pn[n+i]=a,b
                else:
                    pn[i]=Q() if not a else frac(certify(lambda c:c.q(lam*a)/c.q(norm2).sqrt(),dtype))
                    pn[n+i]=Q() if not b else frac(certify(lambda c:c.q(lam*b)/c.q(norm2).sqrt(),dtype))
            else:pn[i],pn[n+i]=frac(rn(min(max(a,-lam),lam),dtype)),frac(rn(min(max(b,-lam),lam),dtype))
        un=[frac(rn((a-tau*b+tau*ff)/(1+tau),dtype)) for a,b,ff in zip(u,_matvec(kt,pn),f)]
        ubar=[frac(rn(a+theta*(a-b),dtype)) for a,b in zip(un,u)];u,p=un,pn
    return _reshape([rn(v,dtype) for v in u],h,w)
