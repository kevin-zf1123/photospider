"""Small-image mathematical references for every MASK member.

Pure Python exact arithmetic, deliberately slow connectivity/nearest-site
oracles. Not a production kernel, metadata codec or resource/demand simulator.
See README.md and the normative family documents for the execution boundary.
"""
from __future__ import annotations
from collections import deque
from fractions import Fraction as Q
import math
import re
from exact import (OracleError, atom, finite, param, integer, rn, sqrt_exact,
                   Quadratic, clip01, curve01, format_info)

REGISTRY = {}
def op(fn):
    REGISTRY[fn.__name__] = fn
    return fn

def grid(data, dtype="float64", domain="coverage"):
    if not isinstance(data, list) or not data or not isinstance(data[0], list) or not data[0]:
        raise OracleError("expected nonempty rectangular HW array", "TypeMismatch")
    h, w = len(data), len(data[0])
    if any(not isinstance(row, list) or len(row) != w for row in data):
        raise OracleError("ragged array", "TypeMismatch")
    if domain != "raw" and dtype not in ("float32", "float64"):
        raise OracleError("field requires Float32/64", "TypeMismatch")
    out = [[atom(v, dtype) for v in row] for row in data]
    if domain == "raw":
        return out
    for row in out:
        for v in row:
            if domain == "distance" and math.isinf(v):
                continue
            f = finite(v)
            if domain == "coverage" and not 0 <= f <= 1:
                raise OracleError("coverage outside [0,1]")
            if domain == "binary" and f not in (0, 1):
                raise OracleError("binary sample is not 0/1")
    return out

def labels_grid(data):
    if not isinstance(data, list) or not data or not data[0]:
        raise OracleError("expected nonempty labels", "TypeMismatch")
    w = len(data[0])
    if any(not isinstance(r, list) or len(r) != w for r in data):
        raise OracleError("ragged labels", "TypeMismatch")
    for row in data:
        for v in row:
            integer(v, "label", 0)
    return [r[:] for r in data]

def tensor(data, dtype="float64", domain="raw"):
    if dtype not in ("float32", "float64"):
        raise OracleError("image tensor requires Float32/64", "TypeMismatch")
    if not isinstance(data, list) or not data:
        raise OracleError("expected CHW tensor", "TypeMismatch")
    out = [grid(p, dtype, domain) for p in data]
    for plane in out[1:]:
        same(out[0], plane)
    return out

def same(a, b):
    if len(a) != len(b) or len(a[0]) != len(b[0]):
        raise OracleError("HW shapes differ", "TypeMismatch")

def dims(a):
    return len(a), len(a[0])

def coords(a):
    return [(y, x) for y in range(len(a)) for x in range(len(a[0]))]

def blank(a, value=0):
    return [[value for _ in row] for row in a]

def outnum(q, dtype):
    return rn(q, dtype)

def mapped(a, y, x, boundary="zero"):
    h, w = dims(a)
    if 0 <= y < h and 0 <= x < w:
        return a[y][x]
    if boundary == "zero":
        return 0.0
    if boundary == "replicate":
        return a[min(max(y, 0), h-1)][min(max(x, 0), w-1)]
    if boundary == "reflect_half":
        def refl(i, n):
            j = i % (2*n)
            return j if j < n else 2*n-1-j
        return a[refl(y, h)][refl(x, w)]
    raise OracleError("invalid boundary")

def channel_indices(channels, c):
    if not isinstance(channels, str) or not re.fullmatch(r'(?:0|[1-9][0-9]*)(?:,(?:0|[1-9][0-9]*))*', channels):
        raise OracleError("noncanonical channel selector")
    indices = list(map(int, channels.split(',')))
    if len(set(indices)) != len(indices) or any(i >= c for i in indices):
        raise OracleError("duplicate/out-of-range channel")
    return indices

@op
def binary_logic(a, b, operation="and", dtype="float64"):
    a, b = grid(a, dtype, "binary"), grid(b, dtype, "binary")
    same(a, b)
    if operation not in ("and", "or", "xor", "subtract"):
        raise OracleError("invalid Boolean operation")
    def calc(x, y):
        return {"and": x and y, "or": x or y, "xor": x != y,
                "subtract": x and not y}[operation]
    return [[outnum(int(bool(calc(x, y))), dtype) for x, y in zip(ar, br)]
            for ar, br in zip(a, b)]

