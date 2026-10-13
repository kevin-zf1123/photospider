from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import from_bits
from oracle.ops.generation.common.exact import round_bits
from oracle.ops.generation.common.exact import round_fraction
import math


def spread(t,method):
    t=frac(t)
    if method=='pad':return min(max(t,F(0)),F(1))
    if method=='repeat':return t-t//1
    if method=='reflect':return 1-abs(1-t%2)
    raise ValueError('unknown spread')


def spread_rounded(t,method,dtype='float64'):
    if method=='pad' and isinstance(t,float) and t==0:
        return round_fraction(0,dtype,math.copysign(1,t)<0)
    q=spread(t,method);b=round_bits(q,dtype)
    return from_bits(b,dtype)


def numeric_lookup(t,table):
    t=frac(t)
    if len(table)<2 or not 0<=t<=1 or not table[0] or any(len(r)!=len(table[0]) for r in table):
        raise ValueError('invalid lookup')
    x=t*(len(table)-1);j=x//1
    if x==j:return list(map(frac,table[j]))
    w=x-j
    return [(1-w)*frac(a)+w*frac(b) for a,b in zip(table[j],table[j+1])]


def linear_rgba_mix(a,b,t):
    """Linear-light straight RGB subset; transparent black overrides copy."""
    t=frac(t)
    if len(a)!=4 or len(b)!=4 or not 0<=t<=1:raise ValueError('RGBA/t')
    aa,bb=list(map(frac,a)),list(map(frac,b))
    if not (0<=aa[3]<=1 and 0<=bb[3]<=1):raise ValueError('alpha range')
    if t==0 or t==1:
        selected=a if t==0 else b
        return list(selected) if frac(selected[3]) else [F(0)]*4
    alpha=(1-t)*aa[3]+t*bb[3]
    if not alpha:return [F(0)]*4
    # Identical-color selection preserves source zero signs as the CRV contract requires.
    if aa==bb and all((math.copysign(1,x) if isinstance(x,float) else 1)==
                      (math.copysign(1,y) if isinstance(y,float) else 1)
                      for x,y,q in zip(a,b,aa) if q==0):
        return list(a)
    rgb=[((1-t)*aa[3]*aa[i]+t*bb[3]*bb[i])/alpha for i in range(3)]
    return rgb+[alpha]
