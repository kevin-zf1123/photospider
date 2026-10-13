from __future__ import annotations
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.registration import op

@op
def gaussian_feather(input, sigma_y=1., sigma_x=1., radius_y=4, radius_x=4, boundary="zero", dtype="float64"):
    from oracle.ops.mask_morphology.mask08.gaussian import certified_gaussian
    return certified_gaussian(grid(input, dtype), sigma_y, sigma_x, radius_y, radius_x, boundary, dtype)
