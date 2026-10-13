from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import sample
from oracle.ops.filter.common.restoration import _flatten
from oracle.ops.filter.common.restoration import _matvec
from oracle.ops.filter.common.restoration import _reshape
from oracle.ops.filter.common.restoration import _transpose
from oracle.ops.filter.common.restoration import imaging_matrix
import copy

def simplex_projection(v):
    v=list(map(frac,v))
    if not v:raise DomainError('empty simplex')
    ordered=sorted(enumerate(v),key=lambda x:(-x[1],x[0]));total=Q();rho=0;theta=Q()
    for j,(_,value) in enumerate(ordered,1):
        total+=value;t=(total-1)/j
        if value>t:rho=j;theta=t
    if not rho:raise AssertionError('simplex projection has no active entry')
    return [max(x-theta,0) for x in v]

def blind_psf(observation,initial_image,initial_psf,support,*,iterations=1,step_image=0.1,step_psf=0.01,image_l2=0,psf_l2=0,anchor=(0,0),dtype='float64'):
    h,w=same_shape(observation,initial_image);kh,kw=same_shape(initial_psf,support)
    ex,ek,lx,lk=map(frac,(step_image,step_psf,image_l2,psf_l2))
    if min(ex,ek)<=0 or min(lx,lk)<0 or iterations<0:raise DomainError('blind parameters')
    if any(v not in (0,1) for row in support for v in row) or not any(v for row in support for v in row):raise DomainError('PSF support')
    if iterations==0:return copy.deepcopy(initial_image),[[rn(v,dtype) for v in row] for row in initial_psf]
    y,x,k=_flatten(observation),_flatten(initial_image),_flatten(initial_psf);active=[i for i,v in enumerate(sum(support,[])) if v]
    if min(x+k)<0 or sum(k,Q())!=1 or any(k[i] for i in range(len(k)) if i not in active):raise DomainError('initial blind constraints')
    for _ in range(iterations):
        a=imaging_matrix(h,w,_reshape(k,kh,kw),anchor,'wrap');at=_transpose(a)
        r=[frac(rn(v-yy,dtype)) for v,yy in zip(_matvec(a,x),y)]
        x=[frac(rn(max(0,xx-ex*(g+lx*xx)),dtype)) for xx,g in zip(x,_matvec(at,r))]
        r=[frac(rn(v-yy,dtype)) for v,yy in zip(_matvec(a,x),y)]
        xx=_reshape(x,h,w);grad=[]
        for j in range(kh):
            for i in range(kw):
                g=sum((r[yy*w+xxi]*sample(xx,yy+anchor[0]-j,xxi+anchor[1]-i,'wrap') for yy in range(h) for xxi in range(w)),Q())+lk*k[j*kw+i]
                grad.append(frac(rn(g,dtype)))
        projected=simplex_projection([k[i]-ek*grad[i] for i in active]);k=[Q()]*(kh*kw)
        for i,value in zip(active,projected):k[i]=value # exact rational constrained state
    return _reshape([rn(v,dtype) for v in x],h,w),_reshape([rn(v,dtype) for v in k],kh,kw)