@op
def fuzzy_logic(a, b, operation="and", dtype="float64"):
    if dtype == "uint8": raise OracleError("soft algebra needs Float32/64", "TypeMismatch")
    a, b = grid(a, dtype), grid(b, dtype)
    same(a, b)
    if operation not in ("and", "or", "xor", "subtract"): raise OracleError("operation")
    def calc(x, y):
        if operation == "and": return x if x <= y else y
        if operation == "or": return x if x >= y else y
        return rn(abs(Q(x)-Q(y)) if operation == "xor" else min(Q(x), 1-Q(y)), dtype)
    return [[calc(x, y) for x, y in zip(ar, br)] for ar, br in zip(a, b)]

@op
def independent_coverage(a, b, operation="and", dtype="float64"):
    if dtype == "uint8": raise OracleError("soft algebra needs Float32/64", "TypeMismatch")
    a, b = grid(a, dtype), grid(b, dtype); same(a, b)
    if operation not in ("and", "or", "xor", "subtract"): raise OracleError("operation")
    def calc(x, y):
        x, y = Q(x), Q(y)
        return rn({"and": x*y, "or": x+y-x*y, "xor": x+y-2*x*y,
                   "subtract": x*(1-y)}[operation], dtype)
    return [[calc(x, y) for x, y in zip(ar, br)] for ar, br in zip(a, b)]

@op
def invert(input, dtype="float64"):
    return [[outnum(1-Q(v), dtype) for v in row] for row in grid(input, dtype)]

@op
def threshold(input, threshold=.5, comparison="ge", dtype="float64"):
    if comparison not in ("ge", "gt", "le", "lt"): raise OracleError("comparison")
    t = param(threshold)
    def calc(v):
        x = Q(v)
        return outnum(int({"ge": x >= t, "gt": x > t, "le": x <= t, "lt": x < t}[comparison]), dtype)
    return [[calc(v) for v in row] for row in grid(input, dtype, "field")]

@op
def range_mask(input, lower, upper, lower_closed=True, upper_closed=True, dtype="float64"):
    l, u = param(lower), param(upper)
    if l > u or type(lower_closed) is not bool or type(upper_closed) is not bool:
        raise OracleError("invalid interval")
    return [[outnum(int((Q(v) >= l if lower_closed else Q(v) > l) and
                       (Q(v) <= u if upper_closed else Q(v) < u)), dtype)
             for v in row] for row in grid(input, dtype, "field")]

def soft_edge(x, t, width, curve):
    return Q(x >= t) if not width else curve01(clip01((x-t+width/2)/width), curve)

@op
def soft_threshold(input, threshold=.5, width=0., curve="smoothstep", dtype="float64"):
    t, w = param(threshold), param(width, minimum=0)
    curve01(Q(0), curve)
    return [[rn(soft_edge(Q(v), t, w, curve), dtype) for v in row]
            for row in grid(input, dtype, "field")]

@op
def soft_range(input, lower, upper, lower_width=0., upper_width=0., curve="smoothstep", dtype="float64"):
    l, u = param(lower), param(upper)
    wl, wu = param(lower_width, minimum=0), param(upper_width, minimum=0)
    curve01(Q(0), curve)
    if l > u: raise OracleError("lower>upper")
    def calc(v):
        x = Q(v)
        low = soft_edge(x, l, wl, curve)
        high = Q(x <= u) if not wu else 1-soft_edge(x, u, wu, curve)
        return rn(min(low, high), dtype)
    return [[calc(v) for v in row] for row in grid(input, dtype, "field")]

@op
def nonzero_to_binary(input, output_dtype="float64", dtype="uint8"):
    if dtype != "uint8": raise OracleError("UInt8 input required", "TypeMismatch")
    format_info(output_dtype)
    return [[rn(int(v != 0), output_dtype) for v in row] for row in grid(input, "uint8", "raw")]

def radial(distance, inner, outer, curve, dtype):
    def cmp(v):
        return distance.compare(v) if isinstance(distance, Quadratic) else (distance > v)-(distance < v)
    if inner == outer: return rn(int(cmp(inner) <= 0), dtype)
    if cmp(inner) <= 0: return rn(1, dtype)
    if cmp(outer) >= 0: return 0.0
    return rn(1-curve01((distance-inner)/(outer-inner), curve), dtype)

def radii(inner, outer, curve):
    i, o = param(inner, minimum=0), param(outer, minimum=0)
    if i > o: raise OracleError("inner>outer")
    curve01(Q(0), curve)
    return i, o

