"""Independent Fraction moments and bit-pattern square-root search for NUM-11."""
import itertools
import math
import random
import subprocess
import sys
from fractions import Fraction
from comparison_oracle import number
from sequence_oracle import ieee_round


def format_info(dtype):
    return (23, 127, 32) if dtype == 4 else (52, 1023, 64)


def root_round(value, dtype):
    """Search adjacent representable values, compare their exact midpoint squared."""
    fraction, bias, width = format_info(dtype)
    infinity = ((1 << (width-fraction-1))-1) << fraction
    low, high = 0, infinity
    while high-low > 1:
        middle = (low+high)//2
        x = number(middle, dtype)
        if x*x <= value:
            low = middle
        else:
            high = middle
    a = number(low, dtype)
    b = Fraction(2)**(bias+1) if high == infinity else number(high, dtype)
    midpoint_squared = ((a+b)/2)**2
    return high if value > midpoint_squared or (value == midpoint_squared and low % 2) else low


def nan_convert(raw, source, destination):
    sf, _, sw = format_info(source)
    df, _, dw = format_info(destination)
    payload = raw & ((1 << (sf-1))-1)
    converted = payload << (df-sf) if df >= sf else payload >> (sf-df)
    if payload and not converted:
        converted = 1
    infinity = ((1 << (dw-df-1))-1) << df
    return ((raw >> (sw-1)) << (dw-1)) | infinity | (1 << (df-1)) | converted


def reference_group(operation, source, destination, ddof, bits):
    n = len(bits)
    if operation == 'count':
        return n
    if operation in ('variance', 'std') and not 0 <= ddof < n:
        return 'ddof'
    values = [number(raw, source) for raw in bits]
    floating = destination in (3, 4)
    if floating:
        fraction, _, width = format_info(destination)
        sign = 1 << (width-1)
        infinity = ((1 << (width-fraction-1))-1) << fraction
        quiet = 1 << (fraction-1)
        for raw, value in zip(bits, values):
            if value is None:
                return nan_convert(raw, source, destination)
    if operation in ('minimum', 'maximum'):
        best = min(values) if operation == 'minimum' else max(values)
        if floating and best == 0:
            zero_bits = [raw for raw, value in zip(bits, values) if value == 0]
            negative = any(raw & sign for raw in zero_bits) if operation == 'minimum' else all(raw & sign for raw in zero_bits)
            return sign if negative else 0
        return bits[values.index(best)]
    if any(value in (math.inf, -math.inf) for value in values):
        if operation in ('variance', 'std') or (math.inf in values and -math.inf in values):
            return infinity | quiet
        return infinity | (sign if -math.inf in values else 0)
    total = sum(values, Fraction())
    if operation == 'sum' and not floating:
        low, high = (0, 255) if destination == 1 else (-(1 << 63), (1 << 63)-1)
        return int(total) % (1 << (8 if destination == 1 else 64)) if low <= total <= high else 'overflow'
    if operation in ('variance', 'std'):
        mean = total/n
        exact = sum(((value-mean)**2 for value in values), Fraction())/(n-ddof)
        if operation == 'std':
            return root_round(exact, destination)
        negative_zero = False
    else:
        exact = total/n if operation == 'mean' else total
        source_sign = 1 << (format_info(source)[2]-1)
        negative_zero = source in (3, 4) and all(raw == source_sign for raw in bits)
    rounded = ieee_round(exact, width, negative_zero)
    return rounded if rounded is not None else infinity | (sign if exact < 0 else 0)


def reference(operation, source, destination, ddof, shape, axes, bits):
    output_shape = [1 if i in axes else size for i, size in enumerate(shape)]
    output = []
    for point in itertools.product(*(range(size) for size in output_shape)):
        group = []
        for coordinate in itertools.product(*(range(size) if i in axes else [point[i]] for i, size in enumerate(shape))):
            linear = 0
            for x, size in zip(coordinate, shape):
                linear = linear*size+x
            group.append(bits[linear])
        value = reference_group(operation, source, destination, ddof, group)
        if isinstance(value, str):
            return value
        output.append(f'{value:x}')
    return ' '.join(output)


def main():
    rng = random.Random(1111)
    rows, expected = [], []
    def add(operation, source, destination, shape, axes, bits, ddof=0):
        rows.append(f"{operation} {source} {destination} {ddof} {','.join(map(str, shape))} {','.join(map(str, axes))} " + ' '.join(f'{raw:x}' for raw in bits) + '\n')
        expected.append(reference(operation, source, destination, ddof, shape, axes, bits))
    for source in (1, 2, 3, 4):
        width = 8 if source == 1 else 32 if source == 4 else 64
        sign = 1 << (width-1)
        special = [0, 1, sign, sign-1, (1 << width)-1]
        if source in (3, 4):
            fraction, _, _ = format_info(source)
            inf = ((1 << (width-fraction-1))-1) << fraction
            special += [inf, inf|sign, inf|1, inf|sign|1, inf|sign|(1 << (fraction-1)), inf-1, sign|(inf-1), sign|1]
        for serial in range(100):
            shape = [rng.randrange(1,4) for _ in range(1+serial%3)]
            axes = rng.sample(range(len(shape)), rng.randrange(1,len(shape)+1))
            values = [rng.choice(special) if serial%3 else rng.getrandbits(width) for _ in range(math.prod(shape))]
            n = math.prod(shape[i] for i in axes)
            for operation in ('sum','minimum','maximum','mean','count','variance','std'):
                destinations = ((source,) if operation in ('minimum','maximum') else (2,) if operation == 'count' else (1,2) if operation == 'sum' and source in (1,2) else (3,4))
                for destination in destinations:
                    add(operation,source,destination,shape,axes,values, rng.randrange(-1,n+1) if operation in ('variance','std') else 0)
        # Cross-window row-major ordering, maximum cancellation, subnormal and
        # midpoint roots, and integer moments without premature conversion.
        groups = [[0]*63+special+list(reversed(special)), [sign-1,sign-2], [sign,sign+1]]
        if source in (3,4):
            groups += [[inf-1, sign|(inf-1)], [1,sign|1], [0,1], [0,3], [sign,sign], [0,sign], [inf|sign|1,inf|2]]
            one = 0x3f800000 if source == 4 else 0x3ff0000000000000
            groups += [[one,sign|(one+1)], [one+1,sign|(one+2)],
                       [inf-1,inf-2], [one,one+1]]
        for values in groups:
            for operation in ('sum','mean','variance','std','minimum','maximum'):
                destinations = (source,) if operation in ('minimum','maximum') else (1,2) if operation == 'sum' and source in (1,2) else (3,4)
                for destination in destinations:
                    add(operation,source,destination,[len(values)],[0],values)
    run = subprocess.run([sys.argv[1], sys.argv[2] if len(sys.argv)>2 else 'strict', 'oracle'], input=''.join(rows), text=True, capture_output=True)
    actual = run.stdout.splitlines()
    if run.returncode or len(actual) != len(expected):
        raise AssertionError((run.returncode,len(actual),len(expected),run.stderr))
    for row, want, got in zip(rows, expected, actual):
        if want != got:
            raise AssertionError((row.strip(),want,got))
    print(f'independent Fraction/midpoint-square oracle: {len(rows)} cases passed')


if __name__ == '__main__':
    main()
