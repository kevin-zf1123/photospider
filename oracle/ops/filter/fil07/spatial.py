from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import solve_linear
from oracle.ops.filter.common.core import zeros

def clipped_box(y,x,h,w,ry,rx):
    return [(j,i) for j in range(max(0,y-ry),min(h,y+ry+1)) for i in range(max(0,x-rx),min(w,x+rx+1))]

def guided(image,guide,radius_y=1,radius_x=1,*,epsilon=0.01,metric_scale=None,dtype='float64'):
    h,w=same_shape(image,guide);epsilon=frac(epsilon)
    if epsilon<=0 or min(radius_y,radius_x)<0:raise DomainError('guided parameters')
    vector=isinstance(guide[0][0],(list,tuple));gdim=len(guide[0][0]) if vector else 1
    if not 1<=gdim<=4:raise DomainError('guide components')
    scales=[Q(1)]*gdim if metric_scale is None else list(map(frac,metric_scale))
    if len(scales)!=gdim or min(scales)<=0:raise DomainError('metric scale')
    g=[[[frac(guide[y][x][c] if vector else guide[y][x])*scales[c] for c in range(gdim)] for x in range(w)] for y in range(h)]
    aa=[[None]*w for _ in range(h)];bb=zeros(h,w)
    for y in range(h):
        for x in range(w):
            pts=clipped_box(y,x,h,w,radius_y,radius_x);n=len(pts)
            mg=[sum((g[j][i][c] for j,i in pts),Q())/n for c in range(gdim)]
            mp=sum((frac(image[j][i]) for j,i in pts),Q())/n
            cov=[[sum((g[j][i][a]*g[j][i][b] for j,i in pts),Q())/n-mg[a]*mg[b]+(epsilon if a==b else 0) for b in range(gdim)] for a in range(gdim)]
            cg=[sum((g[j][i][c]*frac(image[j][i]) for j,i in pts),Q())/n-mg[c]*mp for c in range(gdim)]
            a=[frac(rn(v)) for v in solve_linear(cov,cg)]
            aa[y][x]=a;bb[y][x]=frac(rn(mp-sum((u*v for u,v in zip(a,mg)),Q())))
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            pts=clipped_box(y,x,h,w,radius_y,radius_x);n=len(pts)
            ma=[sum((aa[j][i][c] for j,i in pts),Q())/n for c in range(gdim)]
            mb=sum((bb[j][i] for j,i in pts),Q())/n
            out[y][x]=rn(sum((a*b for a,b in zip(ma,g[y][x])),Q())+mb,dtype)
    return out
