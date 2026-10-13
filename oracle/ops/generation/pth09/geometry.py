from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.geometry import _edges
from oracle.ops.generation.common.geometry import segment_intersections


def polygon_pixel_area(contours,pixel=(0,0),fill_rule='nonzero'):
    """Exact rational area via vertical slabs, not sample-count AA.

    Handles arbitrary finite polygon contours, overlap, holes, shared/collinear
    edges and self-crossings; all edge events are retained with exact predicates.
    """
    if fill_rule not in ('nonzero','evenodd'):raise ValueError('fill rule')
    edges=_edges(contours);x0,y0=map(frac,pixel);x1,y1=x0+1,y0+1
    xs={x0,x1}
    for a,b in edges:
        xs.update(x for x in (a[0],b[0]) if x0<x<x1)
        if a[1]!=b[1]:
            for y in (y0,y1):
                t=(y-a[1])/(b[1]-a[1])
                if 0<t<1:
                    x=a[0]+t*(b[0]-a[0])
                    if x0<x<x1:xs.add(x)
    for i,(a,b) in enumerate(edges):
        for c,d in edges[:i]:
            for p in segment_intersections(a,b,c,d):
                if x0<p[0]<x1:xs.add(p[0])
    xs=sorted(xs);area=F(0)
    def val(line,x):return line[0]*x+line[1]
    for xl,xr in zip(xs,xs[1:]):
        xm=(xl+xr)/2;groups={}
        for a,b in edges:
            if min(a[0],b[0])<xm<max(a[0],b[0]):
                m=(b[1]-a[1])/(b[0]-a[0]);line=(m,a[1]-m*a[0])
                groups[line]=groups.get(line,0)+(1 if b[0]>a[0] else -1)
        lines=sorted(groups,key=lambda line:val(line,xm));w=0;previous=None
        for line in lines:
            inside=(w!=0) if fill_rule=='nonzero' else (w%2!=0)
            if previous is not None and inside:
                low=previous if val(previous,xm)>y0 else (F(0),y0)
                high=line if val(line,xm)<y1 else (F(0),y1)
                if val(high,xm)>val(low,xm):
                    area+=(xr-xl)*(val(high,xl)-val(low,xl)+val(high,xr)-val(low,xr))/2
            w+=groups[line];previous=line
        if w:raise AssertionError('closed contours must have zero final winding')
    if not 0<=area<=1:raise AssertionError('area outside unit pixel')
    return area


polygon_coverage=polygon_pixel_area