@op
def coordinate_range(image, target, scales, channels, inner=0., outer=1., curve="smoothstep", dtype="float64"):
    image = tensor(image, dtype)
    cs = channel_indices(channels, len(image))
    if len(target) != len(cs) or len(scales) != len(cs): raise OracleError("target/scales shape", "TypeMismatch")
    ts = [param(t) for t in target]; ss = [param(s, minimum=0, strict=True) for s in scales]
    i, o = radii(inner, outer, curve)
    out = blank(image[0], 0.)
    for y, x in coords(out):
        sq = sum(((finite(image[c][y][x])-t)/s)**2 for c, t, s in zip(cs, ts, ss))
        out[y][x] = radial(sqrt_exact(sq), i, o, curve, dtype)
    return out

@op
def lab76_range(image, target, channels="0,1,2", inner=0., outer=1., curve="smoothstep", dtype="float64"):
    image = tensor(image, dtype)
    cs = channel_indices(channels, len(image))
    if len(cs) != 3 or len(target) != 3: raise OracleError("Lab needs 3 coordinates", "TypeMismatch")
    ts = [param(t) for t in target]; i, o = radii(inner, outer, curve)
    out = blank(image[0], 0.)
    for y, x in coords(out):
        delta = [finite(image[c][y][x])-t for c, t in zip(cs, ts)]
        sq = (100*delta[0])**2+delta[1]**2+delta[2]**2
        out[y][x] = radial(sqrt_exact(sq), i, o, curve, dtype)
    return out

@op
def hue_range(image, channels="0,1", center_turns=0., minimum_chroma=0., neutral_policy="exclude", inner=0., outer=.5, curve="smoothstep", dtype="float64"):
    image = tensor(image, dtype)
    cs = channel_indices(channels, len(image))
    if len(cs) != 2: raise OracleError("hue,chroma selector required")
    h0, minc = param(center_turns), param(minimum_chroma, minimum=0)
    i, o = radii(inner, outer, curve)
    if o > Q(1,2) or neutral_policy not in ("include", "exclude"): raise OracleError("hue parameters")
    out = blank(image[0], 0.)
    for y, x in coords(out):
        h, c = finite(image[cs[0]][y][x]), finite(image[cs[1]][y][x])
        if c == 0 or c < minc:
            out[y][x] = rn(int(neutral_policy == "include"), dtype)
        else:
            delta = h-h0
            delta -= (delta+Q(1,2)).numerator // (delta+Q(1,2)).denominator
            out[y][x] = radial(abs(delta), i, o, curve, dtype)
    return out

def footprint_offsets(footprint="square", radius=None, offsets=None):
    if footprint == "custom":
        if radius is not None or not isinstance(offsets, str): raise OracleError("custom footprint fields")
        integer_pat = r'(?:0|-?[1-9][0-9]*)'
        if not re.fullmatch(f'{integer_pat}:{integer_pat}(?:;{integer_pat}:{integer_pat})*', offsets):
            raise OracleError("custom footprint grammar")
        result = [tuple(map(int, entry.split(':'))) for entry in offsets.split(';')]
        for pair in result:
            for v in pair: integer(v, "offset", -(1 << 63))
        if result != sorted(set(result)) or (0, 0) not in result or set(result) != {(-y, -x) for y, x in result}:
            raise OracleError("custom footprint must be sorted, unique, symmetric, origin-containing")
        return result
    if offsets is not None or footprint not in ("square", "diamond", "disk"):
        raise OracleError("invalid footprint fields")
    r = integer(1 if radius is None else radius, "radius")
    # The oracle is intentionally bounded, not a production admission system.
    if r > 128: raise OracleError("manual oracle radius cap 128", "OracleCapacity")
    return [(y, x) for y in range(-r, r+1) for x in range(-r, r+1)
            if footprint == "square" or (abs(y)+abs(x) <= r if footprint == "diamond" else y*y+x*x <= r*r)]

def extremum(sample, y, x, offsets, erode=False):
    ordered = [(0, 0)]+[b for b in offsets if b != (0, 0)]
    best = sample(y, x)
    for dy, dx in ordered[1:]:
        val = sample(y+dy, x+dx)
        if val < best if erode else val > best:
            best = val
    return best

def morph_raw(a, offsets, mode):
    src = lambda y, x: mapped(a, y, x)
    if mode in ("dilate", "erode"):
        fn = lambda y, x: extremum(src, y, x, offsets, mode == "erode")
    else:
        first_erode = mode == "opening"
        cache = {}
        def inter(y, x):
            if (y, x) not in cache:
                cache[y, x] = extremum(src, y, x, offsets, first_erode)
            return cache[y, x]
        fn = lambda y, x: extremum(inter, y, x, offsets, not first_erode)
    return [[fn(y, x) for x in range(len(a[0]))] for y in range(len(a))]

