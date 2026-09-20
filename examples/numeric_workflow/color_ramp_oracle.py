"""Independent Fraction/IEEE and alternating-series pi color-ramp oracle.

Run: python3 color_ramp_oracle.py <photospider_numeric_color_ramps> strict|apple|x86
No production arithmetic, pi constant, rounding or transfer helper is imported.
"""
import bisect
import functools
import itertools
import random
import subprocess
import sys
from fractions import Fraction

from comparison_oracle import number
from sequence_oracle import ieee_round


def bits(value, dtype=3, minus=False):
    return ieee_round(Fraction(value), 32 if dtype == 4 else 64, minus)


@functools.lru_cache(None)
def pi_interval(terms):
    # Machin's identity with exact alternating-series remainder bounds.
    def atan_inverse(q):
        total = Fraction()
        for n in range(terms):
            total += Fraction((-1)**n, (2*n+1)*q**(2*n+1))
        following = Fraction((-1)**terms, (2*terms+1)*q**(2*terms+1))
        return min(total, total+following), max(total, total+following)
    a, b = atan_inverse(5), atan_inverse(239)
    return 16*a[0]-4*b[1], 16*a[1]-4*b[0]


def rounded(exact, dtype, negative_zero=False, pi_power=0):
    if not pi_power or not exact:
        return bits(exact, dtype, negative_zero)
    for terms in (96, 192, 384, 768, 1536):
        lo, hi = pi_interval(terms)
        if pi_power < 0:
            lo, hi = 1/hi, 1/lo
        a, b = sorted((exact*lo, exact*hi))
        ra, rb = bits(a, dtype), bits(b, dtype)
        if ra == rb:
            return ra
    raise AssertionError('independent pi rounding unresolved')


def reference(model, unit, output_unit, qt, st, ct, ot, policy,
              query_raw, stops_raw, colors_raw, p, denominator):
    q = number(query_raw, qt)
    stops = [number(x, st) for x in stops_raw]
    finite = lambda x: isinstance(x, Fraction)
    if not all(map(finite, stops)) or any(a >= b for a, b in zip(stops, stops[1:])):
        return 'domain'
    if not finite(q) or (policy and not stops[0] <= q <= stops[-1]):
        return 'domain'
    polar = model in ('cielch', 'oklch', 'hsl')
    split = polar and unit == 2
    channels = 4 if model == 'cmyk' else 2 if split else 3
    insertion = bisect.bisect_left(stops, q)
    if insertion < len(stops) and stops[insertion] == q:
        selected = [insertion]
    elif insertion == 0 or insertion == len(stops):
        selected = [0 if insertion == 0 else len(stops)-1]
    else:
        selected = [insertion-1, insertion]
    raw_rows = [colors_raw[row*channels:(row+1)*channels] for row in selected]
    rows = [[number(x, ct) for x in row] for row in raw_rows]
    for row in rows:
        if not all(map(finite, row)):
            return 'domain'
        if model == 'cmyk' and any(not 0 <= x <= 1 for x in row):
            return 'domain'
        if model in ('cielch', 'oklch') and row[1] < 0:
            return 'domain'
    if split and any(denominator[row] <= 0 for row in selected):
        return 'domain'
    direct = len(selected) == 1 or (raw_rows[0] == raw_rows[1] and
             (not split or (p[selected[0]], denominator[selected[0]]) ==
                           (p[selected[1]], denominator[selected[1]])))
    weight = Fraction() if direct else (q-stops[selected[0]])/(stops[selected[1]]-stops[selected[0]])
    result = []
    for channel in range(4 if model == 'cmyk' else 3):
        hue = polar and channel == (0 if model == 'hsl' else 2)
        if hue and split:
            samples = [Fraction(p[row], denominator[row]) for row in selected]
            negative_zero = False
        else:
            index = channel-(1 if split and model == 'hsl' else 0)
            samples = [row[index] for row in rows]
            negative_zero = direct and raw_rows[0][index] == (1 << (31 if ct == 4 else 63))
        exact = samples[0] if direct else (1-weight)*samples[0]+weight*samples[1]
        pi_power = (1 if unit != 0 and output_unit == 0 else
                    -1 if unit == 0 and output_unit == 1 else 0) if hue else 0
        value = rounded(exact, ot, negative_zero, pi_power)
        if value is None:
            return 'overflow'
        result.append(f'{value:x}')
    return ' '.join(result)


