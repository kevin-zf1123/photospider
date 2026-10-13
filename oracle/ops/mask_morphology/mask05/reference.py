from __future__ import annotations
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import integer
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import mapped
from oracle.ops.mask_morphology.common.registration import op
import re

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
