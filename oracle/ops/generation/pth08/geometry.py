from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
from oracle.ops.generation.common.geometry import add
from oracle.ops.generation.common.geometry import mul
from oracle.ops.generation.common.geometry import norm2
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.common.geometry import sub
from oracle.ops.generation.pth04.geometry import _polyline_lengths


def simplify_polyline(points,epsilon,locked=()):
    pp,lengths,L=_polyline_lengths(points);epsilon=frac(epsilon)
    if epsilon<0:raise ValueError('epsilon')
    if len(pp)<3:return pp
    fixed={0,len(pp)-1,*locked}
    if any(i<0 or i>=len(pp) for i in fixed):raise ValueError('locked index')
    cumulative=[F(0)]
    for z in lengths:cumulative.append(cumulative[-1]+z)
    def recurse(a,b):
        locks=sorted(i for i in fixed if a<i<b)
        if locks:
            idx=locks[0];return recurse(a,idx)[:-1]+recurse(idx,b)
        if b<=a+1:return [a,b]
        width=cumulative[b]-cumulative[a]
        values=[]
        for i in range(a+1,b):
            t=(cumulative[i]-cumulative[a])/width if width else F(0)
            q=add(mul(pp[a],1-t),mul(pp[b],t))
            values.append((norm2(sub(pp[i],q)),i))
        err,idx=max(values,key=lambda x:(x[0],-x[1]))
        if err<=epsilon*epsilon:return [a,b]
        return recurse(a,idx)[:-1]+recurse(idx,b)
    return [pp[i] for i in recurse(0,len(pp)-1)]


def cubic_candidate_checks(controls,source_endpoints):
    # Only an analytic necessary check, NOT a Hausdorff certifier/fitter.
    return len(controls)==4 and point(controls[0])==point(source_endpoints[0]) and point(controls[-1])==point(source_endpoints[-1])


def chaikin(points,iterations=1,closed=False):
    if not 0<=iterations<=12:raise ValueError('iterations')
    current=[tuple(v) for v in points]
    for _ in range(iterations):
        if len(current)<2:break
        edges=list(zip(current,current[1:]))+([(current[-1],current[0])] if closed else [])
        out=[] if closed else [current[0]]
        for a,b in edges:
            a,b=point(a),point(b)
            out.extend(tuple(round_fraction(v) for v in q) for q in (mul(add(mul(a,3),b),F(1,4)),mul(add(a,mul(b,3)),F(1,4))))
        if not closed:out.append(current[-1])
        current=out
    return current
