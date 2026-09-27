"""Independent exact-rational/root and Decimal enclosure RGB ramp oracle.

Run: python3 rgb_ramp_oracle.py <photospider_numeric_color_ramps> strict|apple|x86
sRGB uses integer nth-root comparisons, with exact rational decimal constants.
General gamma uses Python Decimal correctly-rounded log/exp with outward
neighbors. Neither oracle imports production transfer or interval arithmetic.
"""
import bisect
import itertools
import random
import subprocess
import sys
from decimal import Decimal, localcontext, ROUND_FLOOR, ROUND_CEILING
from fractions import Fraction as F

from comparison_oracle import number
from sequence_oracle import ieee_round


def bits(value, dtype=3, negative_zero=False):
    return ieee_round(F(value), 32 if dtype == 4 else 64, negative_zero)


def root_floor(value, degree):
    if value < 2:
        return value
    current = 1 << ((value.bit_length() + degree - 1) // degree)
    while True:
        following = ((degree - 1) * current + value // current**(degree - 1)) // degree
        if following >= current:
            return current
        current = following


def rational_power(value, exponent, precision):
    if value == 0 or value == 1:
        return value, value
    p, q = exponent.numerator, exponent.denominator
    numerator = value.numerator**p << (precision*q)
    denominator = value.denominator**p
    floor = root_floor(numerator // denominator, q)
    lower = F(floor, 1 << precision)
    upper = lower if floor**q*denominator == numerator else F(floor+1, 1 << precision)
    return lower, upper


def decimal_interval(value, digits):
    with localcontext() as ctx:
        ctx.prec = digits
        ctx.rounding = ROUND_FLOOR
        lower = Decimal(value.numerator) / Decimal(value.denominator)
        ctx.rounding = ROUND_CEILING
        upper = Decimal(value.numerator) / Decimal(value.denominator)
        return lower, upper


def general_power(value, exponent, precision):
    if value in (0, 1):
        return value, value
    if exponent.numerator <= 12 and exponent.denominator <= 12:
        return rational_power(value, exponent, precision)
    digits = precision  # More than three decimal digits per binary target bit.
    with localcontext() as ctx:
        ctx.prec = digits
        lo, hi = decimal_interval(value, digits)
        # Decimal ln/exp are correctly rounded. Adjacent context numbers
        # enclose their exact real result regardless of tie direction.
        log = F(lo.ln().next_minus()), F(hi.ln().next_plus())
        product = log[0]*exponent, log[1]*exponent
        left = decimal_interval(product[0], digits)[0]
        right = decimal_interval(product[1], digits)[1]
        return F(left.exp().next_minus()), F(right.exp().next_plus())


def scale(interval, factor):
    return tuple(sorted((interval[0]*factor, interval[1]*factor)))


def add(a, b):
    return a[0]+b[0], a[1]+b[1]


def positive_transfer(value, transfer, gamma, encode, precision):
    if transfer == 0:
        return value, value
    if transfer == 2:
        return general_power(value, 1/gamma if encode else gamma, precision)
    if encode:
        if value <= F(7827, 2500000):
            return value*F(323, 25), value*F(323, 25)
        result = rational_power(value, F(5, 12), precision)
        return tuple(x*F(211, 200)-F(11, 200) for x in result)
    if value <= F(809, 20000):
        return value*F(25, 323), value*F(25, 323)
    return rational_power((value+F(11, 200))*F(200, 211), F(12, 5), precision)


def encode_interval(interval, transfer, gamma, precision):
    # Odd extension; the sRGB encoding join is not globally monotone.
    boundaries = [interval[0], interval[1]]
    if interval[0] < 0 < interval[1]:
        boundaries.append(F())
    join = F(7827, 2500000)
    values = []
    for point in boundaries:
        output = positive_transfer(abs(point), transfer, gamma, True, precision)
        values.extend(scale(output, -1 if point < 0 else 1))
    if transfer == 1:
        for sign in (-1, 1):
            if interval[0] <= sign*join <= interval[1]:
                values.append(sign*join*F(323, 25))
                high = rational_power(join, F(5, 12), precision)
                values.extend(sign*(x*F(211, 200)-F(11, 200)) for x in high)
    return min(values), max(values)


def reference(association, output, types, policy, query, stops, colors, transfer, gamma):
    qt, st, ct, ot = types
    q = number(query, qt)
    s = [number(x, st) for x in stops]
    finite = lambda value: isinstance(value, F)
    if not all(map(finite, s)) or any(a >= b for a, b in zip(s, s[1:])):
        return 'domain'
    if not finite(q) or (policy and not s[0] <= q <= s[-1]):
        return 'domain'
    insertion = bisect.bisect_left(s, q)
    if insertion < len(s) and s[insertion] == q:
        selected = [insertion]
    elif insertion in (0, len(s)):
        selected = [0 if insertion == 0 else len(s)-1]
    else:
        selected = [insertion-1, insertion]
    channels = 4 if association else 3
    raw = [colors[i*channels:(i+1)*channels] for i in selected]
    rows = [[number(x, ct) for x in row] for row in raw]
    for row in rows:
        if not all(map(finite, row)):
            return 'domain'
        if association and (not 0 <= row[3] <= 1 or
                            (association == 2 and row[3] == 0 and any(row[:3]))):
            return 'association'
    direct = len(rows) == 1 or raw[0] == raw[1]
    w = F() if direct else (q-s[selected[0]])/(s[selected[1]]-s[selected[0]])
    if len(rows) == 1:
        rows.append(rows[0])
    alpha = [row[3] if association else F(1) for row in rows]
    a = (1-w)*alpha[0]+w*alpha[1]
    if not a:
        return ' '.join(['0']*channels)
    rounded = []
    for channel in range(3):
        source = [row[channel] if association != 2 else row[channel]/ai if ai else F()
                  for row, ai in zip(rows, alpha)]
        if direct:
            exact = source[0]*(a if output == 2 else 1)
            result = bits(exact, ot, raw[0][channel] == (1 << (31 if ct == 4 else 63)))
        else:
            beta = [(1-w)*alpha[0], w*alpha[1]]
            for precision in (128, 256, 512, 1024, 2048):
                if transfer == 2 and source[0] == source[1]:
                    encoded = source[0], source[0]
                elif transfer == 2 and not beta[0]:
                    encoded = source[1], source[1]
                elif transfer == 2 and not beta[1]:
                    encoded = source[0], source[0]
                else:
                    decoded = [positive_transfer(abs(x), transfer, gamma, False, precision)
                               if b else (F(), F()) for x, b in zip(source, beta)]
                    linear = add(scale(decoded[0], beta[0]*(-1 if source[0] < 0 else 1)/a),
                                 scale(decoded[1], beta[1]*(-1 if source[1] < 0 else 1)/a))
                    # Algebraic equality may be provable before interval work.
                    if source[0] == -source[1] and beta[0] == beta[1]:
                        linear = F(), F()
                    if transfer == 2 and gamma.denominator == 1 and gamma <= 8:
                        exact = sum(b*(-1 if x < 0 else 1)*abs(x)**int(gamma)
                                    for x, b in zip(source, beta))/a
                        linear = exact, exact
                    encoded = encode_interval(linear, transfer, gamma, precision)
                final = scale(encoded, a if output == 2 else F(1))
                lo, hi = bits(final[0], ot), bits(final[1], ot)
                if lo == hi:
                    result = lo
                    break
            else:
                raise AssertionError('independent RGB enclosure unresolved')
        if result is None:
            return 'overflow'
        rounded.append(result)
    alpha_bits = bits(a, ot)
    if not alpha_bits:
        if any(x & ((1 << (31 if ot == 4 else 63))-1) for x in rounded):
            return 'association_underflow'
        rounded = [0, 0, 0]
    if association:
        rounded.append(alpha_bits)
    return ' '.join(f'{x:x}' for x in rounded)


def cases():
    records = []
    rng = random.Random(20260921)
    def case(association=0, output=0, types=(3, 3, 3, 3), transfer=1, gamma=2.2,
             query=F(1, 2), stops=(0, 1), colors=(0, 0, 0, 1, 1, 1), policy=0, raw=False):
        qt, st, ct, ot = types
        g = bits(gamma)
        q = query if raw else bits(query, qt)
        ss = list(stops) if raw else [bits(x, st) for x in stops]
        cc = list(colors) if raw else [bits(x, ct) for x in colors]
        expected = reference(association, output, types, policy, q, ss, cc,
                             transfer, number(g, 3))
        line = f'rgb {association} {output} {qt} {st} {ct} {ot} {policy} {len(ss)} {transfer} {g:x} '
        line += ' '.join(f'{x:x}' for x in [q]+ss+cc)
        records.append((line, expected))
    for types in itertools.product((3, 4), repeat=4):
        for transfer, gamma in ((0, 1), (1, 1), (2, 2), (2, 2.2), (2, F(1, 2)), (2, 3)):
            case(types=types, transfer=transfer, gamma=gamma)
        for association, output in itertools.product((1, 2), repeat=2):
            case(association, output, types, colors=(F(1, 4), F(-1, 8), 0, F(1, 2),
                                                     2, -1, F(1, 4), 1))
    for _ in range(120):
        association = rng.randrange(3)
        output = rng.choice((1, 2)) if association else 0
        colors = []
        for i in range(2):
            colors.extend(F(rng.randrange(-128, 129), 64) for _ in range(3))
            if association:
                colors.append(F(rng.randrange(1, 17), 16))
        case(association, output, transfer=rng.randrange(3), gamma=rng.choice((1.25, 2.2, 3, 4)),
             query=F(rng.randrange(1, 16), 16), colors=colors)
    for transfer in (0, 1, 2):
        for output in (1, 2):
            for query in (0, F(1, 2), 1):
                case(1, output, transfer=transfer, query=query, colors=(1, -1, 2, 0, 0, 0, 1, 1))
        case(transfer=transfer, colors=(-2, -1, 0, 2, 1, 0))
    for exponent in range(3, 9):
        case(transfer=2, gamma=exponent, query=1, stops=(0, 2**exponent+1), colors=(1, 0, 0, -2, 0, 0))
    for transfer, gamma in ((0, 1), (1, 1), (2, 2), (2, 2.2), (2, F(1, 4)), (2, 8)):
        # Restoring premultiplied straight RGB exceeds binary64 here, while
        # the complete premultiplied output is finite. IEEE intermediate
        # overflow must not replace real arithmetic.
        case(2, 2, transfer=transfer, gamma=gamma,
             colors=(F(2)**1000, 0, 0, F(2)**-1074,
                     F(2)**999, 0, 0, F(2)**-1074))
        case(transfer=transfer, gamma=gamma,
             stops=(-F(2)**1023, F(2)**1023), query=0,
             colors=(F(2)**-1074, -F(2)**-1074, 0, F(2)**-1073, -F(2)**-1073, 0))
    for edge in (F(809, 20000), F(2528121, 62500000)):
        center = bits(edge)
        for delta in range(-2, 3):
            color = center+delta
            case(1, 1, raw=True, colors=(color, 0, 0, bits(F(1, 2)), color, 0, 0, bits(1)),
                 query=bits(F(1, 2)), stops=(0, bits(1)))
    for association, output in itertools.product((1, 2), repeat=2):
        for query in (0, F(1, 2), 1):
            case(association, output, query=query, colors=(F(1, 4), -0., 1, F(1, 2))*2)
    for output in (1, 2):
        case(1, output, types=(3, 3, 3, 4), query=0,
             colors=(2**30, 0, 0, F(1, 2**150), 0, 0, 0, 1))
    case(1, 2, types=(3, 3, 3, 4), query=0,
         colors=(F(1, 1024), 0, 0, F(1, 2**150), 0, 0, 0, 1))
    case(2, 2, colors=(1, 0, 0, 0, 0, 0, 0, 1))
    case(1, 2, colors=(1, 0, 0, -1, 0, 0, 0, 1))
    # Demanded versus remote invalid color, invalid stops/query and K=1.
    for q in (0, F(1, 2), 1):
        case(raw=True, query=bits(q), stops=(0, bits(1)), colors=(0, 0, 0, 0x7ff8000000000000, 0, 0))
    case(query=3, stops=(0,), colors=(1, 2, -1))
    case(query=3, stops=(0,), colors=(1, 2, -1), policy=1)
    # For g>2^996 and sources 1,2 at half weight: encoded lies between
    # 2*2^(-1/g) and 2. Since 1-exp(-ln(2)/g)<1/g, its distance below 2
    # is <2^-995, far below half a binary64 ULP. This analytical fixture
    # independently proves a huge exponent case without Decimal overflow.
    line = f'rgb 0 0 3 3 3 3 0 2 2 {bits(1e300):x} {bits(F(1,2)):x} 0 {bits(1):x} '
    line += ' '.join(f'{bits(x):x}' for x in (1, 1, 1, 2, 2, 2))
    records.append((line, '4000000000000000 4000000000000000 4000000000000000'))
    return records


if __name__ == '__main__':
    records = cases()
    run = subprocess.run([sys.argv[1], sys.argv[2], '--probe'],
                         input='\n'.join(x for x, _ in records)+'\n', text=True,
                         capture_output=True, check=False)
    actual = run.stdout.splitlines()
    for index, ((line, expected), observed) in enumerate(zip(records, actual)):
        if expected != observed:
            raise AssertionError(f'case {index}: {line}\nexpected {expected}\nactual {observed}')
    if run.returncode or len(actual) != len(records):
        raise AssertionError(f'probe stopped at {len(actual)}/{len(records)}: {run.stderr}')
    print(f'RGB independent rational/root/Decimal oracle: {len(records)} cases {sys.argv[2]} PASS')
