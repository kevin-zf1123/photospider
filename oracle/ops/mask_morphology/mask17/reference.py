from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import finite
from oracle.ops.mask_morphology.common.exact import integer
from oracle.ops.mask_morphology.common.exact import rn
from oracle.ops.mask_morphology.common.reference import blank
from oracle.ops.mask_morphology.common.reference import channel_indices
from oracle.ops.mask_morphology.common.reference import coords
from oracle.ops.mask_morphology.common.reference import dims
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import same
from oracle.ops.mask_morphology.common.reference import tensor
from oracle.ops.mask_morphology.mask05.reference import footprint_offsets
from oracle.ops.mask_morphology.common.registration import op

@op
def affect_result(original, processed, mask, channels, dtype='float64'):
    a, b, m = tensor(original, dtype), tensor(processed, dtype), grid(mask, dtype)
    if len(a) != len(b): raise OracleError('channel count differs', 'TypeMismatch')
    same(a[0], b[0]); same(a[0], m)
    cs = channel_indices(channels, len(a))
    out = [[row[:] for row in p] for p in a]
    for c in cs:
        for y, x in coords(m):
            v = Q(m[y][x])
            out[c][y][x] = rn((1-v)*finite(a[c][y][x])+v*finite(b[c][y][x]), dtype)
    return out
@op
def multiply_mask(image, mask, channels, dtype='float64'):
    a, m = tensor(image, dtype), grid(mask, dtype); same(a[0], m)
    cs = channel_indices(channels, len(a)); out = [[row[:] for row in p] for p in a]
    for c in cs:
        for y, x in coords(m):
            v = Q(m[y][x])
            out[c][y][x] = rn(v*finite(a[c][y][x]), dtype)
    return out
@op
def restricted_mean(image, mask, footprint='square', radius=None, offsets=None, empty_policy='zero', dtype='float64', request='both'):
    a, m = tensor(image, dtype), grid(mask, dtype); same(a[0], m)
    bs = footprint_offsets(footprint, radius, offsets)
    if empty_policy not in ('zero', 'error') or request not in ('both', 'values', 'valid'):
        raise OracleError('empty policy/request')
    h, w = dims(m); valid = blank(m, 0)
    out = [blank(m, 0.) for _ in a]
    for y, x in coords(m):
        sites = [(y+dy, x+dx) for dy, dx in bs if 0 <= y+dy < h and 0 <= x+dx < w]
        total = sum((Q(m[yy][xx]) for yy, xx in sites), Q(0))
        valid[y][x] = int(total > 0)
        if request == 'valid': continue
        numerators = [sum((Q(m[yy][xx])*finite(a[c][yy][xx]) for yy, xx in sites), Q(0))
                      for c in range(len(a))]
        if not total:
            if empty_policy == 'error': raise OracleError('zero restricted denominator')
            continue
        for c, numerator in enumerate(numerators):
            out[c][y][x] = rn(numerator/total, dtype)
    return {'valid': valid} if request == 'valid' else ({'values': out} if request == 'values' else dict(values=out, valid=valid))
@op
def apply_alpha(image, mask, alpha_channel, dtype='float64'):
    a, m = tensor(image, dtype), grid(mask, dtype); same(a[0], m)
    c = integer(alpha_channel, 'alpha_channel', 0, len(a)-1)
    out = [[row[:] for row in p] for p in a]
    for y, x in coords(m):
        alpha = finite(a[c][y][x])
        if not 0 <= alpha <= 1: raise OracleError('alpha outside [0,1]')
        out[c][y][x] = a[c][y][x] if m[y][x] == 1 else rn(alpha*Q(m[y][x]), dtype)
    return out