def morph(input, mode, footprint="square", radius=None, offsets=None, dtype="float64"):
    a = grid(input, dtype)
    out = morph_raw(a, footprint_offsets(footprint, radius, offsets), mode)
    return [[v for v in r] for r in out]

@op
def dilate(input, **kw): return morph(input, "dilate", **kw)
@op
def erode(input, **kw): return morph(input, "erode", **kw)
@op
def opening(input, **kw): return morph(input, "opening", **kw)
@op
def closing(input, **kw): return morph(input, "closing", **kw)

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

@op
def gaussian_feather(input, sigma_y=1., sigma_x=1., radius_y=4, radius_x=4, boundary="zero", dtype="float64"):
    from gaussian import certified_gaussian
    return certified_gaussian(grid(input, dtype), sigma_y, sigma_x, radius_y, radius_x, boundary, dtype)

def feather(input, inner, outer, curve, dtype):
    a = grid(input, dtype, "distance")
    i, o = param(inner, minimum=0), param(outer, minimum=0)
    def calc(v):
        if math.isinf(v): return float(v < 0)
        d = Q(v)
        if not i+o: return rn(int(d <= 0), dtype)
        return rn(1-curve01(clip01((d+i)/(i+o)), curve), dtype)
    return [[calc(v) for v in row] for row in a]

@op
def distance_feather_linear(input, inner=1., outer=1., dtype="float64"):
    return feather(input, inner, outer, "linear", dtype)
@op
def distance_feather_smoothstep(input, inner=1., outer=1., dtype="float64"):
    return feather(input, inner, outer, "smoothstep", dtype)

def distance_setup(input, dtype, metric, sy, sx, exterior):
    a = grid(input, dtype, "binary")
    sy, sx = param(sy, minimum=0, strict=True), param(sx, minimum=0, strict=True)
    metric_value(0, 0, sy, sx, metric)
    if exterior not in ("none", "background"): raise OracleError("exterior")
    h, w = dims(a)
    ring = [(y, x) for y in range(-1, h+1) for x in range(-1, w+1)
            if y in (-1, h) or x in (-1, w)] if exterior == 'background' else []
    return a, sy, sx, ring

def nearest_at(a, y, x, fg, ring, metric, sy, sx, limit=None):
    sites = [(yy, xx) for yy, xx in coords(a) if (a[yy][xx] != 0) == fg]
    if not fg: sites += ring
    candidates = [(metric_value(y-yy, x-xx, sy, sx, metric), yy, xx) for yy, xx in sites]
    if limit is not None:
        bound = limit*limit if metric == 'l2' else limit
        candidates = [v for v in candidates if v[0] <= bound]
    return min(candidates) if candidates else None

def rounded_distance(score, metric, squared, dtype, sign=1):
    return rn(sign*sqrt_exact(score), dtype) if metric == 'l2' and not squared else rn(sign*score, dtype)

@op
def nearest_feature(input, feature="foreground", metric="l2", sy=1., sx=1., exterior="none", squared=False, output_dtype="float64", dtype="float64"):
    format_info(output_dtype)
    if feature not in ('foreground', 'background') or type(squared) is not bool or (squared and metric != 'l2'):
        raise OracleError("feature/squared")
    a, sy, sx, ring = distance_setup(input, dtype, metric, sy, sx, exterior)
    distance = blank(a, math.inf)
    nearest = [blank(a, -1), blank(a, -1)]
    for y, x in coords(a):
        best = nearest_at(a, y, x, feature == 'foreground', ring, metric, sy, sx)
        if best is not None:
            score, yy, xx = best
            distance[y][x] = rounded_distance(score, metric, squared, output_dtype)
            nearest[0][y][x], nearest[1][y][x] = yy, xx
    return dict(distance=distance, nearest=nearest)

@op
def signed_center_distance(input, metric="l2", sy=1., sx=1., exterior="background", output_dtype="float64", dtype="float64"):
    format_info(output_dtype)
    a, sy, sx, ring = distance_setup(input, dtype, metric, sy, sx, exterior)
    distance = blank(a, math.inf)
    for y, x in coords(a):
        fg = a[y][x] != 0
        best = nearest_at(a, y, x, not fg, ring, metric, sy, sx)
        if best is not None:
            distance[y][x] = rounded_distance(best[0], metric, False, output_dtype, -1 if fg else 1)
        else:
            distance[y][x] = -math.inf if fg else math.inf
    return dict(distance=distance)

