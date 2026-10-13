from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import outnum
from oracle.ops.mask_morphology.mask05.reference import footprint_offsets
from oracle.ops.mask_morphology.mask05.reference import morph_raw
from oracle.ops.mask_morphology.common.registration import op

def residual(input, mode, footprint="square", radius=None, offsets=None, dtype="float64"):
    a = grid(input, dtype)
    bs = footprint_offsets(footprint, radius, offsets)
    if mode == 'gradient': left, right = morph_raw(a, bs, 'dilate'), morph_raw(a, bs, 'erode')
    elif mode == 'inner': left, right = a, morph_raw(a, bs, 'erode')
    elif mode == 'outer': left, right = morph_raw(a, bs, 'dilate'), a
    elif mode == 'white': left, right = a, morph_raw(a, bs, 'opening')
    else: left, right = morph_raw(a, bs, 'closing'), a
    return [[outnum(Q(l)-Q(r), dtype) for l, r in zip(lr, rr)] for lr, rr in zip(left, right)]
@op
def morph_gradient(input, **kw): return residual(input, 'gradient', **kw)
@op
def inner_border(input, **kw): return residual(input, 'inner', **kw)
@op
def outer_border(input, **kw): return residual(input, 'outer', **kw)
@op
def white_top_hat(input, **kw): return residual(input, 'white', **kw)
@op
def black_top_hat(input, **kw): return residual(input, 'black', **kw)
