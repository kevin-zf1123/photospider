"""Strict mathematical query-replication reference for CRV-05C.

This is an independent comparison oracle, not an additional strict runtime key.
The native implementation uses normalized positioning and tolerance comparison.
Ports and returned hexadecimal bit patterns follow crv05.reference's format.
"""
import math
from .reference import reference, number, rn


def vector_reference(dtype, policy, sample_shape, length, columns, coords, ports):
    """Evaluate selected S+[C] coordinates; singleton reads only table payload."""
    if not coords:
        return ""
    if length == 1:
        table_dtype, raw = ports[1]
        sign = 31 if table_dtype == 4 else 63
        return "".join(f"{rn(number(raw[at[-1]], table_dtype), dtype, bool(raw[at[-1]] >> sign)):x} "
                       for at in coords)
    query_dtype, raw_query = ports[0]
    if len(raw_query) != math.prod(sample_shape):
        raise ValueError("query shape")
    repeated = [value for value in raw_query for _ in range(columns)]
    return reference(True, dtype, policy, list(sample_shape)+[columns],
                     length, columns, coords,
                     [(query_dtype, repeated), ports[1], ports[2]])


def reference_cases():
    """Small authored anchors reusable by future implementation drivers."""
    from fractions import Fraction as F
    for dtype in (3, 4):
        enc = lambda xs: [rn(F(x), dtype) for x in xs]
        ports = [(dtype, enc([0, F(1, 4), 1])),
                 (dtype, enc([0, 10, 0, 2, 8, 4])),
                 (3, [rn(F(0)), rn(F(1)), rn(F(1))])]
        coords = [(r, c) for r in range(3) for c in range(3)]
        case = (dtype, 1, [3], 2, 3, coords, ports)
        yield case, vector_reference(*case)
        negative_zero = 1 << (31 if dtype == 4 else 63)
        singleton = [None, (dtype, [rn(F(7), dtype), negative_zero, rn(F(4), dtype)]), None]
        case = (dtype, 1, [2], 1, 3, [(0, 1), (1, 0), (1, 2)], singleton)
        yield case, vector_reference(*case)
