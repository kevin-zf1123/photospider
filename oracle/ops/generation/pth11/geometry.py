from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
from oracle.ops.generation.common.exact import sqrt_fraction
from oracle.ops.generation.common.geometry import norm2
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.common.geometry import point_inside
from oracle.ops.generation.common.geometry import point_segment_distance2
from oracle.ops.generation.common.geometry import sub
from oracle.ops.generation.pth12.geometry import boolean_boundary
from oracle.ops.generation.pth12.geometry import boundary_contours


def region_boundary(contours,fill_rule='nonzero'):
    return boolean_boundary(contours,[],op='union',fill_rule_a=fill_rule)


def closest_polyline(p,contours):
    p=point(p);candidates=[]
    for si,contour in enumerate(contours):
        pp=list(map(point,contour))
        if len(pp)==1:candidates.append((norm2(sub(p,pp[0])),si,0,F(0),pp[0]))
        for j,(a,b) in enumerate(zip(pp,pp[1:])):
            q,t,c=point_segment_distance2(p,a,b);candidates.append((q,si,j,t,c))
    if not candidates:raise ValueError('NoSolution')
    return min(candidates,key=lambda v:v[:4])


def region_sdf(p,contours,fill_rule='nonzero',max_distance=None,dtype='float64'):
    boundary=region_boundary(contours,fill_rule)
    if not boundary:
        if max_distance is None:raise ValueError('NoSolution: empty regularized region')
        return round_fraction(frac(max_distance),dtype)
    p=point(p);d2=min(point_segment_distance2(p,a,b)[0] for a,b in boundary)
    if not d2:return 0.0
    # Use the exposed boundary, not raw source edges: a query on an internal
    # shared source edge can still be strictly inside the regularized union.
    sign=-1 if point_inside(p,boundary_contours(boundary),'nonzero',False) else 1
    if max_distance is not None and d2>=frac(max_distance)**2:return round_fraction(sign*frac(max_distance),dtype)
    return sign*sqrt_fraction(d2,dtype)
