from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import clip01
from oracle.ops.mask_morphology.common.exact import curve01
from oracle.ops.mask_morphology.common.exact import format_info
from oracle.ops.mask_morphology.common.exact import integer
from oracle.ops.mask_morphology.common.exact import param
from oracle.ops.mask_morphology.common.exact import rn
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.mask05.reference import morph_raw
from oracle.ops.mask_morphology.common.registration import op
import math

def radii(inner, outer, curve):
    i, o = param(inner, minimum=0), param(outer, minimum=0)
    if i > o: raise OracleError("inner>outer")
    curve01(Q(0), curve)
    return i, o

def metric_value(dy, dx, sy, sx, metric):
    y, x = abs(dy)*sy, abs(dx)*sx
    if metric == 'l2': return y*y+x*x
    if metric == 'l1': return y+x
    if metric == 'linf': return max(y, x)
    raise OracleError("metric")

def metric_offsets(radius, sy, sx, metric):
    ry, rx = int(radius//sy), int(radius//sx)
    if ry > 128 or rx > 128: raise OracleError("manual oracle stencil cap128", "OracleCapacity")
    lim = radius*radius if metric == 'l2' else radius
    return [(y, x) for y in range(-ry, ry+1) for x in range(-rx, rx+1)
            if metric_value(y, x, sy, sx, metric) <= lim]

@op
def offset_discrete(input, radius, sy=1., sx=1., metric="l2", dtype="float64"):
    a = grid(input, dtype, "binary")
    r, sy, sx = param(radius), param(sy, minimum=0, strict=True), param(sx, minimum=0, strict=True)
    metric_value(0, 0, sy, sx, metric)
    bs = metric_offsets(abs(r), sy, sx, metric)
    return [[v for v in row] for row in morph_raw(a, bs, "dilate" if r >= 0 else "erode")]

@op
def shift_distance_field(input, radius, dtype="float64"):
    a, r = grid(input, dtype, "distance"), param(radius)
    return [row[:] for row in a] if not r else [[v if math.isinf(v) else rn(Q(v)-r, dtype) for v in row] for row in a]

@op
def threshold_distance_field(input, radius=0., dtype="float64"):
    a, r = grid(input, dtype, "distance"), param(radius)
    return [[rn(int(v <= r), dtype) for v in row] for row in a]

def orient(a, b, c):
    return (b[1]-a[1])*(c[0]-a[0])-(b[0]-a[0])*(c[1]-a[1])

def on_segment(p, a, b):
    return orient(a, b, p) == 0 and all(min(a[i], b[i]) <= p[i] <= max(a[i], b[i]) for i in (0, 1))

def intersects(a, b, c, d):
    o = [orient(a, b, c), orient(a, b, d), orient(c, d, a), orient(c, d, b)]
    return ((o[0]*o[1] < 0 and o[2]*o[3] < 0) or on_segment(c, a, b) or
            on_segment(d, a, b) or on_segment(a, c, d) or on_segment(b, c, d))

def polygon_validate(vs):
    n = len(vs)
    if n < 3 or len(set(vs)) != n: raise OracleError("polygon vertices/repetitions")
    if sum(vs[i][1]*vs[(i+1)%n][0]-vs[(i+1)%n][1]*vs[i][0] for i in range(n)) == 0:
        raise OracleError("polygon zero area")
    for i in range(n):
        a, b = vs[i], vs[(i+1)%n]
        if a == b: raise OracleError("zero edge")
        prev = vs[(i-1)%n]
        if on_segment(prev, a, b) or on_segment(b, prev, a):
            raise OracleError("overlapping adjacent edges")
        for j in range(i+1, n):
            if j == i+1 or (i == 0 and j == n-1): continue
            if intersects(a, b, vs[j], vs[(j+1)%n]): raise OracleError("self-intersecting/touching polygon")

def polygon_sample(p, vs, radius):
    inside = False
    mind2 = None
    edge = False
    for a, b in zip(vs, vs[1:]+vs[:1]):
        delta = (b[0]-a[0], b[1]-a[1])
        v = (p[0]-a[0], p[1]-a[1])
        t = clip01((v[0]*delta[0]+v[1]*delta[1])/(delta[0]**2+delta[1]**2))
        d2 = (v[0]-t*delta[0])**2+(v[1]-t*delta[1])**2
        mind2 = d2 if mind2 is None else min(mind2, d2)
        if not d2: edge = True
        if (a[0] > p[0]) != (b[0] > p[0]):
            crossing = a[1]+(p[0]-a[0])*(b[1]-a[1])/(b[0]-a[0])
            if p[1] < crossing: inside = not inside
    inside = inside or edge
    if radius >= 0: return inside or mind2 <= radius*radius
    return inside and mind2 >= radius*radius

VULKAN_STANDARD_SIXTEENTHS = {1: [(8, 8)], 2: [(12, 12), (4, 4)], 4: [(6, 2), (14, 6), (2, 10), (10, 14)], 8: [(9, 5), (7, 11), (13, 9), (5, 3), (3, 13), (1, 7), (11, 15), (15, 1)], 16: [(9, 9), (7, 5), (5, 10), (12, 7), (3, 6), (10, 13), (13, 11), (11, 3), (6, 14), (8, 1), (4, 2), (2, 12), (0, 8), (15, 4), (14, 15), (1, 0)]}

def polygon_sample_locations(sample_pattern, samples_per_axis, sample_count):
    if sample_pattern == 'grid_center':
        if sample_count is not None: raise OracleError('sample_count forbidden for grid_center')
        n = integer(samples_per_axis, 'samples_per_axis', 1, 64)
        return [(Q(2*j+1,2*n), Q(2*i+1,2*n)) for i in range(n) for j in range(n)]
    if sample_pattern == 'vulkan_standard':
        if samples_per_axis is not None: raise OracleError('samples_per_axis forbidden for vulkan_standard')
        n = integer(sample_count, 'sample_count', 1, 16)
        if n not in VULKAN_STANDARD_SIXTEENTHS: raise OracleError('unsupported standard count')
        return [(Q(x,16),Q(y,16)) for x,y in VULKAN_STANDARD_SIXTEENTHS[n]]
    raise OracleError('unknown sample_pattern')

@op
def offset_polygon_grid(vertices, H, W, radius=0., sy=1., sx=1., samples_per_axis=None, output_dtype="float64", dtype="float64", sample_pattern="grid_center", sample_count=None):
    format_info(output_dtype)
    vertices = grid(vertices, dtype, "field")
    if len(vertices[0]) != 2: raise OracleError("polygon Vx2", "TypeMismatch")
    vs = [tuple(Q(c) for c in row) for row in vertices]
    polygon_validate(vs)
    H, W = integer(H, "H", 1), integer(W, "W", 1)
    points = polygon_sample_locations(sample_pattern, samples_per_axis, sample_count)
    if H*W*len(points)*len(vs) > 1000000: raise OracleError("manual polygon oracle capacity", "OracleCapacity")
    r, sy, sx = param(radius), param(sy, minimum=0, strict=True), param(sx, minimum=0, strict=True)
    out = []
    for y in range(H):
        row = []
        for x in range(W):
            count = sum(polygon_sample(((Q(y)-Q(1,2)+v)*sy,
                                        (Q(x)-Q(1,2)+u)*sx), vs, r) for u,v in points)
            row.append(rn(Q(count, len(points)), output_dtype))
        out.append(row)
    return out
