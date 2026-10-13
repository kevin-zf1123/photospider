from oracle.ops.filter.common.bounds import Interval
from oracle.ops.filter.common.bounds import atan2_q
from oracle.ops.filter.common.bounds import sqrt_q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import zeros

def magnitude(gx,gy,*,norm='l2',dtype='float64'):
    h,w=same_shape(gx,gy);out=zeros(h,w)
    if norm not in ('l1','l2'):raise DomainError('norm')
    for y in range(h):
        for x in range(w):
            a,b=frac(gx[y][x]),frac(gy[y][x])
            out[y][x]=sqrt_q(a*a+b*b,dtype) if norm=='l2' else rn(abs(a)+abs(b),dtype)
    return out

def orientation(gx,gy,*,unit='radian',dtype='float64'):
    h,w=same_shape(gx,gy);angle=zeros(h,w);valid=zeros(h,w,0)
    if unit not in ('radian','turn'):raise DomainError('unit')
    for y in range(h):
        for x in range(w):
            a,b=frac(gx[y][x]),frac(gy[y][x]);valid[y][x]=int(bool(a or b))
            angle[y][x]=atan2_q(b,a,dtype,turn=(unit=='turn'))
    return angle,valid

def _atan_interval(c,y,x):
    if not y:
        return c.pi() if x<0 else c.q(0)
    yy,xx=c.q(y),c.q(x)
    lo=[c.binary('atan2',a,b,c.DOWN) for a in (yy.lo,yy.hi) for b in (xx.lo,xx.hi)]
    hi=[c.binary('atan2',a,b,c.UP) for a in (yy.lo,yy.hi) for b in (xx.lo,xx.hi)]
    return Interval(c,c.lo(lo),c.hi(hi))
