from __future__ import annotations
from collections import deque
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import param
from oracle.ops.mask_morphology.common.reference import coords
from oracle.ops.mask_morphology.common.reference import dims
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import same
from oracle.ops.mask_morphology.common.reference import tensor
from oracle.ops.mask_morphology.common.registration import op

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
