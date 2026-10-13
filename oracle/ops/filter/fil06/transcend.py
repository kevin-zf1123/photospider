from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import exp_q
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import coordinate
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import shape
from oracle.ops.filter.common.core import zeros

def bilateral(image,guide=None,*,radius_y=1,radius_x=1,sigma_y=1,sigma_x=1,sigma_range=1,
              metric_scale=None,boundary='reflect_half',dtype='float64'):
    h,w=shape(image)
    if guide is None:guide=image
    same_shape(image,guide)
    sy,sx,sr=map(frac,(sigma_y,sigma_x,sigma_range))
    if min(sy,sx,sr)<=0 or min(radius_y,radius_x)<0:raise DomainError('bilateral parameters')
    if boundary not in ('reflect_half','reflect_whole','clamp','wrap','truncate'):raise DomainError('boundary')
    if radius_y==0 and radius_x==0:return [list(row) for row in image]
    vector=isinstance(guide[0][0],(list,tuple));gdim=len(guide[0][0]) if vector else 1
    if not 1<=gdim<=4:raise DomainError('guide components')
    scales=[Q(1)]*gdim if metric_scale is None else list(map(frac,metric_scale))
    if len(scales)!=gdim or min(scales)<=0:raise DomainError('metric_scale')
    def g(y,x):return [frac(guide[y][x][c] if vector else guide[y][x])*scales[c] for c in range(gdim)]
    spatial={(dy,dx):frac(exp_q(-Q(dy*dy)/(2*sy*sy)-Q(dx*dx)/(2*sx*sx))) for dy in range(-radius_y,radius_y+1) for dx in range(-radius_x,radius_x+1)}
    out=zeros(h,w)
    for y in range(h):
        for x in range(w):
            center=g(y,x);num=den=Q()
            for (dy,dx),ws in spatial.items():
                if not ws:continue
                p=coordinate(y+dy,x+dx,h,w,boundary)
                if p is None:continue
                other=g(*p);distance=sum(((a-b)**2 for a,b in zip(center,other)),Q())
                weight=ws*frac(exp_q(-distance/(2*sr*sr)))
                if weight:num+=weight*frac(image[p[0]][p[1]]);den+=weight
            out[y][x]=rn(num/den,dtype)
    return out