@op
def truncated_nearest_feature(input, limit, feature="foreground", metric="l2", sy=1., sx=1., exterior="none", squared=False, output_dtype="float64", dtype="float64"):
    format_info(output_dtype)
    if feature not in ('foreground', 'background') or type(squared) is not bool or (squared and metric != 'l2'):
        raise OracleError("feature/squared")
    a, sy, sx, ring = distance_setup(input, dtype, metric, sy, sx, exterior)
    lim = param(limit, minimum=0, strict=True)
    distance, within = blank(a, 0.), blank(a, 0)
    for y, x in coords(a):
        best = nearest_at(a, y, x, feature == 'foreground', ring, metric, sy, sx, lim)
        if best is None:
            distance[y][x] = rn(lim*lim if squared else lim, output_dtype)
        else:
            distance[y][x] = rounded_distance(best[0], metric, squared, output_dtype)
            within[y][x] = 1
    return dict(distance=distance, within=within)

@op
def truncated_signed_distance(input, limit, metric="l2", sy=1., sx=1., exterior="background", output_dtype="float64", dtype="float64"):
    format_info(output_dtype)
    a, sy, sx, ring = distance_setup(input, dtype, metric, sy, sx, exterior)
    lim = param(limit, minimum=0, strict=True)
    distance, within = blank(a, 0.), blank(a, 0)
    for y, x in coords(a):
        fg = a[y][x] != 0
        best = nearest_at(a, y, x, not fg, ring, metric, sy, sx, lim)
        sign = -1 if fg else 1
        if best is None:
            distance[y][x] = rn(sign*lim, output_dtype)
        else:
            distance[y][x] = rounded_distance(best[0], metric, False, output_dtype, sign)
            within[y][x] = 1
    return dict(distance=distance, within=within)

def neighbors(y, x, h, w, connectivity):
    if connectivity not in (4, 8) or isinstance(connectivity, bool): raise OracleError("connectivity")
    return [(yy, xx) for yy in range(max(0, y-1), min(h, y+2))
            for xx in range(max(0, x-1), min(w, x+2))
            if (yy, xx) != (y, x) and (connectivity == 8 or abs(yy-y)+abs(xx-x) == 1)]

def reached(seeds, barrier, connectivity, vertex=None, edge=None):
    h, w = dims(barrier)
    neighbors(0, 0, h, w, connectivity)
    for y, x in seeds:
        if barrier[y][x]: raise OracleError("seed on barrier")
    found, queue = set(seeds), deque(seeds)
    while queue:
        y, x = queue.popleft()
        for yy, xx in neighbors(y, x, h, w, connectivity):
            if (yy, xx) in found or barrier[yy][xx]: continue
            if vertex is not None and not vertex(yy, xx): continue
            if edge is not None and not edge(y, x, yy, xx): continue
            found.add((yy, xx)); queue.append((yy, xx))
    return found

def flood_inputs(image, seeds, barrier, dtype):
    im = tensor(image, dtype, "field")
    s, b = grid(seeds, dtype, "binary"), grid(barrier, dtype, "binary")
    same(im[0], s); same(s, b)
    sites = [(y, x) for y, x in coords(s) if s[y][x]]
    if any(b[y][x] for y, x in sites): raise OracleError("seed on barrier")
    return im, sites, b

@op
def flood_fixed(image, seeds, barrier, tolerance=0., connectivity=4, dtype="float64"):
    im, sites, b = flood_inputs(image, seeds, barrier, dtype)
    tol = param(tolerance, minimum=0)
    neighbors(0, 0, *dims(b), connectivity)
    found = set()
    for y, x in sites:
        pred = lambda yy, xx: all(abs(Q(p[yy][xx])-Q(p[y][x])) <= tol for p in im)
        found |= reached([(y, x)], b, connectivity, vertex=pred)
    return [[int((y, x) in found) for x in range(len(b[0]))] for y in range(len(b))]

@op
def flood_neighbor(image, seeds, barrier, tolerance=0., connectivity=4, dtype="float64"):
    im, sites, b = flood_inputs(image, seeds, barrier, dtype)
    tol = param(tolerance, minimum=0)
    pred = lambda y, x, yy, xx: all(abs(Q(p[yy][xx])-Q(p[y][x])) <= tol for p in im)
    found = reached(sites, b, connectivity, edge=pred)
    return [[int((y, x) in found) for x in range(len(b[0]))] for y in range(len(b))]

@op
def flood_barrier(seeds, barrier, connectivity=4, dtype="float64"):
    s, b = grid(seeds, dtype, "binary"), grid(barrier, dtype, "binary"); same(s, b)
    sites = [(y, x) for y, x in coords(s) if s[y][x]]
    found = reached(sites, b, connectivity)
    return [[int((y, x) in found) for x in range(len(b[0]))] for y in range(len(b))]

