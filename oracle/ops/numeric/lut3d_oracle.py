"""Independent Fraction grid, simplex weights and IEEE rounding for LUT3D.

Run: python3 lut3d_oracle.py <photospider_numeric_lut3d> strict|apple|x86
"""
import bisect
import itertools
import math
import random
import subprocess
import sys
from fractions import Fraction as F

from comparison_oracle import number
from sequence_oracle import ieee_round


def bits(value, dtype=3, negative=False):
    return ieee_round(F(value), 32 if dtype == 4 else 64, negative)


def reference(method, model, types, clamp, shape, query, axis, table):
    it, tt, ot = types
    grids = []
    for dimension, n in enumerate(shape):
        raw = axis[dimension*3:dimension*3+3]
        a, b, step = [number(x, 3) for x in raw]
        if not all(isinstance(x, F) for x in (a, b, step)) or a == b:
            return 'domain'
        expected = bits((b-a)/(n-1))
        if expected is None or expected != raw[2] or not step:
            return 'domain'
        grid = [a] + [number(bits(((n-1-j)*a+j*b)/(n-1)), 3)
                      for j in range(1, n-1)] + [b]
        if any((right-left)*(b-a) <= 0 for left, right in zip(grid, grid[1:])):
            return 'domain'
        grids.append(grid)
    q = [number(x, it) for x in query]
    if not all(isinstance(x, F) for x in q):
        return 'domain'
    if model in ('cielch', 'oklch') and q[1] < 0:
        return 'domain'
    cell, t = [], []
    for value, grid in zip(q, grids):
        if not min(grid) <= value <= max(grid):
            if not clamp:
                return 'domain'
            value = max(min(grid), min(max(grid), value))
        decreasing = grid[-1] < grid[0]
        ordered = [-x for x in grid] if decreasing else grid
        key = -value if decreasing else value
        j = bisect.bisect_left(ordered, key)
        if j < len(grid) and ordered[j] == key:
            j = min(j, len(grid)-2)
        else:
            j -= 1
        cell.append(j)
        t.append((value-grid[j])/(grid[j+1]-grid[j]))
    if method:
        a, b, c = sorted(range(3), key=lambda i: (-t[i], i))
        vertices = [[0, 0, 0], [int(i == a) for i in range(3)],
                    [int(i in (a, b)) for i in range(3)], [1, 1, 1]]
        weights = [1-t[a], t[a]-t[b], t[b]-t[c], t[c]]
    else:
        vertices = list(itertools.product((0, 1), repeat=3))
        weights = [math.prod(t[i] if bit else 1-t[i] for i, bit in enumerate(v))
                   for v in vertices]
    assert sum(weights) == 1 and all(w >= 0 for w in weights)
    colors, raw_rows, required = [], [], []
    for vertex, weight in zip(vertices, weights):
        if not weight:
            continue
        at = [a+b for a, b in zip(cell, vertex)]
        flat = ((at[0]*shape[1]+at[1])*shape[2]+at[2])*3
        raw = table[flat:flat+3]
        color = [number(x, tt) for x in raw]
        if not all(isinstance(x, F) for x in color):
            return 'domain'
        if model in ('cielch', 'oklch') and color[1] < 0:
            return 'domain'
        colors.append(color)
        raw_rows.append(raw)
        required.append(weight)
    output = []
    for i in range(3):
        exact = sum(w*row[i] for row, w in zip(colors, required))
        negative_zero = all(row[i] == (1 << (31 if tt == 4 else 63)) for row in raw_rows)
        result = bits(exact, ot, negative_zero)
        if result is None:
            return 'overflow'
        output.append(f'{result:x}')
    return ' '.join(output)


