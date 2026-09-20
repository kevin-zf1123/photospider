"""Independent Fraction and directed MPFR whole-formula shaper reference.

python3 shaper_oracle.py <photospider_numeric_shapers> strict|apple|x86
All four forms run through public Compiler/ExecutionContext. MPFR is reference
only; no host log/pow result is used as a whole-expression expected value.
"""
import math
import random
import subprocess
import sys
from fractions import Fraction as F

from comparison_oracle import number
from math_oracle_support import MPFR, multiply_bounds
from range_oracle import reference as remap
from sequence_oracle import ieee_round


def bits(value, dtype):
    return ieee_round(F(value), 32 if dtype == 4 else 64, False)


def rational_power(base, exponent):
    # Independent integer roots of the complete exact rational U/L, followed
    # by exact exponentiation, cover the inverse's true IEEE midpoint cases.
    n, d = base.numerator, base.denominator
    root = exponent.denominator
    while root > 1:
        a, b = math.isqrt(n), math.isqrt(d)
        if a*a != n or b*b != d:
            return None
        n, d, root = a, b, root//2
    if abs(exponent.numerator) * max(n.bit_length(), d.bit_length()) > 20000:
        return None
    return F(n, d) ** exponent.numerator


def reference(method, dtype, raw, lower, upper):
    width = 32 if dtype == 4 else 64
    fraction = 23 if dtype == 4 else 52
    sign = 1 << (width-1)
    infinity = 0x7f800000 if dtype == 4 else 0x7ff0000000000000
    quiet = 1 << (fraction-1)
    one = bits(1, dtype)
    x, l, u = (number(v, dtype) for v in (raw, lower, upper))
    if not isinstance(l, F) or not isinstance(u, F) or l >= u or (method >= 2 and l <= 0):
        return 'domain'
    if method < 2:
        args = [raw, lower, upper, 0, one] if not method else [raw, 0, one, lower, upper]
        return remap('remap_range', dtype, args)
    if x is None:
        return f'{raw | quiet:x}'
    inverse = method == 3
    if inverse:
        if not x:
            return f'{lower:x}'
        if x == 1:
            return f'{upper:x}'
        if x in (-math.inf, math.inf):
            return f'{0 if x < 0 else infinity:x}'
        exact = rational_power(u/l, x)
        if exact is not None:
            rounded = bits(l*exact, dtype)
            return f'{infinity if rounded is None else rounded:x}'
    else:
        if not x:
            return f'{infinity | sign:x}'
        if x < 0:
            return f'{infinity | quiet:x}'
        if x == math.inf:
            return f'{infinity:x}'
        if x == l:
            return '0'
        if x == u:
            return f'{one:x}'
    for precision in (128, 256, 512, 1024, 2048, 4096, 8192):
        with MPFR(precision) as oracle:
            down, up = oracle.downward, oracle.upward
            px, pl, pu = (oracle.raw(v, dtype) for v in (raw, lower, upper))
            ll = oracle.unary('log', pl, down), oracle.unary('log', pl, up)
            lu = oracle.unary('log', pu, down), oracle.unary('log', pu, up)
            delta = (oracle.binary('sub', lu[0], ll[1], down),
                     oracle.binary('sub', lu[1], ll[0], up))
            if oracle.compare(delta[0], oracle.integer(0)) <= 0:
                continue
            if inverse:
                product = multiply_bounds(oracle, (px, px), delta)
                z = (oracle.binary('add', ll[0], product[0], down),
                     oracle.binary('add', ll[1], product[1], up))
                output = oracle.unary('exp', z[0], down), oracle.unary('exp', z[1], up)
            else:
                lx = oracle.unary('log', px, down), oracle.unary('log', px, up)
                numerator = (oracle.binary('sub', lx[0], ll[1], down),
                             oracle.binary('sub', lx[1], ll[0], up))
                output = multiply_bounds(oracle, numerator, delta, divide=True)
            lo, hi = (oracle.bits(value, dtype) for value in output)
            if lo == hi:
                return f'{lo:x}'
    raise ArithmeticError(('unresolved independent shaper enclosure', method, dtype, raw, lower, upper))