def components(a, connectivity):
    h, w = dims(a)
    neighbors(0, 0, h, w, connectivity)
    unseen = {(y, x) for y, x in coords(a) if a[y][x]}
    groups = []
    while unseen:
        seed = min(unseen)
        unseen.remove(seed); queue = deque([seed]); group = [seed]
        while queue:
            y, x = queue.popleft()
            for site in neighbors(y, x, h, w, connectivity):
                if site in unseen:
                    unseen.remove(site); queue.append(site); group.append(site)
        groups.append(sorted(group))
    return groups

def labeling(input, connectivity, maximum_count, dtype, min_pixel):
    a = grid(input, dtype, "binary")
    h, w = dims(a); maximum_count = h*w if maximum_count is None else integer(maximum_count, "maximum_count", 0, h*w)
    groups = components(a, connectivity)
    if len(groups) > maximum_count: raise OracleError("component count exceeds declared domain cap")
    out = blank(a, 0)
    for i, group in enumerate(groups, 1):
        label = 1+group[0][0]*w+group[0][1] if min_pixel else i
        for y, x in group: out[y][x] = label
    return out

@op
def label_compact(input, connectivity=4, maximum_count=None, dtype="float64"):
    return labeling(input, connectivity, maximum_count, dtype, False)
@op
def label_min_pixel(input, connectivity=4, maximum_count=None, dtype="float64"):
    return labeling(input, connectivity, maximum_count, dtype, True)

def label_groups(labels):
    a = labels_grid(labels)
    groups = {}
    for y, x in coords(a):
        if a[y][x]: groups.setdefault(a[y][x], []).append((y, x))
    return a, sorted(groups.items())

@op
def component_count(labels, dtype=None):
    return [len(label_groups(labels)[1])]
@op
def component_areas(labels, dtype=None):
    return {"rows": [[id, len(sites)] for id, sites in label_groups(labels)[1]]}
@op
def component_bboxes(labels, dtype=None):
    return {"rows": [[id, min(y for y, x in sites), min(x for y, x in sites),
                      max(y for y, x in sites)+1, max(x for y, x in sites)+1]
                     for id, sites in label_groups(labels)[1]]}
@op
def component_bundle(input, connectivity=4, maximum_count=None, dtype="float64"):
    labels = label_min_pixel(input, connectivity, maximum_count, dtype)
    _, groups = label_groups(labels)
    return {"labels": [v for row in labels for v in row],
            "rows": [[id, len(sites), id-1, min(y for y, x in sites), min(x for y, x in sites),
                       max(y for y, x in sites)+1, max(x for y, x in sites)+1] for id, sites in groups]}
@op
def filter_area_index(labels, area_index, minimum_area, labels_id, output_dtype="float64", dtype=None):
    format_info(output_dtype)
    a, _ = label_groups(labels)
    minimum_area = integer(minimum_area, "minimum_area", 1)
    if not isinstance(area_index, dict) or not isinstance(labels_id, str) or not labels_id or area_index.get('source_id') != labels_id:
        raise OracleError("source association mismatch", "InvalidAssociation")
    expected = component_areas(labels)['rows']
    rows = area_index.get('rows')
    if not isinstance(rows, list) or any(not isinstance(r, list) or len(r) != 2 for r in rows):
        raise OracleError('area rows must be Kx2', 'TypeMismatch')
    for row in rows:
        for value in row: integer(value, 'area_index', 1)
    if rows != expected: raise OracleError("area rows incomplete, unsorted or incorrect", "InvalidAssociation")
    areas = dict(rows)
    return [[rn(int(v != 0 and areas[v] >= minimum_area), output_dtype) for v in row] for row in a]

def cleanup(input, foreground_connectivity, dtype, mode, threshold=None):
    a = grid(input, dtype, "binary")
    if foreground_connectivity not in (4, 8): raise OracleError("foreground connectivity")
    h, w = dims(a)
    out = [row[:] for row in a]
    if mode == 'remove':
        for group in components(a, foreground_connectivity):
            if len(group) <= threshold:
                for y, x in group: out[y][x] = outnum(0, dtype)
    else:
        bg = [[int(not v) for v in row] for row in a]
        for group in components(bg, 12-foreground_connectivity):
            if any(y in (0, h-1) or x in (0, w-1) for y, x in group): continue
            if threshold is None or len(group) <= threshold:
                for y, x in group: out[y][x] = outnum(1, dtype)
    return out
@op
def fill_holes(input, foreground_connectivity=4, dtype="float64"):
    return cleanup(input, foreground_connectivity, dtype, 'fill')
