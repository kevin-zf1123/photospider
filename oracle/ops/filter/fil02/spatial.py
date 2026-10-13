from __future__ import annotations
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.fil01.spatial import convolve2d

def separable(image,kx,ky,anchor=(0,0),**kwargs):
    # This *exact* outer product deliberately is NOT a Float64 kernel tensor.
    kernel=[[frac(y)*frac(x) for x in kx] for y in ky]
    return convolve2d(image,kernel,anchor,**kwargs)
