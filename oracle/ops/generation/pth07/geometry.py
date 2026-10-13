from __future__ import annotations
from oracle.ops.generation.common.geometry import split_bezier
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.geometry import add
from oracle.ops.generation.common.geometry import mul
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.pth04.geometry import _at_length
from oracle.ops.generation.pth04.geometry import _polyline_lengths




def trim_bezier_t(controls,a,b):
    a,b=frac(a),frac(b)
    if not 0<=a<=b<=1:raise ValueError('trim interval')
    if a==b:return []
    if a==0 and b==1:return list(map(point,controls))
    left,_=split_bezier(controls,b)
    return split_bezier(left,a/b)[1] if a else left


def trim_polyline(points,start,end):
    pp,lengths,L=_polyline_lengths(points);a,b=frac(start),frac(end)
    if not 0<=a<=b<=L:raise ValueError('trim interval')
    if not pp or a==b:return []
    out=[_at_length(pp,lengths,a)[0]];s=F(0)
    for p,z in zip(pp[1:],lengths):
        s+=z
        if a<s<b:out.append(p)
    out.append(_at_length(pp,lengths,b)[0]);return out


def dash_intervals(length,pattern,offset=0):
    L=frac(length);p=list(map(frac,pattern))
    if L<0 or not p or min(p)<0 or sum(p)<=0:raise ValueError('dash domain')
    if len(p)%2:p=p+p
    phase=frac(offset)%sum(p);i=0
    while p[i]==0 or phase>=p[i]:
        if p[i]:phase-=p[i]
        i=(i+1)%len(p)
    remaining=p[i]-phase;s=F(0);out=[]
    while s<L:
        end=min(L,s+remaining)
        if i%2==0 and end>s:
            if out and out[-1][1]==s:out[-1]=(out[-1][0],end)
            else:out.append((s,end))
        s=end;i=(i+1)%len(p)
        while p[i]==0:i=(i+1)%len(p)
        remaining=p[i]
    return out


def dash_polyline(points,pattern,offset=0):
    _,_,L=_polyline_lengths(points)
    return [trim_polyline(points,a,b) for a,b in dash_intervals(L,pattern,offset)]
