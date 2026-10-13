"""Small-image mathematical references for every MASK member.

Pure Python exact arithmetic, deliberately slow connectivity/nearest-site
oracles. Not a production kernel, metadata codec or resource/demand simulator.
See README.md and the normative family documents for the execution boundary.
"""

from __future__ import annotations
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import atom
from oracle.ops.mask_morphology.common.exact import finite
from oracle.ops.mask_morphology.common.exact import integer
from oracle.ops.mask_morphology.common.exact import rn
import math
import re

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
