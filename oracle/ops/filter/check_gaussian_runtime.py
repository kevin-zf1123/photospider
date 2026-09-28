"""Compare actual public Gaussian workflows with MPFR + exact rational outputs."""
import argparse
from fractions import Fraction
import json
import random
import subprocess
import time

from oracles.bounds import Context
from oracles.core import bits, coordinate, from_bits
from oracles.special import divide_positive, sum_products
from oracles.transcend import gaussian_kernel


def expected(case):
    small, h, w, rx, ry, sx, sy, cval, boundary, data = case
    if rx == ry == 0:
        return data
    dtype = 'float32' if small else 'float64'
    kx = gaussian_kernel(sx, rx)
    ky = gaussian_kernel(sy, ry)
    normalizer = sum(map(Fraction, kx))*sum(map(Fraction, ky))
    result = []
    for y in range(h):
        for x in range(w):
            terms = []
            for j, vy in enumerate(ky):
                for i, vx in enumerate(kx):
                    if not vx or not vy:
                        continue
                    at = coordinate(y+ry-j, x+rx-i, h, w, boundary)
                    value = cval if at is None else from_bits(data[at[0]*w+at[1]], dtype)
                    terms.append((vy, vx, value))
            result.append(bits(divide_positive(sum_products(terms), normalizer, dtype), dtype))
    return result


def cases():
    rng = random.Random(50402)
    for small in (False, True):
        dtype = 'float32' if small else 'float64'
        sign = 1 << (31 if small else 63)
        inf = 0x7f800000 if small else 0x7ff0000000000000
        edge = [0, sign, 1, sign | 1, inf-1, sign | (inf-1),
                inf, sign | inf, inf | 123, sign | inf | 456]
        yield (small, 2, 5, 0, 0, 0., 0., -0., 'clamp', edge)
        yield (small, 1, 1, 1, 0, .125, 0., 1e40, 'constant', [0])
        for boundary in ('constant', 'clamp', 'wrap', 'reflect_half', 'reflect_whole'):
            for trial in range(8):
                h, w = rng.randrange(1, 5), rng.randrange(1, 6)
                rx, ry = rng.randrange(0, 4), rng.randrange(0, 3)
                sx, sy = rng.choice((.125, .7, 1., 2.5)), rng.choice((.25, 1., 1.7))
                cval = rng.choice((-3., -0., float.fromhex('0x1.fffffep128'), 0.25))
                if trial == 0:
                    data = [sign] * (h*w)
                elif trial == 1:
                    data = [rng.choice(edge) for _ in range(h*w)]
                else:
                    data = [bits(rng.randrange(-10000, 10000)/1024, dtype)
                            for _ in range(h*w)]
                yield (small, h, w, rx, ry, sx, sy, cval, boundary, data)
            poison = [bits(float(i), dtype) for i in range(7)]
            poison[5] = inf | sign | 17
            yield (small, 1, 7, 5, 0, .125, 1., 0., boundary, poison)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', required=True)
    parser.add_argument('--tiled', action='store_true')
    args = parser.parse_args()
    all_cases = list(cases())
    goldens = [expected(case) for case in all_cases]
    lines = []
    for small, h, w, rx, ry, sx, sy, cval, boundary, data in all_cases:
        lines.append(f'{int(small)} {h} {w} {rx} {ry} '
                     f'{bits(sx):x} {bits(sy):x} {bits(cval):x} {boundary} '
                     + ' '.join(f'{value:x}' for value in data))
    start = time.perf_counter()
    result = subprocess.run([args.runner, '--tiled-stdin' if args.tiled else '--stdin'], input='\n'.join(lines)+'\n',
                            text=True, capture_output=True, check=True)
    actual = [[int(value, 16) for value in line.split()] for line in result.stdout.splitlines()]
    if len(actual) != len(goldens):
        raise AssertionError((len(actual), len(goldens)))
    for i, (got, want) in enumerate(zip(actual, goldens)):
        if got != want:
            raise AssertionError((i, all_cases[i], [hex(v) for v in got], [hex(v) for v in want]))
    with Context() as context:
        version = context.version
    print(json.dumps(dict(mode='tiled' if args.tiled else 'whole', workflows=len(all_cases), exact_bit_matches=sum(map(len, actual)),
                          runtime_seconds=time.perf_counter()-start,
                          oracle='DirectedMPFR baked64 + Fraction numerator/denominator + IEEE RN',
                          mpfr_version=version)))


if __name__ == '__main__':
    main()
