"""Independent tiny-image restoration references.

Rational solvers/explicit RN stages are exact references. Only the two explicitly
marked mean-domain inverse routines use mpmath diagnostics without proof of
truncation/integration error. No BM3D/SMAA engine is substituted here.
"""

from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import coordinate
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import shape

def _flatten(a):return [frac(v) for row in a for v in row]
def _reshape(v,h,w):return [v[y*w:(y+1)*w] for y in range(h)]
def _matvec(a,x):return [sum((q*v for q,v in zip(row,x)),Q()) for row in a]
def _transpose(a):return [list(row) for row in zip(*a)]
def _matmul(a,b):
    bt=_transpose(b)
    return [[sum((x*y for x,y in zip(row,col)),Q()) for col in bt] for row in a]

def imaging_matrix(h,w,kernel,anchor=(0,0),boundary='constant'):
    if h*w>64:raise DomainError('matrix oracle cap 64 pixels')
    kh,kw=shape(kernel);ay,ax=anchor
    if boundary not in ('constant','wrap') or not(0<=ay<kh and 0<=ax<kw):raise DomainError('imaging geometry')
    k=[[frac(v) for v in row] for row in kernel]
    if any(v<0 for row in k for v in row):raise DomainError('negative PSF')
    total=sum((sum(row,Q()) for row in k),Q())
    if total<=0:raise DomainError('empty PSF')
    a=[[Q() for _ in range(h*w)] for _ in range(h*w)]
    for y in range(h):
        for x in range(w):
            for j in range(kh):
                for i in range(kw):
                    if not k[j][i]:continue
                    p=coordinate(y+ay-j,x+ax-i,h,w,boundary)
                    if p is not None:a[y*w+x][p[0]*w+p[1]]+=k[j][i]/total
    return a

def _gradient_matrix(h,w,dx=1,dy=1,periodic=False):
    dx,dy=frac(dx),frac(dy);n=h*w;k=[[Q() for _ in range(n)] for _ in range(2*n)]
    for y in range(h):
        for x in range(w):
            i=y*w+x
            if y+1<h or periodic:
                k[i][((y+1)%h)*w+x]+=1/dy;k[i][i]-=1/dy
            if x+1<w or periodic:
                k[n+i][y*w+(x+1)%w]+=1/dx;k[n+i][i]-=1/dx
    return k
