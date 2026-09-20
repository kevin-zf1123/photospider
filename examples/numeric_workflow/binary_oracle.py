"""Manual public binary workflows against exact integers/Fractions and MPFR."""
import itertools
import random
import struct
import subprocess
import sys

from elementary_oracle import reference as elementary
from certified_oracle import reference as mathematical
from math_oracle_support import MPFR
from reduction_oracle import format_info

BASIC = {'add': 8, 'subtract': 9, 'multiply': 10,
         'divide': 11, 'minimum': 12, 'maximum': 13}
MATH = {'pow': 10, 'atan2': 11, 'atan2pi': 12}


def cases():
    rng = random.Random(505)
    for dtype in (1, 2):
        width = 8 if dtype == 1 else 64
        pool = [0, 1, 2, (1 << width)-1, (1 << (width-1)),
                (1 << (width-1))-1]
        if dtype == 2:
            pool += [(1 << 53)-1, (1 << 53)+1]
        pairs = list(itertools.product(pool, repeat=2))
        pairs += [(rng.getrandbits(width), rng.getrandbits(width))
                  for _ in range(100)]
        for operation in BASIC:
            if operation != 'divide':
                for a, b in pairs:
                    yield operation, dtype, a, b
    for dtype in (3, 4):
        p, bias, width = format_info(dtype)
        sign = 1 << (width-1)
        inf = ((1 << (width-p-1))-1) << p
        def bits(x):
            return int.from_bytes(struct.pack('>f' if dtype == 4 else '>d', x), 'big')
        special = [0, sign, 1, sign|1, (1 << p)-1, 1 << p,
                   inf-1, sign|(inf-1), inf, sign|inf,
                   inf|0x42, sign|inf|0x13, inf|(1 << (p-1))|0x51,
                   bits(1), bits(-1), bits(.5), bits(-.5), bits(2), bits(-2)]
        pairs = list(itertools.product(special, repeat=2))
        pairs += [(rng.getrandbits(width), rng.getrandbits(width))
                  for _ in range(180)]
        # Cancellation, subnormal/normal boundaries, and adjacent parity values.
        for value in (.25, .5, 1, 2, 3, 10, 100, 2**p, 2**(p+1)):
            for delta in (-1, 0, 1):
                a = bits(value)+delta
                pairs += [(a, bits(-value)), (bits(-1), a), (a, bits(.3)),
                          (a, bits(-.3)), (a, a+1), (a|sign, a+1)]
        for operation in list(BASIC)+list(MATH):
            for a, b in pairs:
                yield operation, dtype, a, b
        # Exact dyadic midpoint powers cannot be resolved by open enclosures.
        for a, b in [(81, 8.5), (121, 3.5), (9, 8.5), (4, -537.5),
                     (2, -1075), (2, -150), (4, -75),
                     (2, 1024), (.5, 1075), (-1, 2**(p+1)-1)]:
            yield 'pow', dtype, bits(a), bits(b)


def main():
    rows = list(cases())
    encoded, wanted = [], []
    for operation, dtype, a, b in rows:
        encoded.append(f'{operation} {dtype} {a:x} {b:x}')
        value = (elementary(BASIC[operation], dtype, a, b) if operation in BASIC
                 else mathematical(MATH[operation], dtype, a, b))
        wanted.append(value if isinstance(value, str) else f'{value:x}')
    profile = sys.argv[2] if len(sys.argv) > 2 else 'strict'
    result = subprocess.run([sys.argv[1], profile, 'oracle'],
                            input='\n'.join(encoded)+'\n', capture_output=True,
                            text=True, check=True)
    answers = result.stdout.splitlines()
    assert len(answers) == len(rows), (len(answers), len(rows), result.stderr)
    for i, (actual, expected) in enumerate(zip(answers, wanted)):
        assert actual == expected, (i, rows[i], actual, expected)
    with MPFR(128) as oracle:
        version = oracle.version
    print(f'{len(rows)} independent integer/Fraction/MPFR-{version} binary cases passed ({profile})')


if __name__ == '__main__':
    main()
