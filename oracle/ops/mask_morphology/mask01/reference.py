from __future__ import annotations
from fractions import Fraction as Q
from oracle.ops.mask_morphology.common.exact import OracleError
from oracle.ops.mask_morphology.common.exact import rn
from oracle.ops.mask_morphology.common.reference import grid
from oracle.ops.mask_morphology.common.reference import outnum
from oracle.ops.mask_morphology.common.reference import same
from oracle.ops.mask_morphology.common.registration import op

@op
def binary_logic(a, b, operation="and", dtype="float64"):
    a, b = grid(a, dtype, "binary"), grid(b, dtype, "binary")
    same(a, b)
    if operation not in ("and", "or", "xor", "subtract"):
        raise OracleError("invalid Boolean operation")
    def calc(x, y):
        return {"and": x and y, "or": x or y, "xor": x != y,
                "subtract": x and not y}[operation]
    return [[outnum(int(bool(calc(x, y))), dtype) for x, y in zip(ar, br)]
            for ar, br in zip(a, b)]

@op
def fuzzy_logic(a, b, operation="and", dtype="float64"):
    if dtype == "uint8": raise OracleError("soft algebra needs Float32/64", "TypeMismatch")
    a, b = grid(a, dtype), grid(b, dtype)
    same(a, b)
    if operation not in ("and", "or", "xor", "subtract"): raise OracleError("operation")
    def calc(x, y):
        if operation == "and": return x if x <= y else y
        if operation == "or": return x if x >= y else y
        return rn(abs(Q(x)-Q(y)) if operation == "xor" else min(Q(x), 1-Q(y)), dtype)
    return [[calc(x, y) for x, y in zip(ar, br)] for ar, br in zip(a, b)]

@op
def independent_coverage(a, b, operation="and", dtype="float64"):
    if dtype == "uint8": raise OracleError("soft algebra needs Float32/64", "TypeMismatch")
    a, b = grid(a, dtype), grid(b, dtype); same(a, b)
    if operation not in ("and", "or", "xor", "subtract"): raise OracleError("operation")
    def calc(x, y):
        x, y = Q(x), Q(y)
        return rn({"and": x*y, "or": x+y-x*y, "xor": x+y-2*x*y,
                   "subtract": x*(1-y)}[operation], dtype)
    return [[calc(x, y) for x, y in zip(ar, br)] for ar, br in zip(a, b)]
