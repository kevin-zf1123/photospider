from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
from oracle.ops.generation.common.exact import sqrt_fraction
from oracle.ops.generation.common.geometry import add
from oracle.ops.generation.common.geometry import mul
from oracle.ops.generation.common.geometry import norm2
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.common.geometry import sub
import math


def bezier_bernstein(controls,t):
    p=list(map(point,controls));t=frac(t);d=len(p)-1
    if d not in (1,2,3) or not 0<=t<=1:raise ValueError('Bezier degree/t')
    return tuple(sum(F(math.comb(d,i))*(1-t)**(d-i)*t**i*p[i][c] for i in range(d+1)) for c in range(2))


def bezier_casteljau(controls,t):
    work=list(map(point,controls));t=frac(t)
    if not work or not 0<=t<=1:raise ValueError('Bezier/t')
    while len(work)>1:work=[add(mul(a,1-t),mul(b,t)) for a,b in zip(work,work[1:])]
    return work[0]


def bezier_derivative(controls,t):
    p=list(map(point,controls));d=len(p)-1
    if d<1:raise ValueError('degree')
    return bezier_casteljau([mul(sub(b,a),d) for a,b in zip(p,p[1:])],t)


def rounded_bezier_position(controls,t,dtype='float64'):
    t=frac(t)
    if t in (0,1):
        p=controls[0 if t==0 else -1]
        return tuple(round_fraction(frac(v),dtype, isinstance(v,float) and math.copysign(1,v)<0 and v==0) for v in p)
    q=bezier_bernstein(controls,t)
    return tuple(round_fraction(v,dtype,all(isinstance(p[c],float) and p[c]==0 and math.copysign(1,p[c])<0 for p in controls)) for c,v in enumerate(q))


def unit_tangent_fixture(derivative,dtype='float64'):
    d=point(derivative);q=norm2(d)
    if not q:return (0.0,0.0),0
    # d/sqrt(q) = sign(d)*sqrt(d^2/q); exact midpoint sqrt routine.
    return tuple(math.copysign(sqrt_fraction(v*v/q,dtype),float(v)) if v else 0.0 for v in d),1


def hermite(data,t,derivative=False):
    if len(data)!=4:raise ValueError('P0,P1,d0,d1')
    p0,p1,d0,d1=map(point,data);t=frac(t)
    if not 0<=t<=1:raise ValueError('t')
    h=(6*t*t-6*t,-6*t*t+6*t,3*t*t-4*t+1,3*t*t-2*t) if derivative else (2*t**3-3*t*t+1,-2*t**3+3*t*t,t**3-2*t*t+t,t**3-t*t)
    return tuple(sum(a*v[c] for a,v in zip(h,(p0,p1,d0,d1))) for c in range(2))


def bspline_fraction(controls,knots,degree,t,weights=None,derivative=False):
    p=list(map(point,controls));k=list(map(frac,knots));t=frac(t);n=len(p)
    if not 0<=degree<=16 or n<degree+1 or len(k)!=n+degree+1 or any(a>b for a,b in zip(k,k[1:])) or not 0<=t<=1:
        raise ValueError('spline dimensions/domain')
    a,b=k[degree],k[n]
    if a>=b:raise ValueError('empty knot interval')
    w=[F(1)]*n if weights is None else list(map(frac,weights))
    if len(w)!=n or min(w)<=0:raise ValueError('positive weights')
    u=a+t*(b-a);left_limit=t==1
    cache={}
    def basis(i,d):
        if i<0 or i+d+1>=len(k):return F(0)
        key=(i,d)
        if key in cache:return cache[key]
        if d==0:
            ans=F(int(k[i]<u<=k[i+1] if left_limit else k[i]<=u<k[i+1]))
        else:
            den0,den1=k[i+d]-k[i],k[i+d+1]-k[i+1]
            ans=((u-k[i])*basis(i,d-1)/den0 if den0 else 0)+((k[i+d+1]-u)*basis(i+1,d-1)/den1 if den1 else 0)
        cache[key]=ans;return ans
    nb=[basis(i,degree) for i in range(n)]
    den=sum(z*v for z,v in zip(nb,w));num=[sum(z*v*c[j] for z,v,c in zip(nb,w,p)) for j in range(2)]
    if not den:raise ValueError('zero rational denominator')
    if not derivative:return tuple(x/den for x in num)
    db=[]
    for i in range(n):
        if degree==0:db.append(F(0));continue
        d0,d1=k[i+degree]-k[i],k[i+degree+1]-k[i+1]
        db.append((degree*basis(i,degree-1)/d0 if d0 else 0)-(degree*basis(i+1,degree-1)/d1 if d1 else 0))
    dd=sum(z*v for z,v in zip(db,w));dn=[sum(z*v*c[j] for z,v,c in zip(db,w,p)) for j in range(2)]
    return tuple((dn[j]*den-num[j]*dd)/(den*den)*(b-a) for j in range(2))
