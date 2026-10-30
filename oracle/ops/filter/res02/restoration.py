from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.filter.common.bounds import Context, exp_q, certify
from oracle.ops.filter.common.core import DomainError
from oracle.ops.filter.common.core import coordinate
from oracle.ops.filter.common.core import frac
from oracle.ops.filter.common.core import rn
from oracle.ops.filter.common.core import same_shape
from oracle.ops.filter.common.core import zeros
from oracle.ops.filter.fil07.spatial import clipped_box
import copy

def _nlm_reference(image,guide,*,_round,patch_radius=1,search_radius=1,h_parameter=1,metric_scale=None,noise_sigma=None,dtype='float64'):
    h,w=same_shape(image,guide);hp=frac(h_parameter)
    if min(patch_radius,search_radius)<0 or hp<=0:raise DomainError('NLM parameters')
    sigma=Q() if noise_sigma is None else frac(noise_sigma)
    if sigma<0:raise DomainError('sigma')
    if search_radius==0:return copy.deepcopy(image)
    vector=isinstance(guide[0][0],(list,tuple));gdim=len(guide[0][0]) if vector else 1
    scales=[Q(1)]*gdim if metric_scale is None else list(map(frac,metric_scale))
    if not 1<=gdim<=4 or len(scales)!=gdim or min(scales)<=0:raise DomainError('guide metric')
    def g(y,x):
        yy,xx=coordinate(y,x,h,w,'reflect_half')
        return [frac(guide[yy][xx][c] if vector else guide[yy][xx])*scales[c] for c in range(gdim)]
    out=zeros(h,w);npatch=(2*patch_radius+1)**2*gdim
    for y in range(h):
        for x in range(w):
            weighted_samples=[]
            for j,i in clipped_box(y,x,h,w,search_radius,search_radius):
                if (j,i)==(y,x):argument=Q()
                else:
                    distance=Q()
                    for dy in range(-patch_radius,patch_radius+1):
                        for dx in range(-patch_radius,patch_radius+1):
                            a,b=g(y+dy,x+dx),g(j+dy,i+dx)
                            distance+=sum(((v-u)**2 for v,u in zip(a,b)),Q())
                    distance/=npatch
                    if noise_sigma is not None:distance=max(distance-2*sigma*sigma,0)
                    argument=-distance/(hp*hp)
                weighted_samples.append((argument,frac(image[j][i])))
            # Cancel a common exact mean across equal-exponent groups. This
            # also resolves rational IEEE midpoints without guessing a bit.
            groups = {}
            for argument, value in weighted_samples:
                count, total = groups.get(argument, (0, Q()))
                groups[argument] = (count + 1, total + value)
            count0, total0 = next(iter(groups.values()))
            common_mean = all(total * count0 == total0 * count
                              for count, total in groups.values())

            def expression(ctx):
                if common_mean:
                    return ctx.q(total0 / count0)
                num,den=ctx.q(0),ctx.q(0)
                for argument,value in weighted_samples:
                    weight=ctx.q(argument).exp()
                    num=num+weight*ctx.q(value)
                    den=den+weight
                return num/den
            out[y][x]=_round(expression,dtype)
    return out


def nlm(image, guide, **kwargs):
    """Mathematical reference with directed-MPFR certified final rounding.

    Parameter conversion is explicit: pass working-dtype parameters when
    comparing a native implementation. Unresolved midpoint cases raise the
    oracle's Inconclusive result, never a guessed final bit pattern.
    """
    return _nlm_reference(image, guide, _round=certify, **kwargs)


def nlm_enclosure(image, guide, *, precision=256, **kwargs):
    """Binary64 outward bounds on the high-precision mathematical result.

    These enclosures remain useful for native error checks when an extremely
    small nonzero weight prevents certification of one final rounding bit.
    They are mathematical bounds, not native-execution goldens.
    """
    import ctypes
    def enclose(expression, dtype):
        del dtype
        with Context(precision) as context:
            result = expression(context)
            return (context.lib.mpfr_get_d(ctypes.byref(result.lo), context.DOWN),
                    context.lib.mpfr_get_d(ctypes.byref(result.hi), context.UP))
    if kwargs.get('search_radius', 1) == 0:
        return [[enclose(lambda ctx, value=v: ctx.q(frac(value)), None)
                 for v in row] for row in image]
    return _nlm_reference(image, guide, _round=enclose, **kwargs)


