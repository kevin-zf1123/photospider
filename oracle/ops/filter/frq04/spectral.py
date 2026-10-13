from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape

def _window1(n,kind,sampling,alpha):
    if n<1:raise DomainError('window length')
    if n==1:return [1.0]
    den=n-1 if sampling=='symmetric' else n
    out=[]
    for i in range(n):
        t=Q(i,den)
        if kind=='rectangular' or (kind=='tukey' and alpha==0):v=1.0
        elif kind=='hann' or (kind=='tukey' and alpha==1):
            v=certify(lambda c:(1-c.cospi(2*t))/2)
        elif t<alpha/2:v=certify(lambda c:(1+c.cospi(2*t/alpha-1))/2)
        elif t<=1-alpha/2:v=1.0
        else:v=certify(lambda c:(1+c.cospi(2*t/alpha-2/alpha+1))/2)
        out.append(v)
    return out

def window(height,width,*,kind='hann',sampling='symmetric',alpha=None):
    if kind not in ('rectangular','hann','tukey') or sampling not in ('symmetric','periodic'):raise DomainError('window profile')
    if kind!='tukey' and alpha is not None:raise DomainError('unexpected alpha')
    a=Q(1,2) if alpha is None else frac(alpha)
    if not 0<=a<=1:raise DomainError('Tukey alpha')
    wy,wx=_window1(height,kind,sampling,a),_window1(width,kind,sampling,a)
    return [[rn(frac(y)*frac(x)) for x in wx] for y in wy]

def apply_window(image,win,*,dtype='float64'):
    h,w=same_shape(image,win)
    return [[rn(frac(image[y][x])*frac(win[y][x]),dtype) for x in range(w)] for y in range(h)]

def window_stats(win):
    h,w=shape(win);a=[frac(v) for row in win for v in row];n=h*w;s=sum(a,Q());s2=sum((v*v for v in a),Q())
    if not s:raise DomainError('zero coherent sum')
    return rn(s/n),rn(s2/n),rn(n*s2/(s*s))