def cases():
    records = []
    rng = random.Random(20720)
    def case(method=0, model='rgb', different=0, types=(3, 3, 3), clamp=0,
             shape=(2, 2, 2), query=(F(3, 4), F(1, 4), F(1, 2)),
             endpoints=((0, 1),)*3, table=None, raw=False, raw_axis=None):
        it, tt, ot = types
        axis = []
        for (a, b), n in zip(endpoints, shape):
            axis.extend((bits(a), bits(b), bits((F(b)-a)/(n-1))))
        if raw_axis is not None:
            axis = list(raw_axis)
        q = list(query) if raw else [bits(x, it) for x in query]
        if table is None:
            table = []
            for r, g, b in itertools.product(*(range(n) for n in shape)):
                table.extend((F(r*g, 8), F(g*b, 8), F(b*r, 8)))
            table = [bits(x, tt) for x in table]
        elif not raw:
            table = [bits(x, tt) for x in table]
        expected = reference(method, model, types, clamp, shape, q, axis, table)
        line = f'{method} {model} {different} {it} {tt} {ot} {clamp} '
        line += ' '.join(map(str, shape))+' '
        line += ' '.join(f'{x:x}' for x in q+axis+list(table))
        records.append((line, expected))
    # Every cube boundary and all split ties, then all eight axis directions.
    for method in (0, 1):
        for query in itertools.product((F(), F(1, 4), F(1, 2), F(3, 4), F(1)), repeat=3):
            case(method, query=query)
        for mask in range(8):
            endpoints = tuple((1, 0) if mask & (1 << i) else (0, 1) for i in range(3))
            for query in itertools.permutations((F(1, 4), F(1, 2), F(3, 4))):
                case(method, endpoints=endpoints, query=query)
        for model in ('rgb', 'xyz', 'cielab', 'oklab', 'cielch', 'oklch', 'hsl', 'ycbcr'):
            for types in itertools.product((3, 4), repeat=3):
                for different in (0, 1):
                    case(method, model, different, types, shape=(3, 4, 5))
        for _ in range(100):
            shape = tuple(rng.randrange(2, 7) for _ in range(3))
            endpoints = tuple((F(-1, 4), F(3, 4)) if rng.randrange(2) else (F(5, 4), F(-1, 2)) for _ in range(3))
            table = [F(rng.randrange(-512, 513), 64) for _ in range(math.prod(shape)*3)]
            query = tuple(F(rng.randrange(-4, 21), 16) for _ in range(3))
            case(method, types=tuple(rng.choice((3, 4)) for _ in range(3)),
                 clamp=rng.randrange(2), shape=shape, endpoints=endpoints, query=query, table=table)
        # Invalid components at zero-weight vertices must not be read.
        for query in ((0, 0, 0), (F(1, 2), 0, 0), (F(1, 2), F(1, 2), 0), (F(1, 2),)*3):
            for vertex in range(8):
                for channel in range(3):
                    table = [bits(F(j, 8)) for j in range(24)]
                    table[vertex*3+channel] = 0x7ff8000000000042
                    case(method, query=[bits(x) for x in query], table=table, raw=True)
        for ot in (3, 4):
            for zero in (0, 1 << 63):
                table = [1 << 63]*24
                table[0] = zero
                case(method, types=(3, 3, ot), table=table, raw=True,
                     query=[bits(F(1, 2))]*3)
            maximum = 0x7fefffffffffffff
            table = [maximum if j < 12 else maximum | (1 << 63) for j in range(24)]
            case(method, types=(3, 3, ot), table=table, raw=True, query=[bits(F(1, 2))]*3)
            case(method, types=(3, 3, ot), table=[maximum]*24, raw=True, query=[bits(F(1, 2))]*3)
            table = [1 if j < 12 else 2 for j in range(24)]
            case(method, types=(3, 3, ot), table=table, raw=True, query=[bits(F(1, 2))]*3)
        for model in ('cielch', 'oklch'):
            case(method, model=model, query=(0, -1, 0), clamp=1)
            table = [F(1)]*24
            table[1] = -1
            case(method, model=model, query=(0, 0, 0), table=table)
            case(method, model=model, query=(1, 1, 1), table=table)
        # Global axis checks apply at exact vertex hits too.
        valid = [bits(0), bits(1), bits(1)]*3
        for j in range(9):
            axis = valid[:]
            axis[j] = 0x7ff8000000000042
            case(method, query=(0, 0, 0), raw_axis=axis)
        for i in range(3):
            axis = valid[:]
            axis[3*i+2] = bits(F(1, 2))
            case(method, query=(0, 0, 0), raw_axis=axis)
        for component in range(3):
            q = [bits(F(1, 2))]*3
            q[component] = 0x7ff0000000000000
            case(method, raw=True, query=q, clamp=1)
        # Irregular rounded interior grid coordinates and full Ni=256 along
        # one axis retain manageable independent input fixture sizes.
        case(method, shape=(256, 2, 3), query=(F(1, 3), F(1, 2), F(2, 3)))
        tiny = [0, 2, 2]*3
        case(method, raw=True, query=[1]*3, raw_axis=tiny)
        # Descending exact interior knot starts at its stored index. Every
        # other vertex is invalid; sole source -0 survives conversion.
        table = [0x7ff8000000000042]*36
        table[12:15] = [1 << 63, bits(F(1, 2)), bits(2)]
        case(method, raw=True, shape=(3, 2, 2), endpoints=((1, 0), (0, 1), (0, 1)),
             query=[bits(F(1, 2)), 0, 0], table=table)
    return records


if __name__ == '__main__':
    records = cases()
    result = subprocess.run([sys.argv[1], sys.argv[2], '--probe'],
                            input='\n'.join(line for line, _ in records)+'\n',
                            text=True, capture_output=True, check=False)
    actual = result.stdout.splitlines()
    for index, ((line, expected), value) in enumerate(zip(records, actual)):
        if expected != value:
            raise AssertionError(f'case {index}: {line}\nexpected {expected}\nactual {value}')
    if result.returncode or len(actual) != len(records):
        raise AssertionError(f'probe stopped at {len(actual)}/{len(records)}: {result.stderr}')
    print(f'LUT3D independent Fraction grid/weights/rounding: {len(records)} cases {sys.argv[2]} PASS')
