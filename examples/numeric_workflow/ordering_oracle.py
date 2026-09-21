"""Independent Python stable ordering and Fraction rank/interpolation oracle."""
import itertools
import math
import random
import subprocess
import sys
from fractions import Fraction
from comparison_oracle import number
from reduction_oracle import nan_convert, format_info
from sequence_oracle import ieee_round


def ordered(bits, dtype):
    values = [number(raw, dtype) for raw in bits]
    return sorted(range(len(bits)), key=lambda i: (values[i] is None, 0 if values[i] is None else values[i], i))


def quantile(bits, dtype, q, destination):
    fraction, _, width = format_info(destination)
    infinity = ((1 << (width-fraction-1))-1) << fraction
    sign = 1 << (width-1)
    quiet = 1 << (fraction-1)
    values = [number(raw, dtype) for raw in bits]
    for raw, value in zip(bits, values):
        if value is None:
            return nan_convert(raw, dtype, destination)
    indices = ordered(bits, dtype)
    h = Fraction(len(bits)-1)*q if len(bits)>1 else Fraction()
    j = h.numerator//h.denominator
    w = h-j
    a = values[indices[j]]
    raw_a = bits[indices[j]]
    negative_zero = False
    if w == 0:
        if a in (math.inf,-math.inf):
            return infinity | (sign if a < 0 else 0)
        exact = Fraction(a)
        negative_zero = dtype in (3,4) and raw_a == (1 << (format_info(dtype)[2]-1))
    else:
        b = values[indices[j+1]]
        raw_b = bits[indices[j+1]]
        if a == -math.inf and b == math.inf:
            return infinity | quiet
        if a in (math.inf,-math.inf) or b in (math.inf,-math.inf):
            return infinity | (sign if a == -math.inf or b == -math.inf else 0)
        exact = (1-w)*a+w*b
        negative_zero = dtype in (3,4) and raw_a == raw_b == (1 << (format_info(dtype)[2]-1))
    rounded = ieee_round(exact, width, negative_zero)
    return rounded if rounded is not None else infinity | (sign if exact < 0 else 0)


def reference(operation, dtype, qtype, destination, axis, shape, qbits, bits):
    q = number(qbits,qtype)
    if operation == 'quantile' and shape[axis]>1 and (q is None or q in (math.inf,-math.inf) or not 0 <= q <= 1):
        return 'q'
    output_shape = [1 if operation == 'quantile' and i == axis else n for i,n in enumerate(shape)]
    values, indices = [], []
    for coordinate in itertools.product(*(range(n) for n in output_shape)):
        group = []
        for at in range(shape[axis]):
            sample = list(coordinate)
            sample[axis] = at
            index = 0
            for c,n in zip(sample,shape):
                index=index*n+c
            group.append(bits[index])
        if operation == 'sort':
            original = ordered(group,dtype)[coordinate[axis]]
            values.append(group[original])
            indices.append(original)
        else:
            values.append(quantile(group,dtype,q,destination))
    result = ' '.join(f'{value:x}' for value in values)
    return result + ' | ' + ' '.join(f'{value:x}' for value in indices) if operation == 'sort' else result


from accuracy_oracle import accepted_values

def main():
    rng = random.Random(1212)
    rows, expected = [], []
    def add(operation,dtype,qtype,destination,axis,shape,qbits,bits):
        rows.append(f"{operation} {dtype} {qtype} {destination} {axis} {','.join(map(str,shape))} {qbits:x} " + ' '.join(f'{raw:x}' for raw in bits) + '\n')
        expected.append(reference(operation,dtype,qtype,destination,axis,shape,qbits,bits))
    for dtype in (1,2,3,4):
        width = 8 if dtype == 1 else 32 if dtype == 4 else 64
        sign = 1 << (width-1)
        special = [0,1,sign,sign-1,(1<<width)-1]
        if dtype in (3,4):
            fraction, _, _ = format_info(dtype)
            inf = ((1 << (width-fraction-1))-1) << fraction
            special += [inf,inf|sign,inf|1,inf|sign|0x1234,inf|(1 << (fraction-1)),inf-1,sign|(inf-1),sign|1]
        for serial in range(80):
            rank = 1+serial%3
            shape = [rng.randrange(1,5) for _ in range(rank)]
            axis = rng.randrange(rank)
            bits = [rng.choice(special) if serial%3 else rng.getrandbits(width) for _ in range(math.prod(shape))]
            add('sort',dtype,3,dtype,axis,shape,0,bits)
            for qtype in (3,4):
                one = 0x3f800000 if qtype == 4 else 0x3ff0000000000000
                half = 0x3f000000 if qtype == 4 else 0x3fe0000000000000
                qwidth = 32 if qtype == 4 else 64
                qbits = rng.choice([0,1,1<<(qwidth-1),half-1,half,half+1,one-1,one,one+1,(1<<qwidth)-1])
                for destination in (3,4):
                    add('quantile',dtype,qtype,destination,axis,shape,qbits,bits)
        for bits in [[special[i%len(special)] for i in range(17)], list(reversed(special)), [sign,0,sign,0], [sign-1,sign-2]]:
            add('sort',dtype,3,dtype,0,[len(bits)],0,bits)
            for qtype in (3,4):
                one = 0x3f800000 if qtype == 4 else 0x3ff0000000000000
                half = 0x3f000000 if qtype == 4 else 0x3fe0000000000000
                for qbits in (0,1,half-1,half,half+1,one-1,one):
                    for destination in (3,4):
                        add('quantile',dtype,qtype,destination,0,[len(bits)],qbits,bits)
    # Exercise the high UInt64 remainder word and the shift>=128 branch.
    for shift in (64,65,127,128):
        qbits = ((1075-shift) << 52) | ((1 << 52)-1)
        for destination in (3,4):
            add('quantile',1,3,destination,0,[4097],qbits,[0]+[1]*4096)
    run = subprocess.run([sys.argv[1],sys.argv[2] if len(sys.argv)>2 else 'strict','oracle'],input=''.join(rows),text=True,capture_output=True)
    actual=run.stdout.splitlines()
    if run.returncode or len(actual)!=len(expected):
        raise AssertionError((run.returncode,len(actual),len(expected),run.stderr))
    for row,want,got in zip(rows,expected,actual):
        if not (got == want or (row.startswith("quantile") and accepted_values(got, want, int(row.split()[3]), sys.argv[2] if len(sys.argv)>2 else "strict"))):
            raise AssertionError((row.strip(),want,got))
    print(f'independent stable-order/Fraction quantile oracle: {len(rows)} cases passed')


if __name__ == '__main__':
    main()