def cases():
    rng = random.Random(20260920)
    records = []
    def add(model, unit, output_unit, types=(3, 3, 3, 3), policy=0,
            query=Fraction(1, 2), stops=(0, 1), colors=None, p=None, d=None,
            raw=False):
        qt, st, ct, ot = types
        split = model in ('cielch', 'oklch', 'hsl') and unit == 2
        channels = 4 if model == 'cmyk' else 2 if split else 3
        if colors is None:
            colors = [0]*channels + [Fraction(j+1, 8) for j in range(channels)]
        p = [7, 1] if p is None else p
        d = [4, 4] if d is None else d
        if not raw:
            query = bits(query, qt)
            stops = [bits(x, st) for x in stops]
            colors = [bits(x, ct) for x in colors]
        header = f'{model} {unit} {output_unit} {qt} {st} {ct} {ot} {policy} {len(stops)}'
        payload = ' '.join(f'{x:x}' for x in [query, *stops, *colors])
        if split:
            payload += ' ' + ' '.join(map(str, [*p, *d]))
        expected = reference(model, unit, output_unit, qt, st, ct, ot, policy,
                             query, stops, colors, p, d)
        records.append((header+' '+payload, expected))
    for model in ('xyz', 'cmyk', 'cielab', 'oklab', 'ycbcr', 'cielch', 'oklch', 'hsl'):
        polar = model in ('cielch', 'oklch', 'hsl')
        for unit in (range(3) if polar else (0,)):
            for output_unit in (range(2) if polar else (0,)):
                for types in itertools.product((3, 4), repeat=4):
                    add(model, unit, output_unit, types)
                for query in (-1, 0, Fraction(1, 8), 1, 2):
                    for policy in (0, 1):
                        add(model, unit, output_unit, query=query, policy=policy)
                channels = 4 if model == 'cmyk' else 2 if polar and unit == 2 else 3
                for ct in (3, 4):
                    sign = 1 << (63 if ct == 3 else 31)
                    one = bits(1, ct)
                    zeros = [sign]*channels
                    for same in (False, True):
                        other = zeros.copy()
                        if not same: other[-1] = one
                        add(model, unit, output_unit, (3, 3, ct, ct), raw=True,
                            query=bits(Fraction(1, 2)), stops=[0, bits(1)],
                            colors=zeros+other)
                    inf = 0x7ff0000000000000 if ct == 3 else 0x7f800000
                    for bad in (inf, inf | 1, sign | inf):
                        for selected in (False, True):
                            colors = [0]*(3*channels)
                            colors[0 if not selected else 2*channels] = bad
                            add(model, unit, output_unit, (3, 3, ct, ct), raw=True,
                                query=bits(2), stops=[0, bits(1), bits(2)],
                                colors=colors, p=[0, 0, 0], d=[1, 1, 1])
                for _ in range(24):
                    types = tuple(rng.choice((3, 4)) for _ in range(4))
                    colors = [Fraction(rng.randrange(-2048, 2049), 128) for _ in range(2*channels)]
                    if model == 'cmyk': colors = [abs(x)/16 for x in colors]
                    if model in ('cielch', 'oklch'):
                        colors[1], colors[channels+1] = abs(colors[1]), abs(colors[channels+1])
                    add(model, unit, output_unit, types,
                        query=Fraction(rng.randrange(17), 16), colors=colors,
                        p=[rng.randrange(-(1 << 63), 1 << 63) for _ in range(2)],
                        d=[rng.randrange(1, 1 << 63) for _ in range(2)])
                for selected in (False, True):
                    add(model, unit, output_unit, query=0 if selected else 1,
                        p=[-(1 << 63), (1 << 63)-1], d=[0, 1])
                for query in (-2, 0, 2):
                    for policy in (0, 1):
                        add(model, unit, output_unit, query=query, stops=[0],
                            colors=[0]*channels, p=[0], d=[1], policy=policy)
                for bad_stops in ((0, 0), (1, 0)):
                    add(model, unit, output_unit, stops=bad_stops)
    # Exact cancellation and final underflow/overflow under very wide exponents.
    for model in ('xyz', 'cielab', 'oklab', 'ycbcr', 'hsl'):
        for destination in (3, 4):
            for opposite in (True, False):
                largest = 0x7fefffffffffffff
                colors = [largest]*3 + [(largest | (1 << 63)) if opposite else largest]*3
                add(model, 0, 0, (3, 3, 3, destination), raw=True,
                    query=bits(Fraction(1, 2)), stops=[0, bits(1)], colors=colors)
            add(model, 0, 0, (3, 3, 3, destination), raw=True,
                query=bits(Fraction(3, 4)), stops=[0, bits(1)],
                colors=[(1 << 63)|1]*3 + [0]*3)
    for model in ('cielch', 'oklch', 'hsl'):
        add(model, 2, 1, p=[(1 << 63)-1, -(1 << 63)], d=[1, 1])
        add(model, 2, 1, p=[14, 2], d=[8, 8])
    return records


def main():
    records = cases()
    result = subprocess.run([sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'strict', '--probe'],
                            input='\n'.join(row for row, _ in records)+'\n',
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    actual = result.stdout.splitlines()
    assert len(actual) == len(records), (len(actual), len(records), result.stderr)
    for i, ((row, expected), found) in enumerate(zip(records, actual)):
        assert found == expected, (i, row, expected, found)
    print(f'Color ramp independent Fraction/Machin-pi: {len(records)} cases PASS')


if __name__ == '__main__':
    main()
