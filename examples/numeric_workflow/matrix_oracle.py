"""Independent Fraction affine dot products and raw source-priority oracle."""
import math
import random
import subprocess
import sys
from fractions import Fraction
from comparison_oracle import number
from reduction_oracle import format_info, nan_convert
from sequence_oracle import ieee_round


def reference(dtype, x, row, bias):
    fraction, _, width = format_info(dtype)
    sign = 1 << (width-1)
    infinity = ((1 << (width-fraction-1))-1) << fraction
    nan = infinity | (1 << (fraction-1))
    for raw in x+row+[bias]:
        if number(raw, dtype) is None:
            return nan_convert(raw, dtype, dtype)
    products = []
    negative_zeros = []
    for a, b in zip(x, row):
        av, bv = number(a, dtype), number(b, dtype)
        if (av == 0 and bv in (math.inf, -math.inf)) or (bv == 0 and av in (math.inf, -math.inf)):
            return nan
        negative = bool((a ^ b) & sign)
        negative_zeros.append(negative and (av == 0 or bv == 0))
        products.append((-math.inf if negative else math.inf) if av in (math.inf, -math.inf) or bv in (math.inf, -math.inf) else av*bv)
    terms = products+[number(bias, dtype)]
    if math.inf in terms and -math.inf in terms:
        return nan
    if math.inf in terms or -math.inf in terms:
        return infinity | (sign if -math.inf in terms else 0)
    total = sum(terms, Fraction())
    if total == 0:
        return sign if all(negative_zeros) and bias == sign else 0
    rounded = ieee_round(total, width, False)
    return rounded if rounded is not None else infinity | (sign if total < 0 else 0)


def cases():
    rng = random.Random(1401)
    for dtype in (3, 4):
        fraction, bias, width = format_info(dtype)
        sign = 1 << (width-1)
        one = bias << fraction
        half = (bias-1) << fraction
        infinity = ((1 << (width-fraction-1))-1) << fraction
        maximum = infinity-1
        pool = [0, sign, 1, sign|1, one, sign|one, maximum, sign|maximum,
                infinity, sign|infinity, infinity|0x42, sign|infinity|(1 << (fraction-1))|0x13]
        for cin in (2, 3, 4):
            for cout in (2, 3, 4):
                for shape in ((cin,), (2, cin), (2, 2, cin)):
                    count = math.prod(shape)
                    for kind in range(20):
                        raw = lambda: rng.choice(pool) if kind < 8 else rng.getrandbits(width)
                        yield dtype, cout, shape, [raw() for _ in range(count)], [raw() for _ in range(cin*cout)], [raw() for _ in range(cout)]
        # Ordinary mantissas exercise the certified blocks and all nine shapes,
        # while adjacent block lengths change BLAS dimensions and tail kernels.
        for cin in (2, 3, 4):
            for cout in (2, 3, 4):
                for rows in (63, 64, 65, 129):
                    raw = lambda: ((bias + rng.randrange(-3, 4)) << fraction) | rng.getrandbits(fraction)
                    x = [raw() for _ in range(rows * cin)]
                    matrix = [raw() for _ in range(cin * cout)]
                    offsets = [raw() for _ in range(cout)]
                    # One lane forces cancellation and one keeps a source NaN.
                    x[cin:2*cin] = [one] * cin
                    x[2*cin] = infinity | 0x42
                    yield dtype, cout, (rows, cin), x, matrix, offsets
        epsilon_half = (bias-fraction-1) << fraction
        fixed = [([maximum, maximum], [maximum, sign|maximum], one),
                 ([one+1, one], [one-2, sign|one], 0),
                 ([1,1], [half,half], 0), ([1,0], [sign|half,one], sign),
                 ([one,epsilon_half], [one,one], 0),
                 ([one+1,epsilon_half], [one,one], 0),
                 ([sign,0], [one,sign|one], sign), ([sign,0], [one,sign|one], 0),
                 ([one,one], [one,sign|one], sign), ([0,0], [one,one], 1),
                 ([one,infinity|0x11], [infinity|0x22,one], infinity|0x33),
                 ([0,one], [infinity,one], infinity|0x33),
                 ([0,one], [infinity,one], 0),
                 ([infinity,infinity], [one,sign|one], 0),
                 ([infinity,one], [one,one], sign|infinity)]
        for x,row,b in fixed:
            yield dtype, 2, (2,), x, row+row, [b,b]


def main():
    rows = list(cases())
    encoded, wanted = [], []
    for dtype, cout, shape, x, matrix, bias in rows:
        cin = shape[-1]
        encoded.append(f'{dtype} {cout} '+','.join(map(str, shape))+' '+' '.join(f'{raw:x}' for raw in x+matrix+bias))
        values = [reference(dtype, x[i:i+cin], matrix[r*cin:(r+1)*cin], bias[r])
                  for i in range(0, len(x), cin) for r in range(cout)]
        wanted.append(' '.join(f'{value:x}' for value in values))
    result = subprocess.run([sys.argv[1], sys.argv[2] if len(sys.argv)>2 else 'strict', 'oracle'],
                            input='\n'.join(encoded)+'\n', text=True, capture_output=True, check=True)
    answers = result.stdout.splitlines()
    assert len(answers) == len(wanted), (len(answers), len(wanted), result.stderr)
    for index, (actual, expected) in enumerate(zip(answers, wanted)):
        assert actual == expected, (index, rows[index], actual, expected)
    print(f'{len(rows)} independent exact affine cases passed ({sys.argv[2] if len(sys.argv)>2 else "strict"})')


if __name__ == '__main__':
    main()
