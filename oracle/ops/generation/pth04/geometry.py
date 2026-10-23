from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
from oracle.ops.generation.common.geometry import add
from oracle.ops.generation.common.geometry import mul
from oracle.ops.generation.common.geometry import norm2
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.common.geometry import sub
from oracle.ops.generation.pth02.geometry import bezier_bernstein
from oracle.ops.generation.common.geometry import split_bezier
import math


def _sqrt_sum_le(a,b,e):
    """Exact test sqrt(a)+sqrt(b)<=e, for nonnegative rationals."""
    if e<0:return False
    z=e*e-a-b
    return z>=0 and z*z>=4*a*b


def flatten_bezier(controls,epsilon=F(1,20),max_depth=24,max_segments=1048576):
    p=list(map(point,controls));d=len(p)-1;epsilon=frac(epsilon)
    if d not in (1,2,3) or epsilon<=0:raise ValueError('flatten input')
    leaves=[]
    def visit(cp,t0,t1,depth):
        target=[add(mul(cp[0],F(d-i,d)),mul(cp[-1],F(i,d))) for i in range(d+1)]
        deviation2=max(norm2(sub(a,b)) for a,b in zip(cp,target))
        published=[point(tuple(round_fraction(v) for v in cp[i])) for i in (0,-1)]
        rounding2=max(norm2(sub(a,b)) for a,b in zip((cp[0],cp[-1]),published))
        if _sqrt_sum_le(deviation2,rounding2,epsilon):
            if len(leaves)>=max_segments:raise OverflowError('CapacityLimit')
            leaves.append({'t0':t0,'t1':t1,'controls':cp,'a':published[0],'b':published[1],
                           'deviation2':deviation2,'rounding2':rounding2,'epsilon':epsilon})
        else:
            if depth>=max_depth:raise ArithmeticError('NotConverged: flatten depth')
            left,right=split_bezier(cp);mid=(t0+t1)/2
            visit(left,t0,mid,depth+1);visit(right,mid,t1,depth+1)
    visit(p,F(0),F(1),0)
    return [leaves[0]['a']]+[x['b'] for x in leaves],leaves


def verify_flatten_certificate(leaves):
    if not leaves:return False
    if leaves[0]['t0']!=0 or leaves[-1]['t1']!=1:return False
    for i,leaf in enumerate(leaves):
        cp=leaf['controls'];d=len(cp)-1
        dev=max(norm2(sub(cp[j],add(mul(cp[0],F(d-j,d)),mul(cp[-1],F(j,d))))) for j in range(d+1))
        rnd=max(norm2(sub(cp[0],leaf['a'])),norm2(sub(cp[-1],leaf['b'])))
        if dev!=leaf['deviation2'] or rnd!=leaf['rounding2'] or not _sqrt_sum_le(dev,rnd,leaf['epsilon']):return False
        if i and (leaves[i-1]['t1']!=leaf['t0'] or leaves[i-1]['b']!=leaf['a']):return False
    return True


def uniform_t_samples(controls,count):
    if count<2:raise ValueError('at least two per-segment samples')
    return [(F(j,count-1),bezier_bernstein(controls,F(j,count-1))) for j in range(count)]


def _exact_segment_length(a,b):
    q=norm2(sub(point(b),point(a)));sn,sd=math.isqrt(q.numerator),math.isqrt(q.denominator)
    if sn*sn!=q.numerator or sd*sd!=q.denominator:
        raise NotImplementedError('this exact polyline helper requires rational segment lengths')
    return F(sn,sd)


def polyline_arc_relation(points,fraction,segment,local_t,closed=False):
    """Prove fraction*total = prefix(segment)+local_t*length(segment).

    All inputs are exact rationals. Segment lengths may be irrational square
    roots. This verifies a candidate relation; it does not choose junction
    ownership, invert arbitrary curves, or certify published IEEE coordinates.
    """
    pp=list(map(point,points))
    if closed and pp and pp[-1]!=pp[0]:pp.append(pp[0])
    f,t=frac(fraction),frac(local_t)
    if not 0<=f<=1 or not 0<=t<=1 or not 0<=segment<len(pp)-1:
        raise ValueError('arc relation input')
    groups=[]
    for i,(a,b) in enumerate(zip(pp,pp[1:])):
        q=norm2(sub(b,a));weight=f-int(i<segment)-(t if i==segment else 0)
        if not q or not weight:continue
        for group in groups:
            ratio=q/group[0]
            n,d=math.isqrt(ratio.numerator),math.isqrt(ratio.denominator)
            if n*n==ratio.numerator and d*d==ratio.denominator:
                group[1]+=weight*F(n,d)
                break
        else:groups.append([q,weight])
    # Square roots from distinct rational square classes are Q-linearly independent.
    return all(not weight for _,weight in groups)


def _polyline_lengths(points,closed=False):
    pp=list(map(point,points))
    if not pp:return pp,[],F(0)
    if closed and pp[-1]!=pp[0]:pp=pp+[pp[0]]
    lengths=[_exact_segment_length(a,b) for a,b in zip(pp,pp[1:])]
    return pp,lengths,sum(lengths,F(0))


def _at_length(pp,lengths,s):
    if not pp:raise ValueError('NoSolution: empty polyline')
    L=sum(lengths,F(0));s=frac(s)
    if not 0<=s<=L:raise ValueError('arc coordinate')
    if not L:return pp[0],0,F(0)
    cumulative=F(0)
    for i,z in enumerate(lengths):
        # Interior boundaries select the following positive-length segment.
        if z and (s<cumulative+z or s==L and cumulative+z==L):
            t=(s-cumulative)/z
            return add(mul(pp[i],1-t),mul(pp[i+1],t)),i,t
        cumulative+=z
    raise AssertionError('unreachable arc position')


def polyline_arc_samples(points,count,closed=False):
    if count<1:raise ValueError('count')
    pp,lengths,L=_polyline_lengths(points,closed)
    if not pp:return []
    ss=[F(0)] if count==1 else [F(j,count if closed else count-1)*L for j in range(count)]
    return [(s,*_at_length(pp,lengths,s)) for s in ss]


def polyline_spacing_samples(points,spacing,closed=False,include_open_end=True):
    delta=frac(spacing)
    if delta<=0:raise ValueError('spacing')
    pp,lengths,L=_polyline_lengths(points,closed)
    if not pp:return []
    if not L:return [(F(0),pp[0],0,F(0))]
    ss=[];s=F(0)
    while s<L:ss.append(s);s+=delta
    if not closed and include_open_end:ss.append(L)
    return [(s,*_at_length(pp,lengths,s)) for s in ss]
