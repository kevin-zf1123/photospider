from __future__ import annotations
from oracle.ops.generation.common.geometry import point

def rectangle_sdf(p,center,half_size,dtype='float64'):
    from oracle.ops.generation.gen03.exact import rectangle_sdf as by_bounds
    c,h=point(center),point(half_size)
    if min(h)<=0:raise ValueError('positive half size')
    return by_bounds(p,(c[0]-h[0],c[1]-h[1],c[0]+h[0],c[1]+h[1]),dtype)
