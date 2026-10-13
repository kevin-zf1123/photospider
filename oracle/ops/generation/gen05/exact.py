from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import sqrt_fraction
import math


def linear_coordinate(p,start,end):
    p,a,b=[tuple(map(frac,v)) for v in (p,start,end)]
    d=[b[i]-a[i] for i in range(2)];den=sum(v*v for v in d)
    if not den: raise ValueError('coincident gradient endpoints')
    return sum((p[i]-a[i])*d[i] for i in range(2))/den


def radial_coordinate(p,center,radii,dtype='float64'):
    r=list(map(frac,radii))
    if min(r)<=0:raise ValueError('positive radii')
    return sqrt_fraction(sum(((frac(p[i])-frac(center[i]))/r[i])**2 for i in range(2)),dtype)


def norm_coordinate(p,center,radii,metric):
    r=list(map(frac,radii))
    if min(r)<=0:raise ValueError('positive radii')
    q=[abs((frac(p[i])-frac(center[i]))/r[i]) for i in range(2)]
    if metric=='l1':return sum(q,F(0))
    if metric=='linf':return max(q)
    raise ValueError('metric')


def quadratic_coefficients(p,c0,r0,c1,r1):
    q=[frac(p[i])-frac(c0[i]) for i in range(2)]
    d=[frac(c1[i])-frac(c0[i]) for i in range(2)]
    r0,s=frac(r0),frac(r1)-frac(r0)
    return sum(a*a for a in d)-s*s,-2*(sum(a*b for a,b in zip(q,d))+r0*s),sum(a*a for a in q)-r0*r0
linear_gradient=linear_coordinate
radial_gradient=radial_coordinate
norm_gradient=norm_coordinate


def two_circle_rational(p,c0,r0,c1,r1,no_solution='valid_zero'):
    """Exact rational-root subset; irrational roots explicitly unsupported."""
    r0,r1=frac(r0),frac(r1)
    if no_solution not in ('valid_zero','reject'):raise ValueError('no_solution')
    if min(r0,r1)<0 or (tuple(map(frac,c0))==tuple(map(frac,c1)) and r0==r1):
        raise ValueError('invalid circles')
    A,B,C=quadratic_coefficients(p,c0,r0,c1,r1);s=r1-r0
    def absent():
        if no_solution=='reject':raise ValueError('NoSolution')
        return F(0),0
    if A==B==C==0:
        return (-r0/s,1) if s<0 else absent()
    if A==0:
        if B==0:return absent()
        roots=[-C/B]
    else:
        disc=B*B-4*A*C
        if disc<0:return absent()
        n,d=math.isqrt(disc.numerator),math.isqrt(disc.denominator)
        if n*n!=disc.numerator or d*d!=disc.denominator:
            raise NotImplementedError('irrational root certification is not implemented')
        root=F(n,d);roots=[(-B-root)/(2*A),(-B+root)/(2*A)]
    valid=[t for t in roots if r0+t*s>=0]
    return (max(valid),1) if valid else absent()
