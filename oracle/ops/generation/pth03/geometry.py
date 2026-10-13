from __future__ import annotations
from fractions import Fraction as F
from oracle.ops.generation.common.exact import sqrt_bounds
from oracle.ops.generation.common.geometry import norm2
from oracle.ops.generation.common.geometry import point
from oracle.ops.generation.common.geometry import sub
from oracle.ops.generation.common.geometry import split_bezier


def bezier_length_bounds(controls,depth=6,precision_bits=160):
    """Certified lower/upper rational bounds; not necessarily one RN cell."""
    if not 0<=depth<=18:raise ValueError('reference depth cap')
    def leaf(cp):
        lower=sqrt_bounds(norm2(sub(cp[-1],cp[0])),precision_bits)[0]
        upper=sum((sqrt_bounds(norm2(sub(b,a)),precision_bits)[1] for a,b in zip(cp,cp[1:])),F(0))
        return lower,upper
    def visit(cp,d):
        if not d:return leaf(cp)
        a,b=split_bezier(cp);la,ua=visit(a,d-1);lb,ub=visit(b,d-1)
        return la+lb,ua+ub
    return visit(list(map(point,controls)),depth)
