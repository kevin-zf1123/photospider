"""Independent RES-04 haar_opponent_v1 mathematical and staged-native oracles.

The reference enumerates patches directly and multiplies explicit Haar matrices.
It does not use displacement rolling sums, a transform cache, SIMD, or production
code. dtype='high' uses mpmath at the caller's active precision (at least 100
decimal digits is recommended); float32/float64 round every elementary operation.
The two native references can select different near-tied groups from a legal
regrouped implementation. Compare group/rank decisions separately when needed.
"""
from __future__ import annotations

import copy
import decimal
import math
import struct


class Arithmetic:
    def __init__(self, dtype):
        self.dtype = dtype
        if dtype == "high":
            import mpmath
            self.mp = mpmath.mp
        elif dtype not in ("float32", "float64"):
            raise ValueError("dtype must be float32, float64 or high")

    def cast(self, x):
        if self.dtype == "high":
            return self.mp.mpf(x)
        if self.dtype == "float64":
            return float(x)
        try:
            return struct.unpack("f", struct.pack("f", float(x)))[0]
        except OverflowError:
            return math.copysign(float("inf"), float(x))

    def add(self, a, b):
        return self.cast(a + b)

    def sub(self, a, b):
        return self.cast(a - b)

    def mul(self, a, b):
        return self.cast(a * b)

    def div(self, a, b):
        return self.cast(a / b)

    def sqrt(self, x):
        return self.cast(self.mp.sqrt(x) if self.dtype == "high" else math.sqrt(x))

    def constant(self, n):
        if self.dtype == "high":
            return self.mp.mpf(1) / self.mp.sqrt(n)
        with decimal.localcontext() as context:
            context.prec = 100
            return self.cast(decimal.Decimal(1) / decimal.Decimal(n).sqrt())


def _reflect(x, n):
    if n == 1:
        return 0
    x %= 2 * (n - 1)
    return x if x < n else 2 * (n - 1) - x


def _centers(n, step):
    out = list(range(0, n, step))
    if out[-1] != n - 1:
        out.append(n - 1)
    return out


