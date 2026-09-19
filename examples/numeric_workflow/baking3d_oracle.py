"""Independent Fraction source/grid/interpolation/error and report oracle.

python3 baking3d_oracle.py <photospider_numeric_baking3d> strict|apple|x86
"""
import itertools
import random
import subprocess
import sys
from fractions import Fraction as F
from comparison_oracle import number
from sequence_oracle import ieee_round
from lut3d_oracle import reference as applied

MODELS = ('rgb', 'xyz', 'cielab', 'oklab', 'cielch', 'oklch', 'hsl', 'ycbcr')


def bits(value, dtype=3):
    return ieee_round(F(value), 32 if dtype == 4 else 64, False)


def reference(mode, method, dtype, model, shape, axis, extra, atol, rtol):
    source_type = 4 if mode == 2 else 3
    grids = []
    for d, n in enumerate(shape):
        a, b, step = (number(v, 3) for v in axis[d*3:d*3+3])
        if not all(isinstance(v, F) for v in (a, b, step)) or a == b or bits((b-a)/(n-1)) != axis[d*3+2]:
            return 'domain'
        grid = [axis[d*3]]+[bits(((n-1-i)*a+i*b)/(n-1)) for i in range(1, n-1)]+[axis[d*3+1]]
        if any((number(y, 3)-number(x, 3))*(b-a) <= 0 for x, y in zip(grid, grid[1:])):
            return 'domain'
        grids.append(grid)
    if model in (4, 5) and any(number(x, 3) < 0 for x in grids[1]):
        return 'domain'
    def source(query):
        values = [number(x, 3) for x in query]
        if mode == 1:
            values = [x*x for x in values]
        elif mode == 6:
            values[0] *= values[0]
        elif mode == 5:
            values = [values[i]*values[(i+1)%3] for i in range(3)]
        return [bits(x, source_type) for x in values]
    table = []
    for query in itertools.product(*grids):
        for raw in source(query):
            if raw is None:
                return 'overflow'
            encoded = bits(number(raw, source_type), dtype)
            if encoded is None:
                return 'overflow'
            table.append(encoded)
    points = []
    for cell in itertools.product(*(range(n-1) for n in shape)):
        points.append([bits((number(grids[d][j], 3)+number(grids[d][j+1], 3))/2) for d, j in enumerate(cell)])
    points.extend(extra)
    maximum, indices, locations = [F(-1)]*3, [0]*3, [[0]*3 for _ in range(3)]
    failed, first, first_input, first_reference, first_lut = 0, -1, [0]*3, [0]*3, [0]*3
    for ordinal, query in enumerate(points):
        ref = source(query)
        if any(v is None for v in ref):
            return 'overflow'
        result = applied(method, MODELS[model], (3, dtype, dtype), 0, shape, query, axis, table)
        if result in ('domain', 'overflow'):
            return result
        values = [int(v, 16) for v in result.split()]
        errors = [abs(number(a, dtype)-number(b, source_type)) for a, b in zip(values, ref)]
        accepted = [error <= number(atol, 3)+number(rtol, 3)*abs(number(r, source_type)) for error, r in zip(errors, ref)]
        for c in range(3):
            if errors[c] > maximum[c]:
                maximum[c], indices[c], locations[c] = errors[c], ordinal, query
        if not all(accepted):
            failed += 1
            if first < 0:
                first, first_input = ordinal, query
                first_reference = [bits(number(v, source_type)) for v in ref]
                first_lut = [bits(number(v, dtype)) for v in values]
    upward = []
    for value in maximum:
        raw = bits(value)
        if raw is None:
            raw = 0x7ff0000000000000
        elif number(raw, 3) < value:
            raw += 1
        upward.append(raw)
    return ' '.join([str(int(not failed)), str(len(points)), str(failed)] +
                    [f'{v:x}' for v in upward] + list(map(str, indices)) +
                    [f'{v:x}' for location in locations for v in location] + [str(first)] +
                    [f'{v:x}' for v in first_input+first_reference+first_lut])


def cases():
    rows = []
    rng = random.Random(20920)
    def add(mode=0, method=0, dtype=3, model=0, shape=(2, 2, 2), endpoints=((0, 1),)*3,
            points=(), atol=0, rtol=0, axis_override=None):
        axis = []
        for (a, b), n in zip(endpoints, shape):
            a, b = number(bits(a), 3), number(bits(b), 3)
            axis += [bits(a), bits(b), bits((b-a)/(n-1))]
        if axis_override is not None:
            axis = axis_override
        extra = [[bits(v) for v in point] for point in points]
        rows.append((mode, method, dtype, model, shape, axis, extra, bits(atol), bits(rtol)))
    for mode, method, dtype, model in itertools.product((0, 1, 2, 5, 6), (0, 1), (3, 4), range(8)):
        add(mode, method, dtype, model, points=((F(1, 2),)*3, (F(1, 4), F(3, 4), F(1, 2))), atol=F(1, 10))
    for mode, method, dtype in itertools.product((0, 1, 2, 5, 6), (0, 1), (3, 4)):
        for mask in range(8):
            ends = tuple((1, 0) if mask & (1 << d) else (0, 1) for d in range(3))
            add(mode, method, dtype, shape=(3, 4, 2), endpoints=ends, atol=F(1, 8), rtol=F(1, 20))
        for _ in range(6):
            shape = tuple(rng.randrange(2, 5) for _ in range(3))
            ends = tuple((F(rng.randrange(4), 8), F(rng.randrange(8, 17), 8)) for _ in range(3))
            add(mode, method, dtype, shape=shape, endpoints=ends, atol=rng.choice((0, F(1, 100), F(1, 4))), rtol=rng.choice((0, F(1, 5))))
        add(mode, method, dtype, points=((2, F(1, 2), F(1, 2)),))
    # Source Float32 cast belongs to reference, and internal LUT must use the
    # explicit table dtype when measuring the rounded cell center.
    for dtype, method in itertools.product((3, 4), (0, 1)):
        add(2, method, dtype, endpoints=((1, 1+F(2)**-23), (0, 1), (0, 1)))
        add(0, method, dtype, endpoints=((1, 1+F(2)**-52), (0, 1), (0, 1)))
        add(1, method, dtype, points=((F(1, 2),)*3,)*3, atol=F(1, 4))
        add(1, method, dtype, atol=F(1, 4)-F(2)**-54)
        add(0, method, dtype, axis_override=[bits(v) for v in (0, 1, 1, 0, 1, F(1, 2), 0, 1, 1)])
    return rows


def main():
    rows = cases()
    lines, expected = [], []
    for row in rows:
        mode, method, dtype, model, shape, axis, points, absolute, relative = row
        lines.append(' '.join(map(str, (mode, method, dtype, model, *shape, len(points))))+' '+
                     ' '.join(f'{v:x}' for v in [absolute, relative]+axis+[v for p in points for v in p]))
        expected.append(reference(*row))
    profile = sys.argv[2] if len(sys.argv) > 2 else 'strict'
    output = subprocess.run([sys.argv[1], profile, '--probe'], input='\n'.join(lines)+'\n',
                            capture_output=True, text=True, check=True)
    actual = output.stdout.splitlines()
    assert len(actual) == len(rows), (len(actual), len(rows), output.stderr)
    for row, want, got in zip(rows, expected, actual):
        assert want == got, (row, want, got)
    print(f'{len(rows)} independent Fraction LUT3D bake/report cases PASS ({profile})')


if __name__ == '__main__':
    main()
