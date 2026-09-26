"""Certified finite Gaussian oracle using independently directed MPFR intervals.

The production implementation is never imported. Reuses only the repository's
manual MPFR ctypes binding. Refuses uncertified rounding, rather than treating a
fixed decimal precision or a library blur as an exact reference.
"""
from __future__ import annotations
from fractions import Fraction as Q
from pathlib import Path
import sys
from exact import OracleError, param, integer, from_bits, rn

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'numeric_workflow'))
from math_oracle_support import MPFR, fraction_bounds


def certified_gaussian(a, sigma_y, sigma_x, radius_y, radius_x, boundary, dtype):
    from reference import mapped, coords
    if dtype not in ('float32', 'float64'):
        raise OracleError('Gaussian requires floating coverage', 'TypeMismatch')
    sy, sx = param(sigma_y, minimum=0), param(sigma_x, minimum=0)
    ry, rx = integer(radius_y, 'radius_y'), integer(radius_x, 'radius_x')
    if boundary not in ('zero', 'replicate', 'reflect_half'):
        raise OracleError('Gaussian boundary')
    if (not sy and ry) or (not sx and rx):
        raise OracleError('zero sigma requires zero radius')
    if not ry and not rx:
        return [row[:] for row in a]
    ntaps = (2*ry+1)*(2*rx+1)
    if ntaps*len(a)*len(a[0]) > 100000:
        raise OracleError('manual Gaussian oracle work cap100000', 'OracleCapacity')
    taps = [(dy, dx, -(Q(dy*dy)/(2*sy*sy) if sy else 0)
                       -(Q(dx*dx)/(2*sx*sx) if sx else 0))
            for dy in range(-ry, ry+1) for dx in range(-rx, rx+1)]
    exponents = sorted({t[2] for t in taps})
    samples = {site: [Q(mapped(a, site[0]+dy, site[1]+dx, boundary))
                     for dy, dx, _ in taps] for site in coords(a)}
    integers = [v for v in exponents] + [v for row in a for v in map(Q, row)]
    initial = max(128, max(max(abs(q.numerator).bit_length(), q.denominator.bit_length())
                           for q in integers)+16)
    precision = 1 << (initial-1).bit_length()
    width = 4 if dtype == 'float32' else 8
    # Binding integer() must be exact. Precision covers every numerator/denominator.
    while precision <= 8192:
        unresolved = False
        result = [[0. for _ in row] for row in a]
        with MPFR(precision) as m:
            zero = m.integer(0)
            weights = {}
            for exponent in exponents:
                lo, hi = fraction_bounds(m, exponent)
                weights[exponent] = (m.unary('exp', lo, m.downward),
                                     m.unary('exp', hi, m.upward))
            dl, du = zero, zero
            for _, _, exponent in taps:
                lo, hi = weights[exponent]
                dl = m.binary('add', dl, lo, m.downward)
                du = m.binary('add', du, hi, m.upward)
            # Origin exp(0)=1 proves the denominator cannot vanish.
            if m.compare(dl, zero) <= 0:
                raise OracleError('MPFR failed positive denominator enclosure', 'Uncertified')
            for (y, x), values in samples.items():
                if len(set(values)) == 1:
                    result[y][x] = rn(values[0], dtype)
                    continue
                nl, nu = zero, zero
                for (_, _, exponent), value in zip(taps, values):
                    if not value:
                        continue
                    vl, vu = fraction_bounds(m, value)
                    wl, wu = weights[exponent]
                    nl = m.binary('add', nl, m.binary('mul', vl, wl, m.downward), m.downward)
                    nu = m.binary('add', nu, m.binary('mul', vu, wu, m.upward), m.upward)
                lower = m.binary('div', nl, du, m.downward)
                upper = m.binary('div', nu, dl, m.upward)
                lb, ub = m.bits(lower, width), m.bits(upper, width)
                if lb != ub:
                    unresolved = True
                    break
                result[y][x] = from_bits(lb, dtype)
        if not unresolved:
            return result
        precision *= 2
    raise OracleError('MPFR bounds did not certify destination rounding by8192bits', 'Uncertified')
