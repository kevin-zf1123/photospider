"""Independent Perlin coordinate samples and Fraction/IEEE reference bits."""
from fractions import Fraction
import random

from oracle.ops.generation.common.exact import from_bits as exact_from_bits
from oracle.ops.generation.common.exact import round_bits as exact_round_bits
from oracle.ops.generation.noi04.rng import perlin2002_fraction as rng_perlin2002_fraction


def cases():
    r=random.Random(90104)
    rows=[]
    for small in [False,True]:
        width=32 if small else 64
        dtype='float32' if small else 'float64'
        mask=0x7f800000 if small else 0x7ff0000000000000
        edge=[0,1,2,3,(1<<(23 if small else 52))-1,1<<(23 if small else 52),mask-1]
        edge += [v|(1<<(width-1)) for v in edge]
        for v in edge:
            for axis in range(3):
                raw=[0,0,0];raw[axis]=v;rows.append((small,raw))
        for i in range(200):
            raw=[r.getrandbits(width) for _ in range(3)]
            if any(v&mask==mask for v in raw):continue
            rows.append((small,raw))
        for i in range(150):
            vals=[r.randint(-65536,65536)/8192 for _ in range(3)]
            raw=[exact_round_bits(Fraction(v),dtype) for v in vals]
            rows.append((small,raw))
    for small, raw in rows:
        for output_small in (False, True):
            yield small, output_small, raw


def expected(case):
    small, output_small, raw = case
    coords = [Fraction(exact_from_bits(v, 'float32' if small else 'float64'))
              for v in raw]
    result = rng_perlin2002_fraction(coords)
    return exact_round_bits(result, 'float32' if output_small else 'float64')


def reference_cases():
    for case in cases():
        yield case, expected(case)
