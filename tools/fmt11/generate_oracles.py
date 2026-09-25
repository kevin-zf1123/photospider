#!/usr/bin/env python3
"""Regenerate pinned FMT-11 finite fixtures (offline; optional mpmath 1.3+).

Rational formulas use fractions.Fraction and integer IEEE ties-to-even rounding.
Nonalgebraic fixtures are independently evaluated at 180 and 360 decimal digits;
only identical destination bits are retained. Stability is an oracle check, not
an exhaustive proof of correct rounding. Runtime/tests do not depend on Python.
"""
from __future__ import annotations
import argparse
from fractions import Fraction as F
from pathlib import Path
import random
import struct
import mpmath as mp

M1 = [[F(s) for s in row.split()] for row in [
    '0.8190224379967030 0.3619062600528904 -0.1288737815209879',
    '0.0329836539323885 0.9292868615863434 0.0361446663506424',
    '0.0481771893596242 0.2642395317527308 0.6335478284694309']]
M2 = [[F(s) for s in row.split()] for row in [
    '0.2104542683093140 0.7936177747023054 -0.0040720430116193',
    '1.9779985324311684 -2.4285922420485799 0.4505937096174110',
    '0.0259040424655478 0.7827717124575296 -0.8086757549230774']]

def inverse(a):
    a = [list(row) + [F(int(i == j)) for j in range(3)] for i, row in enumerate(a)]
    for i in range(3):
        pivot = a[i][i]
        a[i] = [x / pivot for x in a[i]]
        for j in range(3):
            if j != i:
                factor = a[j][i]
                a[j] = [x - factor*y for x, y in zip(a[j], a[i])]
    return [row[3:] for row in a]
I1, I2 = inverse(M1), inverse(M2)

def bits(value: float, narrow: bool) -> int:
    return int.from_bytes(struct.pack('<f' if narrow else '<d', value), 'little')

def rounded(value: F, narrow: bool) -> int:
    sign = (1 << (31 if narrow else 63)) if value < 0 else 0
    value = abs(value)
    if not value:
        return sign
    n, d = value.numerator, value.denominator
    p, emin, emax, bias = (24, -126, 127, 127) if narrow else (53, -1022, 1023, 1023)
    e = n.bit_length() - d.bit_length()
    if (n < d << e) if e >= 0 else (n << -e < d):
        e -= 1
    shift = p - 1 - max(e, emin)
    a, b = (n << shift, d) if shift >= 0 else (n, d << -shift)
    q, r = divmod(a, b)
    if 2*r > b or (2*r == b and q & 1):
        q += 1
    if q >= 1 << p:
        e += 1
        q >>= 1
    if e > emax:
        return sign | (((1 << (8 if narrow else 11)) - 1) << (p - 1))
    if q < 1 << (p - 1):
        return sign | q
    return sign | ((max(e, emin) + bias) << (p - 1)) | (q - (1 << (p - 1)))

def dot(row, vector):
    return sum((a*b for a,b in zip(row, vector)), F(0))

