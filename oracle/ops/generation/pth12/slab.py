"""Independent Fraction vertical-slab oracle for regularized polygon Boolean.

Unlike the production halfedge arrangement, this oracle compares exact vertical
intervals and their affine boundaries on every critical slab. Coordinates
supplied as floats retain their exact binary values. Canonical serialization
(orientation, rotation, collinear suppression) is checked separately.
"""
from __future__ import annotations

from dataclasses import dataclass
from fractions import Fraction as F


class BooleanMismatch(AssertionError):
    """The candidate differs from the exact regular closed Boolean region."""


@dataclass(frozen=True)
class Line:
    slope: F
    intercept: F

    def at(self, x):
        return self.slope * x + self.intercept


def _cross(a, b):
    return a[0] * b[1] - a[1] * b[0]


def _sub(a, b):
    return a[0] - b[0], a[1] - b[1]


def _edges(contours):
    edges, xs = [], set()
    for contour in contours:
        pp = [(F(x), F(y)) for x, y in contour]
        xs.update(p[0] for p in pp)
        for a, b in zip(pp, pp[1:] + pp[:1]):
            if a != b:
                edges.append((a, b))
    return edges, xs


def _critical(edges, xs):
    edges = sorted({tuple(sorted(edge)) for edge in edges})
    xs = set(xs)
    for i, (a, b) in enumerate(edges):
        u = _sub(b, a)
        for c, d in edges[i + 1:]:
            v, delta = _sub(d, c), _sub(c, a)
            denominator = _cross(u, v)
            if denominator:
                t = _cross(delta, v) / denominator
                s = _cross(delta, u) / denominator
                if 0 <= t <= 1 and 0 <= s <= 1:
                    xs.add(a[0] + t * u[0])
    return sorted(xs)


def _inside(winding, rule):
    return bool(winding % 2) if rule == "evenodd" else bool(winding)


def _intervals(edges, x, rule):
    events = {}
    for a, b in edges:
        dx = b[0] - a[0]
        if not dx or not min(a[0], b[0]) < x < max(a[0], b[0]):
            continue
        slope = (b[1] - a[1]) / dx
        line = Line(slope, a[1] - slope * a[0])
        y = line.at(x)
        delta, previous = events.get(y, (0, line))
        assert previous == line, "missing critical intersection"
        events[y] = delta + (1 if dx > 0 else -1), line
    result, winding, lower = [], 0, None
    for y in sorted(events):
        delta, line = events[y]
        before = _inside(winding, rule)
        winding += delta
        after = _inside(winding, rule)
        if not before and after:
            lower = line
        elif before and not after:
            assert lower is not None and lower.at(x) < y
            result.append((lower, line))
            lower = None
    assert winding == 0 and lower is None
    return result


def _truth(a, b, operation):
    if operation == "union":
        return a or b
    if operation == "intersection":
        return a and b
    if operation == "difference":
        return a and not b
    if operation == "xor":
        return a != b
    raise ValueError("unknown Boolean operation")


def _combine(a, b, x, operation):
    events = {}
    for operand, intervals in enumerate((a, b)):
        for lower, upper in intervals:
            for line, delta in ((lower, 1), (upper, -1)):
                y = line.at(x)
                da, db, previous = events.get(y, (0, 0, line))
                assert previous == line, "missing operand intersection"
                events[y] = (da + (delta if operand == 0 else 0),
                             db + (delta if operand == 1 else 0), line)
    result, ca, cb, lower = [], 0, 0, None
    for y in sorted(events):
        da, db, line = events[y]
        before = _truth(bool(ca), bool(cb), operation)
        ca, cb = ca + da, cb + db
        assert ca in (0, 1) and cb in (0, 1)
        after = _truth(bool(ca), bool(cb), operation)
        if not before and after:
            lower = line
        elif before and not after:
            assert lower is not None and lower.at(x) < y
            result.append((lower, line))
            lower = None
    assert ca == cb == 0 and lower is None
    return result


def verify_boolean(a, b, result, operation="union",
                   fill_rule_a="nonzero", fill_rule_b="nonzero"):
    """Prove slab-wise region equality and return exact Fraction area.

    The output is interpreted with nonzero fill. All input AND output vertices
    and segment intersections contribute critical x coordinates. Comparing
    complete affine interval boundaries proves equality throughout each open
    slab, not merely equal total area or equal sampled slice lengths.
    """
    _truth(False, False, operation)
    if fill_rule_a not in ("nonzero", "evenodd") or fill_rule_b not in ("nonzero", "evenodd"):
        raise ValueError("unknown fill rule")
    ea, xa = _edges(a)
    eb, xb = _edges(b)
    er, xr = _edges(result)
    critical = _critical(ea + eb + er, xa | xb | xr)
    area = F(0)
    for left, right in zip(critical, critical[1:]):
        x = (left + right) / 2
        expected = _combine(_intervals(ea, x, fill_rule_a),
                            _intervals(eb, x, fill_rule_b), x, operation)
        actual = _intervals(er, x, "nonzero")
        if expected != actual:
            raise BooleanMismatch(
                f"{operation}, slab ({left}, {right}): "
                f"expected {expected!r}, actual {actual!r}")
        for lower, upper in expected:
            area += (right - left) * (upper.at(left) - lower.at(left)
                                     + upper.at(right) - lower.at(right)) / 2
    return area
