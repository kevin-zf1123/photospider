"""Independent Fraction source/grid/interpolation/error and report oracle.

"""
import itertools
import random
from fractions import Fraction as F
from oracle.ops.numeric.num07.reference import number
from oracle.ops.numeric.num02.reference import ieee_round
from oracle.ops.numeric.crv07.reference import reference as applied

MODELS = ('rgb', 'xyz', 'cielab', 'oklab', 'cielch', 'oklch', 'hsl', 'ycbcr')


def bits(value, dtype=3):
    return ieee_round(F(value), 32 if dtype == 4 else 64, False)


def geometry_reference(kind, shape, axis=(), extra=(), colors=(),
                       input_dtype=3, output_dtype=3):
    """Fixed geometry on valid caller inputs, preserving raw endpoint zeros.

    Grid knots are RN64 endpoint-weighted coordinates. Cell centers average
    those rounded knots, then round once more. Color conversion rounds directly
    from the actual source dtype; same-dtype copies retain the original bits.
    """
    if kind == 'axis':
        return list(axis)
    if kind == 'color':
        if input_dtype == output_dtype:
            return list(colors)
        sign = 31 if input_dtype == 4 else 63
        return [ieee_round(number(v, input_dtype),
                           32 if output_dtype == 4 else 64, bool(v >> sign))
                for v in colors]
    def weighted(a, b, wa, wb, divisor):
        value = (number(a, 3)*wa+number(b, 3)*wb)/divisor
        return ieee_round(value, 64, bool(a >> 63 and b >> 63))
    grids = []
    for d, n in enumerate(shape):
        a, b = axis[3*d:3*d+2]
        grids.append([a]+[weighted(a,b,n-1-i,i,n-1)
                          for i in range(1,n-1)]+[b])
    if kind == 'grid':
        return [v for row in itertools.product(*grids) for v in row]
    assert kind in ('points', 'points_extra')
    result = [weighted(grids[d][cell[d]],grids[d][cell[d]+1],1,1,2)
              for cell in itertools.product(*(range(n-1) for n in shape))
              for d in range(3)]
    if kind == 'points_extra':
        result.extend(v for row in extra for v in row)
    return result


def reference(mode, method, dtype, model, shape, axis, extra, atol, rtol,
              source_type=3):
    """Float64 pointwise source, optionally followed by an explicit Float32 cast.

    Table conversion follows the source's actual dtype. Every arithmetic source
    operation rounds to Float64 before the optional cast, matching that explicit
    graph rather than contracting two rounding steps into one.
    """
    assert source_type in (3, 4)
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
    def convert(raw, source, target):
        if raw is None:
            return None
        return ieee_round(number(raw, source), 32 if target == 4 else 64,
                          bool(raw >> (31 if source == 4 else 63)))
    def source(query):
        raw = list(query)
        components = range(3) if mode in (1, 5) else (0,) if mode == 6 else ()
        for i in components:
            j = (i+1) % 3 if mode == 5 else i
            value = number(query[i], 3) * number(query[j], 3)
            raw[i] = ieee_round(value, 64, bool((query[i] ^ query[j]) >> 63))
        return raw if source_type == 3 else [convert(x, 3, source_type) for x in raw]
    table = []
    for query in itertools.product(*grids):
        for raw in source(query):
            if raw is None:
                return 'overflow'
            encoded = convert(raw, source_type, dtype)
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
                first_reference = [convert(v, source_type, 3) for v in ref]
                first_lut = [convert(v, dtype, 3) for v in values]
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
    for mode, method, dtype, model in itertools.product((0, 1, 5, 6), (0, 1), (3, 4), range(8)):
        add(mode, method, dtype, model, points=((F(1, 2),)*3, (F(1, 4), F(3, 4), F(1, 2))), atol=F(1, 10))
    for mode, method, dtype in itertools.product((0, 1, 5, 6), (0, 1), (3, 4)):
        for mask in range(8):
            ends = tuple((1, 0) if mask & (1 << d) else (0, 1) for d in range(3))
            add(mode, method, dtype, shape=(3, 4, 2), endpoints=ends, atol=F(1, 8), rtol=F(1, 20))
        for _ in range(6):
            shape = tuple(rng.randrange(2, 5) for _ in range(3))
            ends = tuple((F(rng.randrange(4), 8), F(rng.randrange(8, 17), 8)) for _ in range(3))
            add(mode, method, dtype, shape=shape, endpoints=ends, atol=rng.choice((0, F(1, 100), F(1, 4))), rtol=rng.choice((0, F(1, 5))))
        add(mode, method, dtype, points=((2, F(1, 2), F(1, 2)),))
    # Internal LUT measurement uses the explicit table dtype.
    for dtype, method in itertools.product((3, 4), (0, 1)):
        add(0, method, dtype, endpoints=((1, 1+F(2)**-52), (0, 1), (0, 1)))
        add(1, method, dtype, points=((F(1, 2),)*3,)*3, atol=F(1, 4))
        add(1, method, dtype, atol=F(1, 4)-F(2)**-54)
        add(0, method, dtype, axis_override=[bits(v) for v in (0, 1, 1, 0, 1, F(1, 2), 0, 1, 1)])
    return rows


def reference_cases():
    for case in cases():
        yield case, reference(*case)
