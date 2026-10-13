"""Exact algebraic reduction + MPFR enclosure for tiny DFT/DCT fixtures.

Roots of unity are reduced modulo a cyclotomic polynomial over Q. This proves
exact spectral cancellations (including DC/constant and odd lengths) instead of
letting an interval containing true zero refine forever. Numerical evaluation
then encloses the reduced algebraic expression. Explicit caps are oracle-only.
"""

from __future__ import annotations
from fractions import Fraction as Q
from functools import lru_cache
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
import math

@lru_cache(None)
def cyclotomic(n):
    if not 1<=n<=2048:raise DomainError('oracle cyclotomic order cap 2048')
    p=[-1]+[0]*(n-1)+[1]
    for d in range(1,n):
        if n%d:continue
        divisor=list(cyclotomic(d));quot=[0]*(len(p)-len(divisor)+1);rem=p[:]
        for i in range(len(quot)-1,-1,-1):
            a=rem[i+len(divisor)-1];quot[i]=a
            for j,b in enumerate(divisor):rem[i+j]-=a*b
        if any(rem):raise AssertionError('nonexact cyclotomic division')
        p=quot
    return tuple(p)

def reduce_poly(p,n):
    phi=cyclotomic(n);a=list(p)
    while a and not a[-1]:a.pop()
    for i in range(len(a)-len(phi),-1,-1):
        v=a[i+len(phi)-1]
        if v:
            for j,q in enumerate(phi):a[i+j]-=v*q
    a=a[:len(phi)-1]
    while a and not a[-1]:a.pop()
    return a

def evaluate_real_poly(p,n,*,scale=Q(1),sqrt_scale=Q(1),dtype='float64'):
    p=reduce_poly(p,n);scale=frac(scale);sqrt_scale=frac(sqrt_scale)
    if not p or not scale:return 0.0
    if len(p)==1:
        a,b=math.isqrt(sqrt_scale.numerator),math.isqrt(sqrt_scale.denominator)
        if a*a==sqrt_scale.numerator and b*b==sqrt_scale.denominator:return rn(p[0]*scale*Q(a,b),dtype)
    return certify(lambda c:sum((c.cospi(Q(2*j,n))*q for j,q in enumerate(p) if q),c.q(0))*scale*c.q(sqrt_scale).sqrt(),dtype)
