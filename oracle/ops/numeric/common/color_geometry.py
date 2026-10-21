#!/usr/bin/env python3
"""Exact Fraction oracle for native color white and primary-basis admission.

No product arithmetic helpers, third-party packages, or integration registration.
"""

import math
import random
import struct
import sys
from fractions import Fraction


def determinant(columns):
    a, b, c = columns
    return (a[0] * (b[1] * c[2] - b[2] * c[1])
            - b[0] * (a[1] * c[2] - a[2] * c[1])
            + c[0] * (a[1] * b[2] - a[2] * b[1]))


def column(x, y):
    x, y = Fraction(x), Fraction(y)
    return [x, y, 1 - x - y]


def expected(white, primaries):
    if not all(map(math.isfinite, (*white, *primaries))):
        return False
    x, y = map(Fraction, white)
    if not (x > 0 and y > 0 and x + y < 1):
        return False
    basis = [column(*primaries[i:i + 2]) for i in range(0, 6, 2)]
    if determinant(basis) == 0:
        return False
    for i in range(3):
        replaced = basis.copy()
        replaced[i] = column(*white)
        if determinant(replaced) == 0:
            return False
    return True


def cases():
    rng = random.Random(620260920)
    d65 = (.3127, .3290)
    tiny = math.ulp(0.0)
    largest = sys.float_info.max

    def random_float():
        while True:
            x = struct.unpack('<d', struct.pack('<Q', rng.getrandbits(64)))[0]
            if math.isfinite(x):
                return x

    cases = []
    for _ in range(450):
        p = tuple(random_float() for _ in range(6))
        cases.append((d65, p))
        cases.append(((random_float(), random_float()), p))
        cases.append(((tiny, tiny), p))
    # Nearly dependent columns and exact zero normalization scales.
    for _ in range(160):
        x, y = rng.random(), rng.random()
        p = (x, y, math.nextafter(x, math.inf), y, 0, 1)
        cases.append((d65, p))
        cases.append((d65, (x, y, x, y, 0, 1)))
        cases.append((d65, (*d65, x, y, 0, 1)))
    identity = (1, 0, 0, 1, 0, 0)
    for white in [(tiny, tiny), (.5, .5), (.5, math.nextafter(.5, 0)),
                  (0, .5), (-tiny, .5), (math.inf, .5), (math.nan, .5)]:
        cases.append((white, identity))
    for p in [(largest, largest, -largest, largest, tiny, -tiny),
              (largest, -largest, -largest, largest, 0, 0),
              (tiny, 0, 0, tiny, 0, 0),
              (.73470, .26530, 0, 1, .00010, -.077),
              (0, 0, tiny, tiny, 2*tiny, math.nextafter(2*tiny, math.inf))]:
        cases.extend([(d65, p), ((tiny, tiny), p)])
    return cases


def reference_cases():
    for case in cases():
        yield case, expected(*case)
