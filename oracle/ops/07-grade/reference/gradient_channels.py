"""Independent staged strict reference for both GRD-06C registrations.

Inputs to this mathematical layer are already selected scalar queries. Each
control pair has its own knots, values and method. The runtime's native profile
is compared with tolerances; this module retains exact equations and every
published Float64-grid / U-table / U-output rounding boundary.
"""
from bisect import bisect_left
from . import curves
from .precision import DomainError, F, frac, finite_copy, round_fraction


def vector_lookup(query, table, axis, edge="clamp", dtype=64):
    """Strict rounded-grid comparison for one shared query and all columns."""
    length = len(table)
    if length == 1:
        return [finite_copy(v, dtype) for v in table[0]]
    a, b = map(frac, axis[:2])
    grid = [a] + [frac(round_fraction(((length-1-j)*a+j*b)/(length-1), 64))
                  for j in range(1, length-1)] + [b]
    descending = b < a
    ordered = [-x if descending else x for x in grid]
    q = frac(query)
    key = -q if descending else q
    position = bisect_left(ordered, key)
    if position < length and ordered[position] == key:
        return [finite_copy(v, dtype) for v in table[position]]
    if position in (0, length):
        if edge == "reject":
            raise DomainError("query outside LUT axis")
        if edge == "clamp":
            return [finite_copy(v, dtype) for v in table[0 if position == 0 else -1]]
    left = max(0, min(length-2, position-1))
    t = (q-grid[left])/(grid[left+1]-grid[left])
    result = []
    for first, last in zip(table[left], table[left+1]):
        value = (1-t)*frac(first)+t*frac(last)
        result.append(round_fraction(value, dtype,
                      negative_zero=value == 0 and curves._signed_zero([first, last])))
    return result


def gradient_channels(query, start, end, controls, count=257, methods=None,
                      dtype=64, curve_edges=None, lookup_edge="clamp",
                      outputs=("values", "table", "axis"), sample_count=None):
    """Return requested flat-sample outputs, preserving all stage boundaries.

    query=None is permitted for table/axis requests and singleton LUTs. For
    singleton values, sample_count carries shape metadata without a query read.
    Model coordinates, including hue winding, are ordinary supplied scalars.
    """
    if not 1 <= count <= 1048576 or dtype not in (32, 64):
        raise DomainError("count or dtype")
    columns = len(controls)
    if columns not in (3, 4):
        raise DomainError("destination component count")
    methods = methods or ["linear"]*columns
    curve_edges = curve_edges or ["reject"]*columns
    if len(methods) != columns or len(curve_edges) != columns:
        raise DomainError("component option count")
    if lookup_edge not in ("reject", "clamp", "linear_extrapolate"):
        raise DomainError("lookup edge")
    wanted = set(outputs)
    if not wanted <= {"values", "table", "axis"}:
        raise DomainError("output name")
    n = sample_count if sample_count is not None else len(query) if query is not None else 1
    result = {}
    if "values" in wanted and n == 0:
        result["values"] = []
        wanted.remove("values")
    if not wanted:
        return result
    a = finite_copy(start, 64)
    b = a if count == 1 else finite_copy(end, 64)
    axis = [a, b, 0. if count == 1 else round_fraction((frac(b)-frac(a))/(count-1), 64)]
    if "axis" in wanted:
        result["axis"] = axis
    if not wanted & {"values", "table"}:
        return result
    grid = [a] if count == 1 else [a] + [
        round_fraction(((count-1-j)*frac(a)+j*frac(b))/(count-1), 64)
        for j in range(1, count-1)] + [b]
    table = [[0.]*columns for _ in grid]
    for c, ((x, y), method, edge) in enumerate(zip(controls, methods, curve_edges)):
        if method not in ("linear", "pchip"):
            raise DomainError("curve method")
        evaluate = curves.linear if method == "linear" else curves.pchip
        for j, coordinate in enumerate(grid):
            # Strict scalar CRV publishes directly in U. Native implementations
            # use binary64 evaluation followed by the same U publication.
            table[j][c] = evaluate(coordinate, x, y, edge, dtype)
    if "table" in wanted:
        result["table"] = table
    if "values" in wanted:
        result["values"] = [vector_lookup(None if count == 1 else query[i],
                            table, axis, lookup_edge, dtype) for i in range(n)]
    return result
