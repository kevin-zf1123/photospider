from __future__ import annotations
from oracle.ops.generation.common.exact import sqrt_fraction
from oracle.ops.generation.common.geometry import norm2
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.common.geometry import sub


def nearest_points(p,points,ids=None,dtype='float64'):
    ids=list(range(len(points))) if ids is None else list(ids)
    if len(points)<2:raise ValueError('NoSolution: at least two points required')
    if len(ids)!=len(points) or len(set(ids))!=len(ids):raise ValueError('unique ID map')
    p=point(p)
    ds=sorted((norm2(sub(p,point(v))),i) for v,i in zip(points,ids))
    return sqrt_fraction(ds[0][0],dtype),sqrt_fraction(ds[1][0],dtype),ds[0][1]
