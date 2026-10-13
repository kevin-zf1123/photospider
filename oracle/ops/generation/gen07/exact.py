from __future__ import annotations
from oracle.ops.generation.common.exact import frac
import math


def bilinear(colors,u,v):
    u,v=frac(u),frac(v)
    if not (0<=u<=1 and 0<=v<=1):raise ValueError('uv')
    return [sum(w*frac(colors[j][i][c]) for j,i,w in
                ((0,0,(1-u)*(1-v)),(0,1,u*(1-v)),(1,0,(1-u)*v),(1,1,u*v)))
            for c in range(len(colors[0][0]))]


def barycentric(p,triangle):
    def cross(a,b):return a[0]*b[1]-a[1]*b[0]
    a,b,c=[tuple(map(frac,v)) for v in triangle];p=tuple(map(frac,p))
    v0=(b[0]-a[0],b[1]-a[1]);v1=(c[0]-a[0],c[1]-a[1]);v2=(p[0]-a[0],p[1]-a[1])
    den=cross(v0,v1)
    if not den:raise ValueError('degenerate triangle')
    v,w=cross(v2,v1)/den,cross(v0,v2)/den
    return 1-v-w,v,w


def bicubic_patch(colors,u,v):
    u,v=frac(u),frac(v)
    if not(0<=u<=1 and 0<=v<=1):raise ValueError('uv')
    bu=[math.comb(3,i)*u**i*(1-u)**(3-i) for i in range(4)]
    bv=[math.comb(3,j)*v**j*(1-v)**(3-j) for j in range(4)]
    return [sum(bu[i]*bv[j]*frac(colors[j][i][c]) for j in range(4) for i in range(4)) for c in range(len(colors[0][0]))]