def _haar(n, ar):
    """Rows of the recursively normalized Haar analysis matrix."""
    if n == 1:
        return [[ar.cast(1)]]
    previous = _haar(n // 2, ar)
    q = ar.constant(2)
    rows = []
    for row in previous:
        rows.append([ar.mul(row[j // 2], q) for j in range(n)])
    for i in range(n // 2):
        row = [ar.cast(0) for _ in range(n)]
        row[2 * i], row[2 * i + 1] = q, -q
        rows.append(row)
    return rows


def _transpose(a):
    return list(map(list, zip(*a)))


def _matmul(a, b, ar):
    bt = _transpose(b)
    out = []
    for row in a:
        result = []
        for col in bt:
            value = ar.cast(0)
            for x, y in zip(row, col):
                if x != 0 and y != 0:
                    value = ar.add(value, ar.mul(x, y))
            result.append(value)
        out.append(result)
    return out


def _patch_transform(patch, matrix, ar, inverse=False):
    ht = _transpose(matrix)
    return _matmul(_matmul(ht if inverse else matrix, patch, ar),
                   matrix if inverse else ht, ar)


def _i0(x, ar):
    z = ar.mul(ar.mul(x, ar.cast(.5)), ar.mul(x, ar.cast(.5)))
    total = term = ar.cast(1)
    for k in range(1, 10000):
        term = ar.mul(term, ar.div(z, ar.cast(k * k)))
        next_value = ar.add(total, term)
        if next_value == total:
            return total
        total = next_value
    raise ArithmeticError("I0 series failed to converge")


def _window(block, beta, ar):
    denominator = _i0(beta, ar)
    line = []
    for i in range(block):
        x = ar.cast(0) if block == 1 else ar.sub(
            ar.div(ar.mul(ar.cast(2), ar.cast(i)), ar.cast(block - 1)), ar.cast(1))
        root = ar.sqrt(max(ar.cast(0), ar.sub(ar.cast(1), ar.mul(x, x))))
        line.append(ar.div(_i0(ar.mul(beta, root), ar), denominator))
    return [[ar.mul(y, x) for x in line] for y in line]


def bm3d_reference(image, sigma, *, block_size, search_radius, group_size,
                   reference_step, match_hard, match_wiener, hard_threshold,
                   kaiser_beta, dtype="float64", color=False, trace=False, groups=None):
    """Return the native-profile result plus its basic estimate and optional groups.

    Grayscale input is HxW; color input is HxWx3 RGB, with no alpha. Source
    interpretation is provided by the caller at the operator boundary; this
    pure numerical reference performs no color-space inference or conversion.

    Optional groups={"hard": [...], "wiener": [...]} supplies independently
    verified groups in reference-center order. This isolates transform,
    shrinkage and aggregation checks from discrete near-tie matching decisions;
    such a run does not validate matching. Each entry is a list of (y, x).
    """
    h, w = len(image), len(image[0])
    if not h or not w or any(len(row) != w for row in image):
        raise ValueError("nonempty rectangular image required")
    if sigma == 0:
        return {"output": copy.deepcopy(image), "basic": copy.deepcopy(image),
                "hard_groups": [], "wiener_groups": []}
    if block_size not in (1, 2, 4, 8, 16, 32):
        raise ValueError("block_size")
    if group_size not in (1, 2, 4, 8, 16, 32, 64):
        raise ValueError("group_size")
    if not 1 <= reference_step <= block_size or search_radius < 0:
        raise ValueError("reference_step/search_radius")
    ar = Arithmetic(dtype)
    nc = 3 if color else 1
    a, c, d = ar.constant(3), ar.constant(2), ar.constant(6)
    source = [[[ar.cast(0) for _ in range(w)] for _ in range(h)] for _ in range(nc)]
    for y in range(h):
        for x in range(w):
            if color:
                r, g, b = map(ar.cast, image[y][x])
                source[0][y][x] = ar.mul(ar.add(ar.add(r, g), b), a)
                source[1][y][x] = ar.mul(ar.sub(r, b), c)
                source[2][y][x] = ar.mul(ar.add(ar.sub(r, ar.mul(ar.cast(2), g)), b), d)
            else:
                source[0][y][x] = ar.cast(image[y][x])
    sigma = ar.cast(sigma)
    variance = ar.mul(sigma, sigma)
    threshold = ar.mul(ar.cast(hard_threshold), sigma)
    block = block_size
    patch_area = block * block
    window = _window(block, ar.cast(kaiser_beta), ar)
    spatial = _haar(block, ar)
    group_matrices = {k: _haar(k, ar) for k in (1, 2, 4, 8, 16, 32, 64)
                      if k <= group_size}

    def patch(plane, y, x):
        return [[plane[_reflect(y - block // 2 + j, h)]
                       [_reflect(x - block // 2 + i, w)]
                 for i in range(block)] for j in range(block)]

    def execute(guide, match, wiener):
        numerator = [[[ar.cast(0) for _ in range(w)] for _ in range(h)]
                     for _ in range(nc)]
        denominator = copy.deepcopy(numerator)
        selected = []
        group_index = 0
        limit = ar.mul(ar.cast(match), ar.cast(patch_area))
        for y in _centers(h, reference_step):
            for x in _centers(w, reference_step):
                reference = patch(guide[0], y, x)
                candidates = []
                for dy in range(-search_radius, search_radius + 1):
                    for dx in range(-search_radius, search_radius + 1):
                        if dy == dx == 0:
                            continue
                        other = patch(guide[0], y + dy, x + dx)
                        distance = ar.cast(0)
                        for j in range(block):
                            for i in range(block):
                                delta = ar.sub(reference[j][i], other[j][i])
                                distance = ar.add(distance, ar.mul(delta, delta))
                        if distance < limit:
                            candidates.append((distance, y + dy, x + dx))
                candidates.sort()
                centers = [(y, x)] + [(j, i) for _, j, i in candidates[:group_size - 1]]
                size = 1 << (len(centers).bit_length() - 1)
                centers = centers[:size]
                if groups is not None:
                    centers = [tuple(point) for point in
                               groups["wiener" if wiener else "hard"][group_index]]
                    size = len(centers)
                    if (not size or size > group_size or size & (size - 1) or
                            centers[0] != (y, x) or len(set(centers)) != size or
                            any(abs(j-y) > search_radius or abs(i-x) > search_radius
                                for j, i in centers)):
                        raise ValueError("invalid supplied group")
                group_index += 1
                if trace:
                    selected.append(centers)
                group_matrix = group_matrices[size]
                for channel in range(nc):
                    transformed = []
                    pilot = []
                    for yy, xx in centers:
                        p = _patch_transform(patch(source[channel], yy, xx), spatial, ar)
                        transformed.append([v for row in p for v in row])
                        if wiener:
                            p = _patch_transform(patch(guide[channel], yy, xx), spatial, ar)
                            pilot.append([v for row in p for v in row])
                    values = _matmul(group_matrix, transformed, ar)
                    if wiener:
                        pilot = _matmul(group_matrix, pilot, ar)
                    total = ar.cast(0)
                    nonzero = 0
                    for j in range(size):
                        for i in range(patch_area):
                            if wiener:
                                square = ar.mul(pilot[j][i], pilot[j][i])
                                gain_denominator = ar.add(square, variance)
                                minimum = 2.**(-126 if dtype == "float32" else -1022)
                                if dtype != "high" and (variance < minimum or
                                                        not math.isfinite(gain_denominator)):
                                    magnitude = abs(pilot[j][i])
                                    if magnitude == 0:
                                        gain = ar.cast(0)
                                    else:
                                        ratio = ar.div(min(magnitude, sigma), max(magnitude, sigma))
                                        ratio2 = ar.mul(ratio, ratio)
                                        gain = ar.div(ar.cast(1) if magnitude >= sigma else ratio2,
                                                      ar.add(ar.cast(1), ratio2))
                                else:
                                    gain = ar.div(square, gain_denominator)
                                values[j][i] = ar.mul(values[j][i], gain)
                                total = ar.add(total, ar.mul(gain, gain))
                            elif abs(values[j][i]) < threshold:
                                values[j][i] = ar.cast(0)
                            else:
                                nonzero += values[j][i] != 0
                    if not wiener:
                        total = ar.cast(nonzero)
                    weight = ar.cast(1) if total == 0 else ar.div(ar.cast(1), total)
                    values = _matmul(_transpose(group_matrix), values, ar)
                    for k, (yy, xx) in enumerate(centers):
                        p = [values[k][j * block:(j + 1) * block] for j in range(block)]
                        p = _patch_transform(p, spatial, ar, inverse=True)
                        for j in range(block):
                            oy = yy - block // 2 + j
                            if not 0 <= oy < h:
                                continue
                            for i in range(block):
                                ox = xx - block // 2 + i
                                if 0 <= ox < w:
                                    weight_pixel = ar.mul(weight, window[j][i])
                                    numerator[channel][oy][ox] = ar.add(
                                        numerator[channel][oy][ox],
                                        ar.mul(weight_pixel, p[j][i]))
                                    denominator[channel][oy][ox] = ar.add(
                                        denominator[channel][oy][ox], weight_pixel)
        result = [[[ar.div(numerator[c][y][x], denominator[c][y][x])
                    for x in range(w)] for y in range(h)] for c in range(nc)]
        return result, selected

    basic, hard_groups = execute(source, match_hard, False)
    output, wiener_groups = execute(basic, match_wiener, True)

    def decode(planes):
        if not color:
            return planes[0]
        rgb = []
        for y in range(h):
            row = []
            for x in range(w):
                l, s, t = ar.mul(planes[0][y][x], a), ar.mul(planes[1][y][x], c), ar.mul(planes[2][y][x], d)
                row.append([ar.add(ar.add(l, s), t),
                            ar.sub(l, ar.mul(ar.cast(2), t)),
                            ar.add(ar.sub(l, s), t)])
            rgb.append(row)
        return rgb
    return {"output": decode(output), "basic": decode(basic),
            "hard_groups": hard_groups, "wiener_groups": wiener_groups}
