from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import outnum
from oracle.ops.mask_morphology.common.registration import op

@op
def invert(input, dtype="float64"):
    return [[outnum(1-Q(v), dtype) for v in row] for row in grid(input, dtype)]
