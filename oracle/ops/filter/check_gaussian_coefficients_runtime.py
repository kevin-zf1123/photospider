"""Compare native baked64 coefficient bits with directed-MPFR certification.

The runner exercises the runtime coefficient builder, not a registered filter.
"""
import argparse
from fractions import Fraction
import json
import math
import random
import subprocess
import time

from oracles.bounds import Context, exp_q
from oracles.core import bits, from_bits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', required=True)
    args = parser.parse_args()
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
    expected = []
    for sigma, j in cases:
        s = Fraction(from_bits(sigma))
        expected.append(bits(exp_q(-Fraction(j*j)/(2*s*s))) if j else bits(1.0))
    start = time.perf_counter()
    result = subprocess.run([args.runner, '--stdin'],
                            input=''.join(f'{s:x} {j}\n' for s, j in cases),
                            text=True, capture_output=True, check=True)
    actual = [int(line, 16) for line in result.stdout.split()]
    if len(actual) != len(expected):
        raise AssertionError((len(actual), len(expected)))
    for case, got, want in zip(cases, actual, expected):
        if got != want:
            raise AssertionError((case, hex(got), hex(want)))
    with Context() as context:
        version = context.version
    print(json.dumps(dict(cases=len(cases), exact_bit_matches=len(actual),
                          runtime_seconds=time.perf_counter()-start,
                          oracle='DirectedMPFR', mpfr_version=version,
                          scope='native coefficient builder, no filter registration')))


if __name__ == '__main__':
    main()
