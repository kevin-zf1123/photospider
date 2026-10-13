"""Independent exact finite 2D geometry oracle (Fraction arithmetic).

This module does not implement PathSet ABI, ObjectId associations, budgets or
CPU backend admission. Polygon area/arrangement routines are intentionally slow
reference algorithms for finite fixtures, including self-crossing and overlap.
Curve transcendental/root/inverse-length cases are not silently approximated.
"""

from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac


def point(p):
    if len(p)!=2:raise ValueError('2D point')
    return tuple(map(frac,p))

def add(a,b):return tuple(x+y for x,y in zip(a,b))
def sub(a,b):return tuple(x-y for x,y in zip(a,b))
def mul(a,s):return tuple(x*s for x in a)
def dot(a,b):return sum(x*y for x,y in zip(a,b))
def cross(a,b):return a[0]*b[1]-a[1]*b[0]
def norm2(a):return dot(a,a)

def orientation(a,b,c):
    a,b,c=map(point,(a,b,c));return cross(sub(b,a),sub(c,a))


def point_segment_distance2(p,a,b):
    p,a,b=map(point,(p,a,b));v=sub(b,a);den=norm2(v)
    t=min(F(1),max(F(0),dot(sub(p,a),v)/den)) if den else F(0)
    closest=add(a,mul(v,t))
    return norm2(sub(p,closest)),t,closest


def pairwise_distances(points,min_distance):
    r=frac(min_distance)
    if r<=0:raise ValueError('positive distance')
    pp=list(map(point,points))
    return all(norm2(sub(a,b))>=r*r for i,a in enumerate(pp) for b in pp[:i])


def _edges(contours):
    edges=[]
    for contour in contours:
        p=list(map(point,contour))
        edges.extend((a,b) for a,b in zip(p,p[1:]+p[:1]) if a!=b)
    return edges


def _on_segment(p,a,b):
    return not orientation(a,b,p) and min(a[0],b[0])<=p[0]<=max(a[0],b[0]) and min(a[1],b[1])<=p[1]<=max(a[1],b[1])


def segment_intersections(a,b,c,d):
    a,b,c,d=map(point,(a,b,c,d));r,s=sub(b,a),sub(d,c);den=cross(r,s)
    if den:
        t=cross(sub(c,a),s)/den;u=cross(sub(c,a),r)/den
        return [add(a,mul(r,t))] if 0<=t<=1 and 0<=u<=1 else []
    if cross(sub(c,a),r):return []
    return sorted({p for p in (a,b,c,d) if _on_segment(p,a,b) and _on_segment(p,c,d)})


def point_inside(p,contours,fill_rule='nonzero',boundary=True):
    p=point(p);w=0
    if fill_rule not in ('nonzero','evenodd'):raise ValueError('fill rule')
    for a,b in _edges(contours):
        if _on_segment(p,a,b):return boundary
        o=orientation(a,b,p)
        if a[1]<=p[1]<b[1] and o>0:w+=1
        elif b[1]<=p[1]<a[1] and o<0:w-=1
    return w!=0 if fill_rule=='nonzero' else w%2!=0

def split_bezier(controls,t=F(1,2)):
    work=list(map(point,controls));t=frac(t)
    if not 0<=t<=1:raise ValueError('split parameter')
    left=[work[0]];right=[work[-1]]
    while len(work)>1:
        work=[add(mul(a,1-t),mul(b,t)) for a,b in zip(work,work[1:])]
        left.append(work[0]);right.append(work[-1])
    return left,list(reversed(right))
