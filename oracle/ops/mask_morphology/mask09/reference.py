from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.exact import clip01
from oracle.ops.mask_morphology.common.exact import curve01
from oracle.ops.mask_morphology.common.exact import param
from oracle.ops.mask_morphology.common.exact import rn
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.registration import op
import math

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