def cases():
    rows = []
    rng = random.Random(20820)
    for dtype in (3, 4):
        sign = 1 << (31 if dtype == 4 else 63)
        infinity = 0x7f800000 if dtype == 4 else 0x7ff0000000000000
        one = bits(1, dtype)
        raw_special = [0, sign, 1, sign | 1, infinity-1, sign | (infinity-1),
                       infinity, sign | infinity, infinity | 0x42,
                       sign | infinity | 0x123, infinity | (1 << (22 if dtype == 4 else 51)) | 7]
        pairs = [(bits(1, dtype), bits(16, dtype)), (one, one+1),
                 (1, infinity-1), (1, 4), (infinity-2, infinity-1),
                 (bits(3, dtype), bits(27, dtype)), (bits(12, dtype), bits(27, dtype)),
                 (bits(18, dtype), bits(32, dtype)), (bits(F(1, 10), dtype), bits(10, dtype))]
        for method in range(4):
            for l, u in pairs + ([(sign, one), (sign | (infinity-1), infinity-1)] if method < 2 else []):
                for x in raw_special + [l, u, l-1 if l else 0, l+1, u-1, u+1] + [bits(F(i, 4), dtype) for i in range(-8, 13)]:
                    rows.append((method, dtype, x, l, u))
            # Invalid dynamic bounds, including lower positivity, outrank NaN.
            for l, u in [(one, one), (one, 0), (0, 0), (infinity, one),
                         (0, infinity), (infinity | 1, one), (sign | one, sign),
                         (sign, one), (0, one)]:
                rows.append((method, dtype, infinity | 0x42, l, u))
            for _ in range(100):
                l, u = sorted(rng.sample(range(1, infinity), 2))
                x = rng.randrange(infinity) | (sign if rng.randrange(2) else 0)
                if method < 2 and rng.randrange(2):
                    l |= sign
                rows.append((method, dtype, x, l, u))
        # Exact inverse midpoint (odd square exceeds precision by one bit),
        # cancellation of U/L denominator by lower, square roots and half-minsub.
        square = 4097 if dtype == 4 else 94906267
        for x, l, u in [(2, 1, square), (-1, 3, 9), (F(1, 2), 12, 27),
                         (F(1, 2), 18, 32), (F(1, 4), 32, 162),
                         (F(-1, 2), F(2)**(-149 if dtype == 4 else -1074),
                          F(2)**(-147 if dtype == 4 else -1072))]:
            rows.append((3, dtype, bits(x, dtype), bits(l, dtype), bits(u, dtype)))
        # Ordered near-boundary clusters cross exact and certified branches.
        for method in (2, 3):
            l, u = bits(1, dtype), bits(16, dtype)
            ordered = [bits(F(i, 32), dtype) for i in range(1, 65)]
            ordered += [one-2, one-1, one, one+1, one+2]
            rows += [(method, dtype, x, l, u) for x in sorted(set(ordered))]
    return rows


def main():
    rows = cases()
    expected = [reference(*row) for row in rows]
    encoded = '\n'.join(f'{m} {d} {x:x} {l:x} {u:x}' for m, d, x, l, u in rows)+'\n'
    profile = sys.argv[2] if len(sys.argv) > 2 else 'strict'
    result = subprocess.run([sys.argv[1], profile, '--probe'], input=encoded,
                            capture_output=True, text=True, check=True)
    actual = result.stdout.splitlines()
    assert len(actual) == len(rows), (len(actual), len(rows), result.stderr)
    for row, wanted, got in zip(rows, expected, actual):
        assert wanted == got, (row, wanted, got)
    # Grouping is only oracle verification; the product sees the original
    # stream order and never sorts values to force monotonicity.
    groups = {}
    for row, got in zip(rows, actual):
        m, d, x, l, u = row
        v = number(x, d)
        if m < 2 or got == 'domain' or not isinstance(v, F) or (m == 2 and v <= 0):
            continue
        groups.setdefault((m, d, l, u), []).append((v, number(int(got, 16), d)))
    for key, group in groups.items():
        ordered = sorted(group)
        assert all(a[1] <= b[1] for a, b in zip(ordered, ordered[1:])), key
    with MPFR(128) as oracle:
        version = oracle.version
    print(f'{len(rows)} Fraction/directed MPFR-{version} shaper cases, exact bits and monotonic groups PASS ({profile})')


if __name__ == '__main__':
    main()
