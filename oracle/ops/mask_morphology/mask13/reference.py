from __future__ import annotations
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import integer
from oracle.ops.mask_morphology.common.reference import dims
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import outnum
from oracle.ops.mask_morphology.mask12.reference import components
from oracle.ops.mask_morphology.common.registration import op

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