@op
def remove_small(input, maximum_removed_area, foreground_connectivity=4, dtype="float64"):
    return cleanup(input, foreground_connectivity, dtype, 'remove', integer(maximum_removed_area, "maximum_removed_area", 0))
@op
def fill_small_holes(input, maximum_area, foreground_connectivity=4, dtype="float64"):
    return cleanup(input, foreground_connectivity, dtype, 'fill', integer(maximum_area, "maximum_area", 0))

def residual(input, mode, footprint="square", radius=None, offsets=None, dtype="float64"):
    a = grid(input, dtype)
    bs = footprint_offsets(footprint, radius, offsets)
    if mode == 'gradient': left, right = morph_raw(a, bs, 'dilate'), morph_raw(a, bs, 'erode')
    elif mode == 'inner': left, right = a, morph_raw(a, bs, 'erode')
    elif mode == 'outer': left, right = morph_raw(a, bs, 'dilate'), a
    elif mode == 'white': left, right = a, morph_raw(a, bs, 'opening')
    else: left, right = morph_raw(a, bs, 'closing'), a
    return [[outnum(Q(l)-Q(r), dtype) for l, r in zip(lr, rr)] for lr, rr in zip(left, right)]
@op
def morph_gradient(input, **kw): return residual(input, 'gradient', **kw)
@op
def inner_border(input, **kw): return residual(input, 'inner', **kw)
@op
def outer_border(input, **kw): return residual(input, 'outer', **kw)
@op
def white_top_hat(input, **kw): return residual(input, 'white', **kw)
@op
def black_top_hat(input, **kw): return residual(input, 'black', **kw)

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

@op
def bridge_axis_gaps(barrier, maximum_gap, dtype='float64'):
    a = grid(barrier, dtype, 'binary')
    g = integer(maximum_gap, 'maximum_gap')
    h, w = dims(a); out = [[int(v) for v in row] for row in a]
    for line in [[(y, x) for x in range(w)] for y in range(h)]+[[(y, x) for y in range(h)] for x in range(w)]:
        ones = [i for i, (y, x) in enumerate(line) if a[y][x]]
        for left, right in zip(ones, ones[1:]):
            if 0 < right-left-1 <= g:
                for i in range(left+1, right):
                    y, x = line[i]; out[y][x] = 1
    return out
def seed_override_barrier(original, temporary, seeds, dtype):
    a,s=grid(original,dtype,'binary'),grid(seeds,dtype,'binary')
    same(a,s);same(a,temporary)
    filtered = [[float(bool(s[y][x] and not a[y][x])) for x in range(len(a[0]))] for y in range(len(a))]
    effective = [[float(bool(a[y][x] or (temporary[y][x] and not filtered[y][x])))
                  for x in range(len(a[0]))] for y in range(len(a))]
    return effective, filtered

def gap_fill_outputs(barrier, temporary, seeds, fill_connectivity, dtype, request):
    names=('fill','barrier','ignored_seeds','reopened_barrier')
    if request!='all' and request not in names:raise OracleError('invalid gap output request')
    a,s=grid(barrier,dtype,'binary'),grid(seeds,dtype,'binary')
    same(a,s)
    neighbors(0,0,*dims(a),fill_connectivity)
    effective,filtered=seed_override_barrier(a,temporary,s,dtype)
    outputs={}
    if request in ('all','fill'):outputs['fill']=flood_barrier(filtered,effective,fill_connectivity,dtype)
    if request in ('all','barrier'):outputs['barrier']=effective
    if request in ('all','ignored_seeds'):
        outputs['ignored_seeds']=[[float(bool(x and y)) for x,y in zip(ar,sr)] for ar,sr in zip(a,s)]
    if request in ('all','reopened_barrier'):
        outputs['reopened_barrier']=[[float(bool(x and y)) for x,y in zip(tr,sr)] for tr,sr in zip(temporary,filtered)]
    return outputs

@op
def fill_axis_gaps(barrier, seeds, maximum_gap, fill_connectivity=4, dtype='float64', request='all'):
    temporary = bridge_axis_gaps(barrier, maximum_gap, dtype)
    return gap_fill_outputs(barrier,temporary,seeds,fill_connectivity,dtype,request)
@op
def fill_morphological_gaps(barrier, seeds, fill_connectivity=4, footprint='square', radius=None, offsets=None, dtype='float64', request='all'):
    a = grid(barrier, dtype, 'binary')
    c = closing(a, footprint=footprint, radius=radius, offsets=offsets, dtype=dtype)
    temporary = [[int(x or y) for x, y in zip(ar, cr)] for ar, cr in zip(a, c)]
    return gap_fill_outputs(barrier,temporary,seeds,fill_connectivity,dtype,request)

