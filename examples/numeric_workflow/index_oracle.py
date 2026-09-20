"""Independent integer coordinate, contributor grouping and Fraction NUM-10 oracle."""
import itertools
import math
import random
import subprocess
import sys
from comparison_oracle import number
from sequence_oracle import ieee_round


def coordinates(shape):
    return itertools.product(*(range(n) for n in shape))


def linear(coordinate, shape):
    result = 0
    for index, size in zip(coordinate, shape):
        result = result * size + index
    return result


def aggregate(operation, dtype, bits):
    values = [number(b, dtype) for b in bits]
    floating = dtype in (3, 4)
    width = 32 if dtype == 4 else 64
    sign = 1 << (width - 1)
    quiet = 1 << (22 if dtype == 4 else 51)
    infinity = 0x7f800000 if dtype == 4 else 0x7ff0000000000000
    if floating:
        for raw, value in zip(bits, values):
            if value is None:
                return raw | quiet
    if operation != 'scatter_sum':
        selected = min(values) if operation == 'scatter_minimum' else max(values)
        if floating and selected == 0:
            zeros = [b for b, v in zip(bits, values) if v == 0]
            return sign if (any(b & sign for b in zeros) if operation == 'scatter_minimum'
                            else all(b & sign for b in zeros)) else 0
        return bits[values.index(selected)]
    if floating and any(v in (math.inf, -math.inf) for v in values):
        if math.inf in values and -math.inf in values:
            return infinity | quiet
        return infinity | (sign if -math.inf in values else 0)
    exact = sum(values)
    if not floating:
        low, high = (0, 255) if dtype == 1 else (-(1 << 63), (1 << 63)-1)
        return (exact & ((1 << (8 if dtype == 1 else 64))-1)) if low <= exact <= high else 'overflow'
    if exact == 0:
        return sign if all(b == sign for b in bits) else 0
    result = ieee_round(exact, width, False)
    return result if result is not None else infinity | (sign if exact < 0 else 0)


def reference(operation, dtype, axis, arrays):
    shape = list(arrays[0][0])
    if operation == 'concatenate':
        shape[axis] = sum(a[0][axis] for a in arrays)
    else:
        indices = [b if b < 1 << 63 else b-(1 << 64) for b in arrays[1][1]]
        if any(i < 0 or i >= shape[axis] for i in indices):
            return 'index'
        if operation == 'gather':
            shape[axis] = len(indices)
    output = []
    for coordinate in coordinates(shape):
        source = list(coordinate)
        if operation == 'concatenate':
            offset = 0
            for input_shape, data in arrays:
                if coordinate[axis] < offset + input_shape[axis]:
                    source[axis] -= offset
                    value = data[linear(source, input_shape)]
                    break
                offset += input_shape[axis]
        elif operation == 'gather':
            source[axis] = indices[coordinate[axis]]
            value = arrays[0][1][linear(source, arrays[0][0])]
        else:
            matches = [j for j, target in enumerate(indices) if target == coordinate[axis]]
            base = arrays[0][1][linear(coordinate, arrays[0][0])]
            if not matches:
                value = base
            elif operation == 'scatter_replace':
                source[axis] = matches[-1]
                value = arrays[2][1][linear(source, arrays[2][0])]
            else:
                terms = [base]
                for j in matches:
                    source[axis] = j
                    terms.append(arrays[2][1][linear(source, arrays[2][0])])
                value = aggregate(operation, dtype, terms)
        if isinstance(value, str):
            return value
        output.append(f'{value:x}')
    return ' '.join(output)


def main():
    rng = random.Random(1010)
    rows, expected = [], []

    def add(operation, dtype, axis, arrays, layout='dense'):
        row = f'{operation} {dtype} {axis} {layout} {len(arrays)}'
        for shape, values in arrays:
            row += ' ' + ','.join(map(str, shape)) + ' ' + ' '.join(f'{v:x}' for v in values)
        rows.append(row + '\n')
        expected.append(reference(operation, dtype, axis, arrays))

    for dtype, width in [(1, 8), (2, 64), (3, 64), (4, 32)]:
        sign = 1 << (width-1)
        special = [0, 1, sign, (1 << width)-1, sign-1]
        if dtype in (3, 4):
            inf = 0x7f800000 if dtype == 4 else 0x7ff0000000000000
            one = 0x3f800000 if dtype == 4 else 0x3ff0000000000000
            special += [inf, inf|sign, inf|1, inf|0x1234|sign, inf-1, one, one|sign, sign|1]
        def values(shape):
            return [rng.choice(special) if i%2 else rng.getrandbits(width) for i in range(math.prod(shape))]
        for rank in (1, 2, 3):
            for axis in range(rank):
                for _ in range(12):
                    shape = [rng.randrange(1,4) for _ in range(rank)]
                    arrays = []
                    for __ in range(rng.randrange(2,5)):
                        item = shape.copy()
                        item[axis] = rng.randrange(1,4)
                        arrays.append((item,values(item)))
                    for layout in ('view','dense'):
                        add('concatenate',dtype,axis,arrays,layout)
                    indices = [rng.randrange(-1,shape[axis]+1) for __ in range(rng.randrange(1,5))]
                    index_array = ([len(indices)],[v % (1 << 64) for v in indices])
                    base = (shape,values(shape))
                    add('gather',dtype,axis,[base,index_array])
                    updated_shape = shape.copy()
                    updated_shape[axis] = len(indices)
                    updates = (updated_shape,values(updated_shape))
                    for operation in ('scatter_replace','scatter_sum','scatter_minimum','scatter_maximum'):
                        add(operation,dtype,axis,[base,index_array,updates])
        for _ in range(150):
            terms = [rng.choice(special) for __ in range(rng.randrange(2,7))]
            for operation in ('scatter_sum','scatter_minimum','scatter_maximum'):
                add(operation,dtype,0,[([1],[terms[0]]),([len(terms)-1],[0]*(len(terms)-1)),([len(terms)-1],terms[1:])])
        if dtype in (3,4):
            half_ulp = 0x33800000 if dtype == 4 else 0x3ca0000000000000
            for terms in [[one,half_ulp,1],[inf-1,inf-1,sign|(inf-1)],
                          [inf,sign|inf,inf|0x1234],[sign,sign],[0,sign],
                          [sign|1,1],[inf|1,inf|0x4321|sign]]:
                for operation in ('scatter_sum','scatter_minimum','scatter_maximum'):
                    add(operation,dtype,0,[([1],[terms[0]]),([len(terms)-1],[0]*(len(terms)-1)),([len(terms)-1],terms[1:])])
    output = subprocess.run([sys.argv[1],sys.argv[2] if len(sys.argv)>2 else 'strict','oracle'],
                            input=''.join(rows),text=True,capture_output=True)
    if output.returncode:
        raise AssertionError((output.returncode, len(output.stdout.splitlines()), output.stderr))
    actual = output.stdout.splitlines()
    if len(actual) != len(expected):
        raise AssertionError((len(actual),len(expected),output.stderr))
    for row, want, got in zip(rows,expected,actual):
        if want != got:
            raise AssertionError((row.strip(),want,got))
    print(f'independent coordinate/contributor/Fraction oracle: {len(expected)} cases passed')


if __name__ == '__main__':
    main()
