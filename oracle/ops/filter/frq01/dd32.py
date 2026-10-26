"""Exact-rational checks for the fixed DD32 center graph and stage constants.

Center operations use Python binary64 arithmetic. Fraction models the fused
product residual and checks exact errors and coefficient inequalities,
independently of the C++ implementation. Samples do not prove coverage of all
binary64 operands. Quarter-frequency regroupings use exact rational arithmetic.
"""
from fractions import Fraction as Q
import math
import random

U = Q(1, 2**53)
ETA = Q(1, 2**1074)


def two_sum(a, b):
    s = a + b
    p = s - a
    return s, (a - (s - p)) + (b - p)


def add(a, b):
    s = two_sum(a[0], b[0])
    t = two_sum(a[1], b[1])
    v = two_sum(s[1], t[0])
    return two_sum(s[0], v[0])


def multiply(a, b):
    p = a[0] * b[0]
    e = float(Q(a[0]) * Q(b[0]) - Q(p))
    c = a[0] * b[1]
    d = a[1] * b[0]
    return two_sum(p, (e + c) + d)


def value(a):
    return Q(a[0]) + Q(a[1])


def check_coefficients():
    # Explicit RN bounds for the short FMA product; low-low is omitted.
    q_coefficient = 9 + 18*U + 15*U**2 + 6*U**3 + U**4
    eta_coefficient = Q(3, 2) + 4*U + Q(5, 2)*U**2 + U**3/2
    assert q_coefficient < 16 and eta_coefficient < 2
    # Three complex products plus the two radix-four addition layers.
    q = U**2
    assert 224 + 2112*q + 4096*q*q < 256
    assert 192 + 2048*q + 4096*q*q < 256
    return 4


def check_samples(count=4096):
    rng = random.Random(7381984)
    checked = 0
    seeds = [0., -0., 2.**-1074, -2.**-1074, 2.**-1022,
             2.**-200, -2.**-200, 1., -1., 2.**200]
    for i in range(count):
        def operand():
            h = seeds[rng.randrange(len(seeds))] if i < 256 else math.ldexp(
                rng.uniform(-1, 1), rng.randrange(-480, 481))
            l = math.ldexp(rng.uniform(-1, 1), -54) * h
            return two_sum(h, l)
        a, b = operand(), operand()
        if i % 7 == 0:
            b = (-a[0], -a[1])
        x, y = value(a), value(b)
        assert abs(value(add(a, b)) - x-y) <= 4*U**2*(abs(x)+abs(y))
        assert abs(value(multiply(a, b)) - x*y) <= 16*U**2*abs(x*y)+8*ETA
        checked += 2
    return checked


def check_quarter_groups():
    rng = random.Random(5618)
    checked = 0
    for h in [1, 2, 4, 8, 16, 32]:
        for w in [1, 2, 4, 8, 16, 32]:
            data = [[tuple(Q(rng.randrange(-100, 101),
                             2**rng.randrange(1, 60)) for _ in range(2))
                     for x in range(w)] for y in range(h)]
            groups = [(Q(0), Q(0)) for _ in range(16)]
            for y in range(h):
                for x in range(w):
                    k = (y % 4)*4 + x % 4
                    groups[k] = tuple(a+b for a, b in zip(groups[k], data[y][x]))
            for ky in range(h):
                for kx in range(w):
                    if 4*ky % h or 4*kx % w:
                        continue
                    for inverse in [False, True]:
                        for component in [0, 1]:
                            def term(z, y, x):
                                phase = ((4*ky//h)*y + (4*kx//w)*x) % 4
                                phase = phase if inverse else -phase % 4
                                negative = (phase >= 2 if component else
                                            phase in [1, 2])
                                return (-1 if negative else 1)*z[component ^ (phase % 2)]
                            direct = sum(term(data[y][x], y, x)
                                         for y in range(h) for x in range(w))
                            grouped = sum(term(groups[y*4+x], y, x)
                                          for y in range(4) for x in range(4))
                            assert direct == grouped
                            checked += 1
    return checked


if __name__ == '__main__':
    import json
    print(json.dumps(dict(coefficient_inequalities=check_coefficients(),
                          exact_rational_center_samples=check_samples(),
                          quarter_frequency_regroupings=check_quarter_groups())))
