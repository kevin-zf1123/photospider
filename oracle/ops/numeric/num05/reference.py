"""Independent binary references from exact integers/Fractions and MPFR."""
import itertools
import random
import struct

from oracle.ops.numeric.common.elementary import reference as elementary
from oracle.ops.numeric.common.certified import reference as mathematical
from oracle.ops.numeric.common.mpfr import MPFR
from oracle.ops.numeric.num11.reference import format_info

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


from oracle.ops.numeric.common.accuracy import accepted

def reference_cases():
    for case in cases():
        operation, dtype, a, b = case
        yield case, (elementary(BASIC[operation], dtype, a, b) if operation in BASIC
                     else mathematical(MATH[operation], dtype, a, b))
