from __future__ import annotations
from oracle.ops.generation.common.exact import frac
from oracle.ops.generation.common.geometry import cross
from oracle.ops.generation.common.geometry import point
import math


def reverse_bezier(controls):return list(reversed(controls))


def reverse_polyline(points,closed=False):
    return ([points[0]]+list(reversed(points[1:]))) if closed and points else list(reversed(points))


def concat_paths(paths):return [subpath for path in paths for subpath in path]


def split_subpaths(paths,groups,group_count):
    if len(paths)!=len(groups) or group_count<1 or any(not 0<=g<group_count for g in groups):raise ValueError('groups')
    out=[];offsets=[0]
    for g in range(group_count):out.extend(p for p,k in zip(paths,groups) if k==g);offsets.append(len(out))
    return out,offsets


def validate_core_fixture(verbs,controls,control_offsets,subpath_offsets,closed):
    """Logical M/L/Q/C/Z fixture only; does not validate ABI associations."""
    arity={'M':1,'L':1,'Q':2,'C':3,'Z':0}
    pp=list(map(point,controls))
    if len(control_offsets)!=len(verbs)+1 or not control_offsets or control_offsets[0]!=0 or control_offsets[-1]!=len(pp):raise ValueError('control offsets')
    if len(subpath_offsets)!=len(closed)+1 or not subpath_offsets or subpath_offsets[0]!=0 or subpath_offsets[-1]!=len(verbs):raise ValueError('subpath offsets')
    for i,v in enumerate(verbs):
        if v not in arity or control_offsets[i+1]-control_offsets[i]!=arity[v]:raise ValueError('arity')
    for a,b,c in zip(subpath_offsets,subpath_offsets[1:],closed):
        if not a<b or verbs[a]!='M' or any(v=='M' for v in verbs[a+1:b]):raise ValueError('subpath M')
        if any(v=='Z' for v in verbs[a:b-1]) or bool(verbs[b-1]=='Z')!=bool(c):raise ValueError('closed/Z')
    return True


def validate_arc_fixture(center,u,v,start,sweep=None,direction=None):
    center,u,v=map(point,(center,u,v));frac(start)
    if not cross(u,v):raise ValueError('degenerate ellipse axes')
    if direction is not None:
        if direction not in (-1,1) or sweep is not None:raise ValueError('fullturn direction')
    elif sweep is None or not 0<abs(float(sweep))<2*math.pi:
        # MEASURED boundary check only; exact nonzero test is rational.
        raise ValueError('arc sweep')
    return True