def rational(kind: int, x: list[F]):
    a,b,c = x
    if kind == 1:
        f = (100*a+16)/116
        def g(t): return t**3 if t > F(6,29) else F(108,841)*(t-F(4,29))
        return [g(f+b/500), g(f), 2*g(f-c/200)]
    if kind == 5:
        cubes = [dot(row, x)**3 for row in I2]
        return [dot(row, cubes) for row in I1]
    if kind in (8,10):
        hi, lo = max(x), min(x)
        delta = hi-lo
        level = (hi+lo)/2 if kind == 8 else hi
        if not delta: return [F(0),F(0),level]
        sector = x.index(hi)
        hue = (((x[(sector+1)%3]-x[(sector+2)%3])/delta + 2*sector) % 6)/3
        denominator = 1-abs(hi+lo-1) if kind == 8 else hi
        return [hue, delta/denominator if denominator else None, level]
    if kind in (9,11):
        q = (3*a) % 6
        chroma = (1-abs(2*c-1))*b if kind == 9 else c*b
        offset = c-chroma/2 if kind == 9 else c-chroma
        v = chroma*(1-abs(q % 2-1))
        choices = [(chroma,v,0),(v,chroma,0),(0,chroma,v),(0,v,chroma),(v,0,chroma),(chroma,0,v)]
        return [F(z)+offset for z in choices[q.numerator//q.denominator]]
    if kind == 12:
        kr,kb = F(1,4),F(1,4)
        y = kr*a+(1-kr-kb)*b+kb*c
        return [y,(c-y)/(2*(1-kb)),(a-y)/(2*(1-kr))]
    if kind == 13:
        return [a+F(3,2)*c,a-F(3,4)*b-F(3,4)*c,a+F(3,2)*b]
    if kind == 14:
        return [a/(a+b+c),b/(a+b+c),b] if a+b+c else [None,None,b]
    if kind == 15:
        return [a*c/b,c,(1-a-b)*c/b] if b else [None,c,None]
    if kind == 16: return [b]
    if kind == 17: return [a,a,2*a]
    return None

def transcendental(kind, x):
    conv = lambda f: mp.mpf(f.numerator)/f.denominator
    a,b,c = [conv(z) for z in x]
    if kind == 0:
        def f(t): return mp.root(t,3) if t > mp.mpf(216)/24389 else mp.mpf(841)/108*t+mp.mpf(4)/29
        return [(116*f(b)-16)/100,500*(f(a)-f(b)),200*(f(b)-f(c/2))]
    if kind in (2,6): return [a,mp.sqrt(b*b+c*c),mp.atan2(c,b)/mp.pi if b or c else mp.mpf(0)]
    if kind in (3,7): return [a,b*mp.cospi(c),b*mp.sinpi(c)]
    if kind == 4:
        first = [sum(conv(k)*v for k,v in zip(row,(a,b,c))) for row in M1]
        roots = [mp.sign(v)*mp.root(abs(v),3) for v in first]
        return [sum(conv(k)*v for k,v in zip(row,roots)) for row in M2]
    raise ValueError(kind)

def to_fraction(v):
    sign, mantissa, exponent, _ = v._mpf_
    f = F(mantissa*(1 if not sign else -1))
    return f * (F(2)**exponent)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2]/'tests/fixtures/fmt11/finite_oracles.hpp')
    parser.add_argument('--samples', type=int, default=8)
    args = parser.parse_args()
    rng = random.Random(110923)
    lines = ['// Generated by tools/fmt11/generate_oracles.py; do not hand-edit.', '#pragma once',
             'struct Fmt11Oracle { unsigned kind; bool narrow; std::array<std::uint64_t,3> input; unsigned component; std::uint64_t expected; };',
             'static constexpr Fmt11Oracle kFmt11Oracles[]{']
    for narrow in (False,True):
        for kind in range(18):
            for sample in range(args.samples):
                xs = [F(rng.randint(-128,128),32) for _ in range(3)]
                if kind in (3,7): xs[1] = abs(xs[1])
                results = rational(kind, xs)
                if results is None:
                    with mp.workdps(180):
                        first = [rounded(to_fraction(y),narrow) for y in transcendental(kind,xs)]
                    with mp.workdps(360):
                        second = [rounded(to_fraction(y),narrow) for y in transcendental(kind,xs)]
                    if first != second:
                        # Near-exact zero/pi landmarks have dedicated analytic tests.
                        continue
                    expected = first
                else:
                    expected = [None if y is None else rounded(F(y),narrow) for y in results]
                # NUM sinpi(integer) has the sign of its input at exact zero;
                # mpmath (and Fraction) cannot encode an IEEE negative zero.
                if kind in (3,7) and xs[2].denominator == 1 and xs[2] < 0 and xs[1] >= 0:
                    expected[2] = 1 << (31 if narrow else 63)
                inputs = ','.join(f'UINT64_C(0x{bits(float(v),narrow):x})' for v in xs)
                for component,result in enumerate(expected):
                    if result is not None:
                        lines.append(f'  {{{kind},{str(narrow).lower()},{{{inputs}}},{component},UINT64_C(0x{result:x})}},')
    lines.append('};')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text('\n'.join(lines)+'\n')
    print(f'{len(lines)-5} finite component fixtures -> {args.output}')
if __name__ == '__main__':
    main()