def nlm_native(image, guide, *, patch_radius=1, search_radius=1,
               h_parameter=1, metric_scale=None, noise_sigma=None,
               dtype='float64'):
    """Independent scalar staged-native reference for RES-02.

    Each finite elementary operation is rounded directly from an exact
    rational to the selected dtype. Exponentials are independently certified
    with directed MPFR intervals. The direct candidate/patch loops deliberately
    do not reuse the production displacement or separable-box algorithm.
    Native FMA is used in the SSD and numerator reductions. Different legal
    native reduction groupings need a conditioning-aware comparison, not
    exact equality with this reference. The original nlm remains the
    higher-precision mathematical reference. Arithmetic comparisons cover
    numerical values and classifications, not NaN payloads or every zero sign;
    the search-radius-zero path separately preserves copy bit patterns.
    """
    import math

    height, width = same_shape(image, guide)
    if min(patch_radius, search_radius) < 0:
        raise DomainError('NLM radii')
    if search_radius == 0:
        return copy.deepcopy(image)

    def cast(value):
        return rn(value, dtype)

    def binary(a, b, op):
        if math.isfinite(a) and math.isfinite(b):
            if op == 'add':
                value = frac(a) + frac(b)
            elif op == 'sub':
                value = frac(a) - frac(b)
            elif op == 'mul':
                value = frac(a) * frac(b)
            elif b:
                value = frac(a) / frac(b)
            else:
                value = float('nan') if not a else math.copysign(
                    float('inf'), a * math.copysign(1.0, b))
            return cast(value)
        if op == 'add':
            return cast(a + b)
        if op == 'sub':
            return cast(a - b)
        if op == 'mul':
            return cast(a * b)
        if b == 0:
            return float('nan') if math.isnan(a) else math.copysign(
                float('inf'), a * math.copysign(1.0, b))
        return cast(a / b)

    def fma(a, b, c):
        if all(map(math.isfinite, (a, b, c))):
            return cast(frac(a) * frac(b) + frac(c))
        if any(map(math.isnan, (a, b, c))):
            return float('nan')
        if math.isinf(a) or math.isinf(b):
            if a == 0 or b == 0:
                return float('nan')
            product = a * b
            if math.isinf(c) and product != c:
                return float('nan')
            return product
        # A finite exact product cannot cancel an infinite addend, even when
        # an unfused floating-point multiplication would overflow first.
        return c

    def exponential(value):
        if math.isnan(value):
            return value
        if math.isinf(value):
            return 0.0 if value < 0 else value
        return exp_q(frac(value), dtype)

    hp = cast(h_parameter)
    sigma = cast(0 if noise_sigma is None else noise_sigma)
    if not hp > 0 or not sigma >= 0:
        raise DomainError('NLM parameter domain')
    vector = isinstance(guide[0][0], (list, tuple))
    components = len(guide[0][0]) if vector else 1
    scales = list(map(cast, [1] * components if metric_scale is None else metric_scale))
    if not 1 <= components <= 4 or len(scales) != components or min(scales) <= 0:
        raise DomainError('guide metric')
    h2 = binary(hp, hp, 'mul')
    correction = binary(2.0, binary(sigma, sigma, 'mul'), 'mul')
    taps = cast(2 * patch_radius + 1)
    normalizer = binary(binary(taps, taps, 'mul'), cast(components), 'mul')

    def value(y, x, component):
        yy, xx = coordinate(y, x, height, width, 'reflect_half')
        return cast(guide[yy][xx][component] if vector else guide[yy][xx])

    out = zeros(height, width)
    for y in range(height):
        for x in range(width):
            numerator, denominator = cast(image[y][x]), 1.0
            for j, i in clipped_box(y, x, height, width, search_radius, search_radius):
                if (j, i) == (y, x):
                    continue
                distance = 0.0
                for dy in range(-patch_radius, patch_radius + 1):
                    for dx in range(-patch_radius, patch_radius + 1):
                        for c in range(components):
                            delta = binary(value(y + dy, x + dx, c),
                                           value(j + dy, i + dx, c), 'sub')
                            delta = binary(delta, scales[c], 'mul')
                            distance = fma(delta, delta, distance)
                distance = binary(distance, normalizer, 'div')
                if noise_sigma is not None:
                    distance = max(binary(distance, correction, 'sub'), 0.0)
                weight = exponential(-binary(distance, h2, 'div'))
                if weight != 0:
                    numerator = fma(weight, cast(image[j][i]), numerator)
                    denominator = binary(denominator, weight, 'add')
            out[y][x] = binary(numerator, denominator, 'div')
    return out
