from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.geometry import dot
from oracle.ops.generation.common.geometry import norm2
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.common.geometry import sub


def straight_stroke_rectangles(a,b,width,cap='butt'):
    """Exact axis-aligned butt/square fixture; round/corner union not included."""
    a,b=point(a),point(b);r=frac(width)/2
    if r<0 or cap not in ('butt','square'):raise ValueError('fixture supports butt/square')
    if not r:return []
    if a==b:
        return [] if cap=='butt' else [[(a[0]-r,a[1]-r),(a[0]+r,a[1]-r),(a[0]+r,a[1]+r),(a[0]-r,a[1]+r)]]
    e=r if cap=='square' else F(0)
    if a[1]==b[1]:x0,x1=min(a[0],b[0])-e,max(a[0],b[0])+e;y0,y1=a[1]-r,a[1]+r
    elif a[0]==b[0]:x0,x1=a[0]-r,a[0]+r;y0,y1=min(a[1],b[1])-e,max(a[1],b[1])+e
    else:raise NotImplementedError('fixture is axis-aligned')
    return [[(x0,y0),(x1,y0),(x1,y1),(x0,y1)]]


def variable_sweep_contains(p,a,b,r0,r1):
    """Exact existential membership in linearly varying disc sweep.

    Minimize ||p-a-t(b-a)||^2-(r0+t(r1-r0))^2 on [0,1].
    """
    p,a,b=map(point,(p,a,b));r0,r1=frac(r0),frac(r1)
    if min(r0,r1)<0:raise ValueError('negative radius')
    v,q=sub(b,a),sub(p,a);dr=r1-r0
    A=norm2(v)-dr*dr;B=-2*(dot(q,v)+r0*dr);C=norm2(q)-r0*r0
    ts=[F(0),F(1)]
    if A>0 and 0<-B/(2*A)<1:ts.append(-B/(2*A))
    return min(A*t*t+B*t+C for t in ts)<=0
