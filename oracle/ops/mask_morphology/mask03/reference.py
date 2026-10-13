from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import clip01
from oracle.ops.mask_morphology.common.exact import curve01
from oracle.ops.mask_morphology.common.exact import format_info
from oracle.ops.mask_morphology.common.exact import param
from oracle.ops.mask_morphology.common.exact import rn
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import outnum
from oracle.ops.mask_morphology.common.registration import op

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
