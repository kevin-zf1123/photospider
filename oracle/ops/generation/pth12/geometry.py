from __future__ import annotations
from fractions import Fraction as F
from functools import cmp_to_key
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.exact import round_fraction
from oracle.ops.generation.common.geometry import _edges
from oracle.ops.generation.common.geometry import _on_segment
from oracle.ops.generation.common.geometry import add
from oracle.ops.generation.common.geometry import cross
from oracle.ops.generation.common.geometry import dot
from oracle.ops.generation.common.geometry import mul
from oracle.ops.generation.common.geometry import norm2
from oracle.ops.generation.common.geometry import orientation
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.common.geometry import point_inside
from oracle.ops.generation.common.geometry import point_segment_distance2
from oracle.ops.generation.common.geometry import segment_intersections
from oracle.ops.generation.common.geometry import sub
from oracle.ops.generation.pth06.geometry import polygon_area


def _split_arrangement(edges):
    cuts=[{a,b} for a,b in edges]
    for i,(a,b) in enumerate(edges):
        for j,(c,d) in enumerate(edges[:i]):
            hits=segment_intersections(a,b,c,d);cuts[i].update(hits);cuts[j].update(hits)
    pieces=set()
    for (a,b),pp in zip(edges,cuts):
        d=sub(b,a);ordered=sorted(pp,key=lambda p:dot(sub(p,a),d))
        for u,v in zip(ordered,ordered[1:]):
            if u!=v:pieces.add(tuple(sorted((u,v))))
    return sorted(pieces)


def _side_points(a,b,all_edges):
    mid=mul(add(a,b),F(1,2));v=sub(b,a);normal=(-v[1],v[0]);n2=norm2(normal)
    distances=[point_segment_distance2(mid,c,d)[0] for c,d in all_edges if not _on_segment(mid,c,d)]
    limit=min(distances) if distances else F(1)
    if limit<=0:raise AssertionError('unsplit crossing at edge midpoint')
    step=F(1)
    while 4*step*step*n2>=limit:step/=2
    return add(mid,mul(normal,step)),sub(mid,mul(normal,step))


def boolean_boundary(a,b,op='union',fill_rule_a='nonzero',fill_rule_b='nonzero'):
    if op not in ('union','intersection','difference','xor'):raise ValueError('boolean op')
    edges=_edges(a)+_edges(b)
    def inside(p):
        x=point_inside(p,a,fill_rule_a,False);y=point_inside(p,b,fill_rule_b,False)
        return (x or y) if op=='union' else ((x and y) if op=='intersection' else (x and not y if op=='difference' else x!=y))
    result=[]
    for u,v in _split_arrangement(edges):
        left,right=_side_points(u,v,edges);li,ri=inside(left),inside(right)
        if li!=ri:result.append((u,v) if li else (v,u))
    return sorted(result)


def _angle_compare(a,b):
    def half(v):return 0 if (v[1]>0 or v[1]==0 and v[0]>=0) else 1
    ha,hb=half(a),half(b)
    if ha!=hb:return -1 if ha<hb else 1
    z=cross(a,b)
    return -1 if z>0 else (1 if z<0 else 0)


def canonical_polygon(contour):
    pp=list(map(point,contour))
    if len(pp)>1 and pp[0]==pp[-1]:pp.pop()
    changed=True
    while changed and len(pp)>2:
        changed=False
        for i in range(len(pp)):
            a,b,c=pp[i-1],pp[i],pp[(i+1)%len(pp)]
            if not orientation(a,b,c) and _on_segment(b,a,c):
                pp.pop(i);changed=True;break
    if len(pp)<3 or not polygon_area(pp):return []
    candidates=[pp[i:]+pp[:i] for i,v in enumerate(pp) if v==min(pp)]
    return min(candidates)


def boundary_contours(edges):
    if not edges:return []
    outgoing={}
    for i,(a,b) in enumerate(edges):outgoing.setdefault(a,[]).append(i)
    nxt={}
    for i,(a,b) in enumerate(edges):
        opts=outgoing.get(b,[])
        if not opts:raise ArithmeticError('open regularized boundary')
        reverse=sub(a,b)
        # predecessor of reverse in CCW order = clockwise successor for left face.
        items=[(sub(edges[j][1],b),j) for j in opts]+[(reverse,-1)]
        items.sort(key=cmp_to_key(lambda x,y:_angle_compare(x[0],y[0]) or ((x[1]>y[1])-(x[1]<y[1]))))
        k=next(k for k,x in enumerate(items) if x[1]==-1)
        nxt[i]=items[(k-1)%len(items)][1]
    used=set();out=[]
    for first in range(len(edges)):
        if first in used:continue
        cycle=[];i=first
        while i not in used:
            used.add(i);cycle.append(edges[i][0]);i=nxt[i]
        if i!=first:raise ArithmeticError('non-permutation boundary traversal')
        cc=canonical_polygon(cycle)
        if cc:out.append(cc)
    return sorted(out)


def boolean_regions(a,b,op='union',fill_rule_a='nonzero',fill_rule_b='nonzero'):
    """Exact rational boundary fixture; Float64 publication is a separate gate."""
    return boundary_contours(boolean_boundary(a,b,op,fill_rule_a,fill_rule_b))


def boolean_axis_aligned(a,b,op='union'):
    def rect(r):
        x0,y0,x1,y1=map(frac,r)
        if x0>=x1 or y0>=y1:raise ValueError('positive rectangle')
        return [(x0,y0),(x1,y0),(x1,y1),(x0,y1)]
    return boolean_regions([rect(a)],[rect(b)],op)


def publish_polygon_fixture(contours,epsilon):
    """Finite incidence/area/nesting checks; NOT a general isotopy certifier.

    Output witnesses and exact distances are useful rejection oracles. Passing
    this helper alone must not certify the full production topology contract.
    """
    epsilon=frac(epsilon)
    if epsilon<=0:raise ValueError('epsilon')
    src=[list(map(point,c)) for c in contours]
    out=[[point(tuple(round_fraction(v) for v in p)) for p in c] for c in src]
    mapping={p:q for c,d in zip(src,out) for p,q in zip(c,d)}
    if len(set(mapping.values()))!=len(mapping):raise ArithmeticError('InvalidQuality: merged distinct vertices')
    if any(norm2(sub(p,q))>epsilon*epsilon for p,q in mapping.items()):raise ArithmeticError('InvalidQuality: publication displacement')
    if any(polygon_area(a)*polygon_area(b)<=0 for a,b in zip(src,out)):raise ArithmeticError('InvalidQuality: contour orientation/area')
    ea,eb=_edges(src),_edges(out)
    if len(ea)!=len(eb):raise ArithmeticError('InvalidQuality: collapsed edge')
    for i,(a,b) in enumerate(ea):
        for j,(c,d) in enumerate(ea[:i]):
            old=segment_intersections(a,b,c,d);new=segment_intersections(*eb[i],*eb[j])
            if set(new)!={mapping[p] for p in old if p in mapping} or any(p not in mapping for p in old):
                raise ArithmeticError('InvalidQuality: incidence changed or source not arranged')
    # Check bounded-loop nesting using a safe interior sample adjacent to edge 0.
    def nesting(cs):
        ee=_edges(cs);result=[]
        for c in cs:
            left,right=_side_points(c[0],c[1],ee)
            sample=left if polygon_area(c)>0 else right
            result.append(tuple(point_inside(sample,[d],boundary=False) for d in cs))
        return result
    if nesting(src)!=nesting(out):raise ArithmeticError('InvalidQuality: loop nesting changed')
    return out
