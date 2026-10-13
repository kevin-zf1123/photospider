"""Independent baked64 Gaussian coefficient cases and MPFR reference bits."""
from fractions import Fraction
import math
import random

from oracle.ops.filter.common.bounds import exp_q
from oracle.ops.filter.common.core import bits
from oracle.ops.filter.common.core import from_bits


def cases():
    cases = [(0, 0), (1 << 63, 0)]
    for sigma in (1, 0x000fffffffffffff, 0x0010000000000000,
                  0x3fc0000000000000, 0x3ff0000000000000,
                  0x7fefffffffffffff):
        cases += [(sigma, j) for j in (0, 1, 2, 3, 4, 5, 31, 2**63-1)]
    rng = random.Random(50401)
    for _ in range(250):
        # Include large integral offsets while keeping many exponents near the
        # nontrivial [subnormal, one] coefficient range.
        j = rng.randrange(1, 2**rng.randrange(1, 64))
        sigma = math.ldexp(j * rng.uniform(.5, 1), rng.randrange(-6, 31))
        cases.append((bits(sigma), j))
    for power in (-27, -26, 4, 5):
        sigma = math.ldexp(1.0, -power)
        raw = bits(sigma)
        cases += [(raw + delta, 1) for delta in (-1, 0, 1)]
    return cases


def expected(case):
    sigma, j = case
    s = Fraction(from_bits(sigma))
    return bits(exp_q(-Fraction(j*j)/(2*s*s))) if j else bits(1.0)


def reference_cases():
    for case in cases():
        yield case, expected(case)
