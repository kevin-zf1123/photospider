from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.restoration import _flatten
from oracle.ops.filter.common.restoration import _matvec
from oracle.ops.filter.common.restoration import _reshape
from oracle.ops.filter.common.restoration import _transpose
from oracle.ops.filter.common.restoration import imaging_matrix
import copy

def forward_adjoint(image,kernel,anchor=(0,0),*,probe=None,boundary='constant'):
    h,w=shape(image);a=imaging_matrix(h,w,kernel,anchor,boundary);x=_flatten(image)
    result={'matrix':a,'forward':_reshape(_matvec(a,x),h,w)}
    if probe is not None:
        same_shape(image,probe);z=_flatten(probe)
        result['adjoint']=_reshape(_matvec(_transpose(a),z),h,w)
        result['dot_forward']=sum((u*v for u,v in zip(_matvec(a,x),z)),Q())
        result['dot_adjoint']=sum((u*v for u,v in zip(x,_matvec(_transpose(a),z))),Q())
    return result

def rl(observation,psf,initial,background,mask,*,iterations=1,epsilon=0,anchor=(0,0),boundary='constant',dtype='float64',request_estimate=True,request_unobserved=True):
    h,w=same_shape(observation,initial,background,mask);eps=frac(epsilon)
    if iterations<0 or eps<0:raise DomainError('RL iterations/epsilon')
    if iterations==0 and not request_unobserved:return copy.deepcopy(initial),None
    a=imaging_matrix(h,w,psf,anchor,boundary);at=_transpose(a);m=_flatten(mask)
    if any(not 0<=v<=1 for v in m):raise DomainError('mask range')
    sensitivity=_matvec(at,m);unobserved=_reshape([int(v==0) for v in sensitivity],h,w)
    if not request_estimate:return None,unobserved
    if iterations==0:return copy.deepcopy(initial),unobserved
    x=_flatten(initial);b=_flatten(background)
    if any(v<0 for v in x+b):raise DomainError('negative RL initial/background')
    raw_y=[v for row in observation for v in row];y=[frac(v) if mm else Q() for v,mm in zip(raw_y,m)]
    if any(v<0 for v in y):raise DomainError('negative observed sample')
    for _ in range(iterations):
        v=[frac(rn(u+bb,dtype)) for u,bb in zip(_matvec(a,x),b)]
        r=[]
        for yy,mm,vv in zip(y,m,v):
            if not mm:r.append(Q());continue
            den=vv+eps
            if not den:
                if yy:raise DomainError('positive observation with zero prediction')
                r.append(Q())
            else:r.append(frac(rn(mm*yy/den,dtype)))
        t=[frac(rn(v,dtype)) for v in _matvec(at,r)]
        x=[frac(rn(xx*tt/ss,dtype)) if ss else xx for xx,tt,ss in zip(x,t,sensitivity)]
    return _reshape([rn(v,dtype) for v in x],h,w),(unobserved if request_unobserved else None)
