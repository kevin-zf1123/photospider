from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import Quadratic
from oracle.ops.mask_morphology.common.exact import curve01
from oracle.ops.mask_morphology.common.exact import finite
from oracle.ops.mask_morphology.common.exact import param
from oracle.ops.mask_morphology.common.exact import rn
from oracle.ops.mask_morphology.common.exact import sqrt_exact
from oracle.ops.mask_morphology.common.reference import blank
from oracle.ops.mask_morphology.common.reference import channel_indices
from oracle.ops.mask_morphology.common.reference import coords
from oracle.ops.mask_morphology.common.reference import tensor
from oracle.ops.mask_morphology.mask07.reference import radii
from oracle.ops.mask_morphology.common.registration import op

def radial(distance, inner, outer, curve, dtype):
    def cmp(v):
        return distance.compare(v) if isinstance(distance, Quadratic) else (distance > v)-(distance < v)
    if inner == outer: return rn(int(cmp(inner) <= 0), dtype)
    if cmp(inner) <= 0: return rn(1, dtype)
    if cmp(outer) >= 0: return 0.0
    return rn(1-curve01((distance-inner)/(outer-inner), curve), dtype)

@op
def coordinate_range(image, target, scales, channels, inner=0., outer=1., curve="smoothstep", dtype="float64"):
    image = tensor(image, dtype)
    cs = channel_indices(channels, len(image))
    if len(target) != len(cs) or len(scales) != len(cs): raise OracleError("target/scales shape", "TypeMismatch")
    ts = [param(t) for t in target]; ss = [param(s, minimum=0, strict=True) for s in scales]
    i, o = radii(inner, outer, curve)
    out = blank(image[0], 0.)
    for y, x in coords(out):
        sq = sum(((finite(image[c][y][x])-t)/s)**2 for c, t, s in zip(cs, ts, ss))
        out[y][x] = radial(sqrt_exact(sq), i, o, curve, dtype)
    return out

@op
def lab76_range(image, target, channels="0,1,2", inner=0., outer=1., curve="smoothstep", dtype="float64"):
    image = tensor(image, dtype)
    cs = channel_indices(channels, len(image))
    if len(cs) != 3 or len(target) != 3: raise OracleError("Lab needs 3 coordinates", "TypeMismatch")
    ts = [param(t) for t in target]; i, o = radii(inner, outer, curve)
    out = blank(image[0], 0.)
    for y, x in coords(out):
        delta = [finite(image[c][y][x])-t for c, t in zip(cs, ts)]
        sq = (100*delta[0])**2+delta[1]**2+delta[2]**2
        out[y][x] = radial(sqrt_exact(sq), i, o, curve, dtype)
    return out

@op
def hue_range(image, channels="0,1", center_turns=0., minimum_chroma=0., neutral_policy="exclude", inner=0., outer=.5, curve="smoothstep", dtype="float64"):
    image = tensor(image, dtype)
    cs = channel_indices(channels, len(image))
    if len(cs) != 2: raise OracleError("hue,chroma selector required")
    h0, minc = param(center_turns), param(minimum_chroma, minimum=0)
    i, o = radii(inner, outer, curve)
    if o > Q(1,2) or neutral_policy not in ("include", "exclude"): raise OracleError("hue parameters")
    out = blank(image[0], 0.)
    for y, x in coords(out):
        h, c = finite(image[cs[0]][y][x]), finite(image[cs[1]][y][x])
        if c == 0 or c < minc:
            out[y][x] = rn(int(neutral_policy == "include"), dtype)
        else:
            delta = h-h0
            delta -= (delta+Q(1,2)).numerator // (delta+Q(1,2)).denominator
            out[y][x] = radial(abs(delta), i, o, curve, dtype)
    return out


@op
def lab2000_range(image,target,channels='0,1,2',kL=1.,kC=1.,kH=1.,inner=0.,outer=1.,curve='smoothstep',dtype='float64'):
    from oracle.ops.mask_morphology.mask04.ciede2000 import selector
    return selector(image,target,channels,kL,kC,kH,inner,outer,curve,dtype)


@op
def fit_color_groups_table(samples,group_ids,weights,epsilon,dtype='float64'):
    from oracle.ops.mask_morphology.mask04.color_statistics import fit
    return fit(samples,group_ids,weights,epsilon,dtype)

@op
def fit_color_groups_image(image,group_ids,weights,epsilon,channels,dtype='float64'):
    from oracle.ops.mask_morphology.mask04.color_statistics import fit_image
    return fit_image(image,group_ids,weights,epsilon,channels,dtype)

@op
def apply_color_groups(image,model,channels,inner,outer,curve='smoothstep',dtype='float64'):
    from oracle.ops.mask_morphology.mask04.color_statistics import apply
    return apply(image,model,channels,inner,outer,curve,dtype)
