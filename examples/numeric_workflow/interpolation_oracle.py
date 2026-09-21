"""Independent exact Fraction oracle for the public NUM-08 workflow."""
import math
import random
import subprocess
import sys
from comparison_oracle import number
from sequence_oracle import ieee_round


def reference(operation, dtype, bits):
    a, b, t = [number(x, dtype) for x in bits]
    width = 32 if dtype == 4 else 64
    sign = 1 << (width-1)
    one = 0x3f800000 if width == 32 else 0x3ff0000000000000
    infinity = 0x7f800000 if width == 32 else 0x7ff0000000000000
    quiet = 1 << (22 if width == 32 else 51)
    if operation == 'mix':
        if t is None or not 0 <= t <= 1:
            return 'error'
        if t == 0:
            return f'{bits[0]:x}'
        if t == 1:
            return f'{bits[1]:x}'
        if a is None or b is None:
            return f'{bits[0 if a is None else 1] | quiet:x}'
        if a in (math.inf, -math.inf) or b in (math.inf, -math.inf):
            if a in (math.inf, -math.inf) and b == -a:
                return f'{infinity | quiet:x}'
            return f'{bits[0 if a in (math.inf,-math.inf) else 1]:x}'
        if bits[0] == sign and bits[1] == sign:
            return f'{sign:x}'
        exact = (1-t)*a+t*b
    else:
        x, lower, upper = a, b, t
        if lower is None or upper is None or lower in (math.inf,-math.inf) or upper in (math.inf,-math.inf) or lower >= upper:
            return 'error'
        if x is None:
            return f'{bits[0] | quiet:x}'
        if x <= lower:
            return '0'
        if x >= upper:
            return f'{one:x}'
        factor = (x-lower)/(upper-lower)
        exact = factor*factor*(3-2*factor)
    result = ieee_round(exact, width, False)
    assert result is not None, (operation,dtype,bits)
    return f'{result:x}'


from accuracy_oracle import accepted

def main():
    rng = random.Random(808)
    rows, expected = [], []

    def add(operation, dtype, bits):
        rows.append(f'{operation} {dtype} ' + ' '.join(f'{b:x}' for b in bits) + '\n')
        expected.append(reference(operation, dtype, bits))

    for dtype,width,one,half,inf in [(3,64,0x3ff0000000000000,0x3fe0000000000000,0x7ff0000000000000),
                                   (4,32,0x3f800000,0x3f000000,0x7f800000)]:
        sign = 1 << (width-1)
        special = [0, sign, 1, sign|1, one, sign|one, inf-1, sign|(inf-1), inf, sign|inf, inf|1, sign|inf|0x1234]
        for a in special:
            for b in special:
                for t in [0,sign,half,one,one-1,1]:
                    add('mix',dtype,[a,b,t])
        for t in [sign|1,one+1,inf,sign|inf,inf|1]:
            add('mix',dtype,[inf|1,sign|inf|1,t])
        for _ in range(400):
            a,b = rng.getrandbits(width),rng.getrandbits(width)
            t = rng.randrange(one+1)
            add('mix',dtype,[a,b,t])
        finite = [b for b in special if number(b,dtype) is not None and number(b,dtype) not in (math.inf,-math.inf)]
        for lower in finite:
            for upper in finite:
                for x in special+[half]:
                    add('smoothstep',dtype,[x,lower,upper])
        for _ in range(500):
            bits=[]
            while len(bits)<3:
                raw=rng.getrandbits(width)
                if number(raw,dtype) is not None and number(raw,dtype) not in (math.inf,-math.inf):
                    bits.append(raw)
            bits.sort(key=lambda b:number(b,dtype))
            add('smoothstep',dtype,[bits[1],bits[0],bits[2]])
        for edge in [inf,sign|inf,inf|1,sign|inf|1]:
            add('smoothstep',dtype,[inf|1,edge,one])
            add('smoothstep',dtype,[inf|1,0,edge])
        # Very small exact factors, widest spans, endpoint and midpoint neighbors.
        for x in [1,2,half-1,half,half+1,one-1]:
            add('smoothstep',dtype,[x,0,one])
            add('smoothstep',dtype,[x,sign|(inf-1),inf-1])
    # Near a non-dyadic upper edge, a fast polynomial must retain [0,1].
    import struct
    for dtype, fmt, code in ((3, '>d', '>Q'), (4, '>f', '>I')):
        edge = struct.unpack(code, struct.pack(fmt, 1.3))[0]
        add('smoothstep', dtype, [edge-1, 0, edge])
    output=subprocess.run([sys.argv[1],sys.argv[2] if len(sys.argv)>2 else '_strict','oracle'],
                          input=''.join(rows),text=True,capture_output=True,check=True)
    actual=output.stdout.splitlines()
    if len(actual)!=len(expected):
        raise AssertionError((len(actual),len(expected),output.stderr))
    for row,want,got in zip(rows,expected,actual):
        if not accepted(got, want, int(row.split()[1]), "strict" if (len(sys.argv)<3 or sys.argv[2] in ("strict","_strict")) else "accelerated"):
            raise AssertionError((row.strip(),want,got))
    print(f'independent exact interpolation/Fraction oracle: {len(expected)} cases passed')


if __name__ == '__main__':
    main()
