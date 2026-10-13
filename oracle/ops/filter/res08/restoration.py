from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import solve_linear
from oracle.ops.filter.common.restoration import _flatten
from oracle.ops.filter.common.restoration import _gradient_matrix
from oracle.ops.filter.common.restoration import _matmul
from oracle.ops.filter.common.restoration import _matvec
from oracle.ops.filter.common.restoration import _reshape
from oracle.ops.filter.common.restoration import _transpose
from oracle.ops.filter.common.restoration import imaging_matrix

def _solve_psd(a,b,singular='error'):
    """Rational PSD solve; 'zero' selects exact minimum Euclidean norm solution."""
    if singular not in ('error','zero'):raise DomainError('singular policy')
    n=len(b);m=[list(row)+[b[i]] for i,row in enumerate(a)];piv=[];row=0
    for col in range(n):
        p=next((r for r in range(row,n) if m[r][col]),None)
        if p is None:continue
        m[row],m[p]=m[p],m[row];v=m[row][col];m[row]=[x/v for x in m[row]]
        for r in range(n):
            if r!=row and m[r][col]:
                v=m[r][col];m[r]=[x-v*y for x,y in zip(m[r],m[row])]
        piv.append(col);row+=1
    if any(not any(r[:n]) and r[n] for r in m):raise DomainError('inconsistent system')
    if row<n and singular=='error':raise DomainError('singular frequency system')
    x=[Q()]*n
    for r,c in enumerate(piv):x[c]=m[r][n]
    free=[j for j in range(n) if j not in piv]
    if free:
        null=[]
        for j in free:
            v=[Q()]*n;v[j]=1
            for r,c in enumerate(piv):v[c]=-m[r][j]
            null.append(v)
        gram=[[sum((a*b for a,b in zip(u,v)),Q()) for v in null] for u in null]
        rhs=[-sum((a*b for a,b in zip(v,x)),Q()) for v in null]
        t=solve_linear(gram,rhs)
        x=[x[i]+sum((tt*v[i] for tt,v in zip(t,null)),Q()) for i in range(n)]
    return x

def tikhonov(observation,psf,*,lam,regularizer='gradient',dx=1,dy=1,anchor=(0,0),singular='error',dtype='float64'):
    h,w=shape(observation);n=h*w;lam,dx,dy=map(frac,(lam,dx,dy))
    if n>36 or lam<0 or min(dx,dy)<=0:raise DomainError('Tikhonov parameters/oracle cap')
    a=imaging_matrix(h,w,psf,anchor,'wrap');at=_transpose(a);normal=_matmul(at,a)
    if regularizer=='identity':r=[[Q(int(i==j)) for j in range(n)] for i in range(n)]
    elif regularizer in ('gradient','laplacian'):
        g=_gradient_matrix(h,w,dx,dy,True);r=_matmul(_transpose(g),g)
        if regularizer=='laplacian':r=_matmul(r,r)
    else:raise DomainError('regularizer')
    normal=[[normal[i][j]+lam*r[i][j] for j in range(n)] for i in range(n)]
    x=_solve_psd(normal,_matvec(at,_flatten(observation)),singular)
    return _reshape([rn(v,dtype) for v in x],h,w)

def wiener(observation,psf,*,nsr,anchor=(0,0),singular='error',dtype='float64'):
    return tikhonov(observation,psf,lam=nsr,regularizer='identity',anchor=anchor,singular=singular,dtype=dtype)
