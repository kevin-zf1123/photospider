from __future__ import annotations
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import format_info
from oracle.ops.mask_morphology.common.exact import param
from oracle.ops.mask_morphology.common.exact import rn
from oracle.ops.mask_morphology.common.exact import sqrt_exact
from oracle.ops.mask_morphology.common.reference import blank
from oracle.ops.mask_morphology.common.reference import coords
from oracle.ops.mask_morphology.common.reference import dims
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import mapped
from oracle.ops.mask_morphology.common.reference import same
from oracle.ops.mask_morphology.mask07.reference import metric_value
from oracle.ops.mask_morphology.mask10.reference import distance_setup
from oracle.ops.mask_morphology.mask10.reference import nearest_at
from oracle.ops.mask_morphology.mask10.reference import rounded_distance
from oracle.ops.mask_morphology.mask11.reference import neighbors
from oracle.ops.mask_morphology.mask12.reference import components
from oracle.ops.mask_morphology.common.registration import op

def topology(a):
    h, w = dims(a)
    bg = [[1]*(w+2)] + [[1]+[int(not v) for v in row]+[1] for row in a] + [[1]*(w+2)]
    return len(components(a, 8)), len(components(bg, 4))

def maximal_ball_sets(input, metric, sy, sx, dtype):
    a,sy,sx,ring=distance_setup(input,dtype,metric,sy,sx,'background')
    if len(a)*len(a[0])>64: raise OracleError('manual maximal-ball oracle limited to 64 pixels','OracleCapacity')
    foreground=[p for p in coords(a) if a[p[0]][p[1]]]
    scores={p:nearest_at(a,*p,False,ring,metric,sy,sx)[0] for p in foreground}
    supports={p:frozenset(q for q in foreground if metric_value(p[0]-q[0],p[1]-q[1],sy,sx,metric)<scores[p]) for p in foreground}
    retained={p for p in foreground if not any(supports[p]<other for other in supports.values())}
    return a,scores,supports,retained

@op
def medial_axis_maximal_balls(input,metric='l2',sy=1.,sx=1.,output_dtype='float64',dtype='float64',request='both'):
    format_info(output_dtype)
    if request not in ('both','axis','radius'):raise OracleError('invalid output request')
    a,scores,supports,retained=maximal_ball_sets(input,metric,sy,sx,dtype)
    out={}
    if request!='radius':out['axis']=[[float((y,x) in retained) for x in range(len(a[0]))] for y in range(len(a))]
    if request!='axis':out['radius']=[[rounded_distance(scores[y,x],metric,False,output_dtype) if (y,x) in retained else 0. for x in range(len(a[0]))] for y in range(len(a))]
    return out

def standard_thinning_delete(b, phase, algorithm):
    if algorithm == 'zhang_suen':
        transitions = sum(not b[i] and b[(i+1)%8] for i in range(8))
        triples = ((0,2,4),(2,4,6)) if phase==0 else ((0,2,6),(0,4,6))
        return 2<=sum(b)<=6 and transitions==1 and all(not all(b[i] for i in t) for t in triples)
    crossings = sum(not b[i] and (b[(i+1)%8] or b[(i+2)%8]) for i in range(0,8,2))
    pair_counts = [sum(b[(i+offset)%8] or b[(i+offset+1)%8] for i in range(0,8,2)) for offset in (-1,0)]
    gate = b[6] and (b[4] or b[5] or not b[7]) if phase==0 else b[2] and (b[0] or b[1] or not b[3])
    return crossings==1 and 2<=min(pair_counts)<=3 and not gate

def standard_thinning(input, dtype, algorithm):
    a=grid(input,dtype,'binary')
    if len(a)*len(a[0])>256: raise OracleError('manual thinning oracle limited to 256 pixels','OracleCapacity')
    a=[[int(v) for v in row] for row in a]
    offsets=((-1,0),(-1,1),(0,1),(1,1),(1,0),(1,-1),(0,-1),(-1,-1))
    while True:
        changed=False
        for phase in (0,1):
            deleted=[]
            for y,x in coords(a):
                if a[y][x] and standard_thinning_delete([bool(mapped(a,y+dy,x+dx)) for dy,dx in offsets],phase,algorithm):
                    deleted.append((y,x))
            for y,x in deleted:a[y][x]=0
            changed=changed or bool(deleted)
        if not changed:return [[float(v) for v in row] for row in a]

@op
def thin_zhang_suen(input,dtype='float64'):
    return standard_thinning(input,dtype,'zhang_suen')

@op
def thin_guo_hall(input,dtype='float64'):
    return standard_thinning(input,dtype,'guo_hall')

def thin_impl(input, dtype, distance_order=False, sy=1., sx=1., output_dtype='float64'):
    a = grid(input, dtype, 'binary')
    if len(a)*len(a[0]) > 256: raise OracleError("manual topology oracle limited to256 pixels", "OracleCapacity")
    a = [[int(v) for v in row] for row in a]
    h, w = dims(a)
    order = [(y, x) for y, x in coords(a) if a[y][x]]
    priorities = {}
    if distance_order:
        sy, sx = param(sy, minimum=0, strict=True), param(sx, minimum=0, strict=True)
        _, _, _, ring = distance_setup(a, dtype, 'l2', sy, sx, 'background')
        for y, x in order: priorities[y, x] = nearest_at(a, y, x, False, ring, 'l2', sy, sx)[0]
        order.sort(key=lambda p: (priorities[p], p))
    topo = topology(a)
    while True:
        changed = False
        for y, x in order:
            if not a[y][x]: continue
            if sum(a[yy][xx] for yy, xx in neighbors(y, x, h, w, 8)) <= 1: continue
            a[y][x] = 0
            if topology(a) == topo: changed = True
            else: a[y][x] = 1
        if not changed: break
    if not distance_order: return a
    radius = [[rn(sqrt_exact(priorities[y, x]), output_dtype) if a[y][x] else 0.
               for x in range(w)] for y in range(h)]
    return dict(skeleton=a, radius=radius)
@op
def thin_topological(input, dtype='float64'):
    return thin_impl(input, dtype)
@op
def thin_distance_ordered(input, sy=1., sx=1., output_dtype='float64', dtype='float64'):
    format_info(output_dtype)
    return thin_impl(input, dtype, True, sy, sx, output_dtype)

def reconstruct(marker, limit, connectivity, dtype, erode):
    m, l = grid(marker, dtype), grid(limit, dtype); same(m, l)
    h, w = dims(m); neighbors(0, 0, h, w, connectivity)
    for y, x in coords(m):
        if m[y][x] < l[y][x] if erode else m[y][x] > l[y][x]:
            raise OracleError("marker/limit ordering violated")
    state = [[0 if v == 0 else v for v in row] for row in m]
    l = [[0 if v == 0 else v for v in row] for row in l]
    while True:
        new = blank(m, 0)
        for y, x in coords(m):
            vals = [state[y][x]]+[state[yy][xx] for yy, xx in neighbors(y, x, h, w, connectivity)]
            new[y][x] = max(l[y][x], min(vals)) if erode else min(l[y][x], max(vals))
        if new == state: return [[float(v) for v in row] for row in new]
        state = new
@op
def reconstruct_dilate(marker, limit, connectivity=4, dtype='float64'):
    return reconstruct(marker, limit, connectivity, dtype, False)
@op
def reconstruct_erode(marker, limit, connectivity=4, dtype='float64'):
    return reconstruct(marker, limit, connectivity, dtype, True)
