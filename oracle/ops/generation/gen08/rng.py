from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.rng import _bounds
from oracle.ops.generation.common.rng import _inside_map
from oracle.ops.generation.common.rng import _integer
from oracle.ops.generation.common.rng import distance2
from oracle.ops.generation.noi01.rng import block
from oracle.ops.generation.noi01.rng import halfopen53
import math


def jitter_points(bounds,nx,ny,jitter=1,seed=0,stream=0):
    x0,y0,x1,y1=_bounds(bounds);jitter=frac(jitter)
    if nx<1 or ny<1 or not 0<=jitter<=1:raise ValueError('grid/jitter')
    out=[]
    for j in range(ny):
        for i in range(nx):
            b=block(i,j,domain=3,seed=seed,stream=stream)
            u,v=halfopen53(b[0]),halfopen53(b[1])
            left=x0+F(i,nx)*(x1-x0);right=x0+F(i+1,nx)*(x1-x0)
            top=y0+F(j,ny)*(y1-y0);bottom=y0+F(j+1,ny)*(y1-y0)
            out.append((_inside_map(F(1,2)+jitter*(u-F(1,2)),left,right),
                        _inside_map(F(1,2)+jitter*(v-F(1,2)),top,bottom)))
    if len(set(out))!=len(out):raise ValueError('rounded duplicate point')
    return out


def pairwise_distances(points,min_distance):
    r=frac(min_distance)
    if r<=0:raise ValueError('positive distance')
    return all(distance2(a,b)>=r*r for i,a in enumerate(points) for b in points[:i])


def rejection_points(bounds,min_distance,candidate_count,seed=0,stream=0,max_count=1048576,with_status=False):
    x0,y0,x1,y1=_bounds(bounds);r=frac(min_distance)
    _integer(candidate_count,0,(1<<31)-1,'candidate_count')
    _integer(max_count,0,(1<<63)-1,'max_count')
    if r<=0:raise ValueError('positive distance')
    points=[]
    if max_count==0:
        return {'points':points,'stop_reason':'count_reached'} if with_status else points
    for i in range(candidate_count):
        b=block(i,0,domain=4,seed=seed,stream=stream)
        p=(_inside_map(halfopen53(b[0]),x0,x1),_inside_map(halfopen53(b[1]),y0,y1))
        if all(distance2(p,q)>=r*r for q in points):
            points.append(p)
            if len(points)==max_count:
                return {'points':points,'stop_reason':'count_reached'} if with_status else points
    return {'points':points,'stop_reason':'candidates_exhausted'} if with_status else points


def bridson_reference(bounds,min_distance,k=30,seed=0,stream=0,max_count=1048576,max_iterations=2097153,with_status=False):
    """MEASURED float candidates + exact acceptance; NOT a strict sequence oracle."""
    x0,y0,x1,y1=_bounds(bounds);r=frac(min_distance)
    if r<=0 or not 1<=k<=256 or max_count<0:raise ValueError('Bridson parameters')
    _integer(max_count,0,(1<<63)-1,"max_count")
    _integer(max_iterations,0,(1<<31)-1,"max_iterations")
    if max_count==0:
        return {'points':[],'stop_reason':'count_reached'} if with_status else []
    b=block(0,0,domain=5,seed=seed,stream=stream)
    points=[(_inside_map(halfopen53(b[0]),x0,x1),_inside_map(halfopen53(b[1]),y0,y1))]
    active=[0];iteration=0
    while active:
        if len(points)==max_count:
            return {'points':points,'stop_reason':'count_reached'} if with_status else points
        if iteration>=max_iterations:raise ArithmeticError('NotConverged: iteration limit')
        parent=points[active[0]];accepted=False
        for attempt in range(k):
            b=block(iteration+1,attempt,domain=5,seed=seed,stream=stream)
            u,v=float(halfopen53(b[0])),float(halfopen53(b[1]))
            rho=float(r)*math.sqrt(1+3*u);theta=2*math.pi*v
            p=(parent[0]+rho*math.cos(theta),parent[1]+rho*math.sin(theta))
            if not all(math.isfinite(v) for v in p):raise OverflowError('candidate overflow')
            if not(x0<=F(p[0])<x1 and y0<=F(p[1])<y1):continue
            if any(distance2(p,q)<r*r for q in points):continue
            points.append(p);active.append(len(points)-1);accepted=True;break
        if not accepted:active.pop(0)
        iteration+=1
    return {'points':points,'stop_reason':'active_exhausted'} if with_status else points
