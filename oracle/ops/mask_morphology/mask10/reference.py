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
from oracle.ops.mask_morphology.mask07.reference import metric_value
from oracle.ops.mask_morphology.common.registration import op
import math

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