@op
def affect_result(original, processed, mask, channels, dtype='float64'):
    a, b, m = tensor(original, dtype), tensor(processed, dtype), grid(mask, dtype)
    if len(a) != len(b): raise OracleError('channel count differs', 'TypeMismatch')
    same(a[0], b[0]); same(a[0], m)
    cs = channel_indices(channels, len(a))
    out = [[row[:] for row in p] for p in a]
    for c in cs:
        for y, x in coords(m):
            v = Q(m[y][x])
            out[c][y][x] = rn((1-v)*finite(a[c][y][x])+v*finite(b[c][y][x]), dtype)
    return out
@op
def multiply_mask(image, mask, channels, dtype='float64'):
    a, m = tensor(image, dtype), grid(mask, dtype); same(a[0], m)
    cs = channel_indices(channels, len(a)); out = [[row[:] for row in p] for p in a]
    for c in cs:
        for y, x in coords(m):
            v = Q(m[y][x])
            out[c][y][x] = rn(v*finite(a[c][y][x]), dtype)
    return out
@op
def restricted_mean(image, mask, footprint='square', radius=None, offsets=None, empty_policy='zero', dtype='float64', request='both'):
    a, m = tensor(image, dtype), grid(mask, dtype); same(a[0], m)
    bs = footprint_offsets(footprint, radius, offsets)
    if empty_policy not in ('zero', 'error') or request not in ('both', 'values', 'valid'):
        raise OracleError('empty policy/request')
    h, w = dims(m); valid = blank(m, 0)
    out = [blank(m, 0.) for _ in a]
    for y, x in coords(m):
        sites = [(y+dy, x+dx) for dy, dx in bs if 0 <= y+dy < h and 0 <= x+dx < w]
        total = sum((Q(m[yy][xx]) for yy, xx in sites), Q(0))
        valid[y][x] = int(total > 0)
        if request == 'valid': continue
        numerators = [sum((Q(m[yy][xx])*finite(a[c][yy][xx]) for yy, xx in sites), Q(0))
                      for c in range(len(a))]
        if not total:
            if empty_policy == 'error': raise OracleError('zero restricted denominator')
            continue
        for c, numerator in enumerate(numerators):
            out[c][y][x] = rn(numerator/total, dtype)
    return {'valid': valid} if request == 'valid' else ({'values': out} if request == 'values' else dict(values=out, valid=valid))
@op
def apply_alpha(image, mask, alpha_channel, dtype='float64'):
    a, m = tensor(image, dtype), grid(mask, dtype); same(a[0], m)
    c = integer(alpha_channel, 'alpha_channel', 0, len(a)-1)
    out = [[row[:] for row in p] for p in a]
    for y, x in coords(m):
        alpha = finite(a[c][y][x])
        if not 0 <= alpha <= 1: raise OracleError('alpha outside [0,1]')
        out[c][y][x] = a[c][y][x] if m[y][x] == 1 else rn(alpha*Q(m[y][x]), dtype)
    return out


@op
def lab2000_range(image,target,channels='0,1,2',kL=1.,kC=1.,kH=1.,inner=0.,outer=1.,curve='smoothstep',dtype='float64'):
    from ciede2000 import selector
    return selector(image,target,channels,kL,kC,kH,inner,outer,curve,dtype)


@op
def fit_color_groups_table(samples,group_ids,weights,epsilon,dtype='float64'):
    from color_statistics import fit
    return fit(samples,group_ids,weights,epsilon,dtype)

@op
def fit_color_groups_image(image,group_ids,weights,epsilon,channels,dtype='float64'):
    from color_statistics import fit_image
    return fit_image(image,group_ids,weights,epsilon,channels,dtype)

@op
def apply_color_groups(image,model,channels,inner,outer,curve='smoothstep',dtype='float64'):
    from color_statistics import apply
    return apply(image,model,channels,inner,outer,curve,dtype)


def evaluate(case):
    """Portable oracle case, not WorkflowDocument: {op, inputs, params, dtype}."""
    name = case.get('op')
    if name not in REGISTRY: raise OracleError(f'unknown oracle operation {name}')
    args = dict(case.get('inputs', {})); params = dict(case.get('params', {}))
    if args.keys() & params.keys(): raise OracleError('duplicate input/parameter field')
    args.update(params)
    args.setdefault('dtype', case.get('dtype', 'float64'))
    return REGISTRY[name](**args)
