"""Small exact special-value references for NUM-compatible filter expressions.

Finite expressions stay Fraction until direct output rounding. Python floats are
used only as decoded IEEE operands/classifications, never finite accumulators.
"""
import math
from .core import Q, frac, rn, DomainError, round_integer_even


def is_nan(x):
    return isinstance(x, float) and math.isnan(x)


def is_inf(x):
    return isinstance(x, float) and math.isinf(x)


def negative(x):
    return math.copysign(1, x) < 0 if isinstance(x, float) else x < 0


def sum_products(terms):
    """Unrounded reduction with input NaN priority over generated invalid NaNs."""
    terms = [tuple(term) for term in terms]
    for term in terms:
        for value in term:
            if is_nan(value):
                return rn(value)
    finite = Q()
    infinities = set()
    all_negative_zero = bool(terms)
    invalid = False
    for term in terms:
        sign = sum(negative(v) for v in term) % 2
        inf = any(is_inf(v) for v in term)
        zero = any(v == 0 for v in term)
        if inf:
            if zero:
                invalid = True
            else:
                infinities.add(sign)
            all_negative_zero = False
        else:
            product = Q(1)
            for value in term:
                product *= frac(value)
            finite += product
            all_negative_zero &= product == 0 and bool(sign)
    if invalid or len(infinities) == 2:
        return float('nan')
    if infinities:
        return -float('inf') if 1 in infinities else float('inf')
    return -0.0 if all_negative_zero else finite


def divide_positive(value, denominator, dtype):
    """Positive finite exact denominator; no extra intermediate rounding."""
    if is_nan(value) or is_inf(value):
        return rn(value, dtype)
    if value == 0 and negative(value):
        return -0.0
    return rn(frac(value) / denominator, dtype)


def quantile(values, q, interpolation, dtype):
    """NUM rank NaNs/ties/infinity interpolation, original input order retained."""
    values = list(values)
    for value in values:
        if is_nan(value):
            return rn(value, dtype)
    values.sort()  # numeric stable ordering, including equal signed zeros
    pos = (len(values)-1)*q
    i = pos.numerator // pos.denominator
    j = -((-pos.numerator)//pos.denominator)
    weight = pos-i
    if interpolation == 'lower':
        return values[i]
    if interpolation == 'higher':
        return values[j]
    if interpolation == 'nearest_even':
        return values[round_integer_even(pos)]
    if i == j:
        return values[i]
    if interpolation == 'midpoint':
        weight = Q(1, 2)
    a, b = values[i], values[j]
    if is_inf(a) or is_inf(b):
        if is_inf(a) and is_inf(b) and negative(a) != negative(b):
            return rn(float('nan'), dtype)
        return a if is_inf(a) else b
    if a == b == 0 and negative(a) and negative(b):
        return -0.0
    return rn((1-weight)*frac(a)+weight*frac(b), dtype)


def fused_color(colors, alphas, weights, *, dtype='float64', alpha_only=False):
    """One component/window of FIL-01D; geometry/metadata are outside this helper."""
    weights = list(map(frac, weights))
    if any(w < 0 for w in weights) or sum(weights) <= 0:
        raise DomainError('positive kernel required')
    if len(alphas) != len(weights) or (not alpha_only and len(colors) != len(weights)):
        raise DomainError('window lengths differ')
    active = []
    for i, w in enumerate(weights):
        if not w:
            continue
        a = frac(alphas[i])
        if not 0 <= a <= 1:
            raise DomainError('coverage range')
        active.append((i, w, a))
    total = sum(weights, Q())
    coverage = sum((w*a for _, w, a in active), Q())
    alpha = rn(coverage/total, dtype)
    if alpha_only:
        return alpha
    numerator = sum_products((w, a, colors[i]) for i, w, a in active)
    if is_nan(numerator):
        color = rn(numerator, dtype)
    elif coverage:
        color = divide_positive(numerator, coverage, dtype)
    else:
        color = 0.0
    return color, alpha


def rl_ratio(observed, mask, prediction, epsilon, *, dtype='float64'):
    """Scalar RL ratio; explicit zero-observation rule does not swallow NaN."""
    mask, epsilon = frac(mask), frac(epsilon)
    if not 0 <= mask <= 1 or epsilon < 0:
        raise DomainError('RL control domain')
    if not mask:
        return 0.0
    if is_nan(observed):
        return rn(observed, dtype)
    if observed < 0:
        raise DomainError('negative observation')
    if is_nan(prediction):
        return rn(prediction, dtype)
    denominator = prediction if is_inf(prediction) else frac(prediction)+epsilon
    if denominator == 0:
        if observed == 0:
            return 0.0
        raise DomainError('positive observation with zero prediction')
    if is_inf(observed):
        return rn(float('nan') if is_inf(denominator) else float('inf'), dtype)
    if is_inf(denominator):
        return 0.0
    return rn(mask*frac(observed)/denominator, dtype)


def exceptional_simplex(values, *, dtype='float64'):
    """Exceptional simplex projection; None means use exact finite projection.

    A nonfinite projection argument has no feasible Euclidean projection. NUM
    invalid arithmetic is a successful NaN result, not a rational certificate.
    """
    values = list(values)
    first = next((v for v in values if is_nan(v)), None)
    if first is not None:
        return [rn(first, dtype)]*len(values)
    if any(is_inf(v) for v in values):
        return [rn(float('nan'), dtype)]*len(values)
    return None
