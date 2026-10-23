"""Version-one explicit-grid reference, separate from exact-input Boolean.

Input vertices are quantized ties-to-even. Intersections and topology are then
constructed over Fraction, using the rational boundary oracle and checked by
the independent vertical-slab oracle. Finally each canonical result vertex is
rounded ties-to-even, preserving the exact cycle order and every snapped edge.
No second clipping, hole deletion, or topology repair is performed.
"""
from fractions import Fraction as F

from oracle.ops.generation.common.exact import round_fraction
from oracle.ops.generation.pth12.geometry import boolean_regions
from oracle.ops.generation.pth12.slab import verify_boolean


def boolean_grid(a, b, operation="union", fill_rule_a="nonzero",
                 fill_rule_b="nonzero", spacing=1, origin=(0, 0)):
    """Return snapped integer cycles and exact squared stage-error witnesses.

    Coordinates, spacing and origin supplied as floats are their exact binary
    values. All quantized input and output indices must fit signed64. This
    tooling check diagnoses an oracle-domain error, not a production validator.
    """
    spacing = F(spacing)
    origin = tuple(map(F, origin))
    if spacing <= 0:
        raise ValueError("positive grid spacing required")
    input_error2 = F(0)

    def quantize(contours):
        nonlocal input_error2
        result = []
        for contour in contours:
            out = []
            for point in contour:
                source = tuple(map(F, point))
                q = tuple(round((x - o) / spacing) for x, o in zip(source, origin))
                if any(not -(1 << 63) <= v < (1 << 63) for v in q):
                    raise ValueError("signed64 input grid coordinate")
                error2 = sum((o + spacing * v - x) ** 2
                             for x, o, v in zip(source, origin, q))
                input_error2 = max(input_error2, error2)
                out.append(q)
            result.append(out)
        return result

    qa, qb = quantize(a), quantize(b)
    exact = boolean_regions(qa, qb, operation, fill_rule_a, fill_rule_b)
    verify_boolean(qa, qb, exact, operation, fill_rule_a, fill_rule_b)
    cycles = [[tuple(round(x) for x in p) for p in c] for c in exact]
    intersection_error2 = F(0)
    for source, snapped in zip(exact, cycles):
        for p, q in zip(source, snapped):
            if any(not -(1 << 63) <= v < (1 << 63) for v in q):
                raise ValueError("signed64 output grid coordinate")
            intersection_error2 = max(
                intersection_error2,
                spacing * spacing * sum((x - y) ** 2 for x, y in zip(p, q)))
    return dict(cycles=cycles, exact_cycles=exact, quantized_a=qa, quantized_b=qb,
                input_error2=input_error2, intersection_error2=intersection_error2)


def publish_grid(cycles, spacing=1, origin=(0, 0)):
    """Round the exact origin-plus-spacing map once, without changing order."""
    spacing, origin = F(spacing), tuple(map(F, origin))
    exact = [[tuple(o + spacing * x for o, x in zip(origin, p)) for p in c]
             for c in cycles]
    result = [[tuple(round_fraction(x) for x in p) for p in c] for c in exact]
    error2 = max((sum((F(y) - x) ** 2 for x, y in zip(p, q))
                  for a, b in zip(exact, result) for p, q in zip(a, b)), default=F(0))
    return result, error2
