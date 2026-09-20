"""Independent Fraction stencils and trapezoidal-prefix oracle for NUM-15."""
import math
import random
import subprocess
import sys
from fractions import Fraction
from comparison_oracle import number
from reduction_oracle import format_info, nan_convert
from sequence_oracle import ieee_round


def reference(integral, dtype, samples, index, step, initial):
    if integral and not index:
        return initial
    h = number(step, dtype)
    if h is None or h in (0, math.inf, -math.inf):
        return 'step'
    fraction, _, width = format_info(dtype)
    sign = 1 << (width-1)
    infinity = ((1 << (width-fraction-1))-1) << fraction
    nan = infinity | (1 << (fraction-1))
    if integral:
        terms = samples[:index+1]
        raw = [initial]+terms
    else:
        low, high = (0, 1) if index == 0 else (len(samples)-2, len(samples)-1) if index == len(samples)-1 else (index-1, index+1)
        terms = [samples[low], samples[high]]
        raw = terms
    for bits in raw:
        if number(bits, dtype) is None:
            return nan_convert(bits, dtype, dtype)
    values = [number(bits, dtype) for bits in terms]
    if integral:
        offset = number(initial, dtype)
        if math.inf in values and -math.inf in values:
            return nan
        if math.inf in values or -math.inf in values:
            negative = (-math.inf in values) != (h < 0)
            if offset in (math.inf, -math.inf) and (offset < 0) != negative:
                return nan
            return infinity | (sign if negative else 0)
        if offset in (math.inf, -math.inf):
            return initial
        exact = offset+h*(values[0]+2*sum(values[1:-1], Fraction())+values[-1])/2
    else:
        a, b = values
        if a in (math.inf, -math.inf) and b == a:
            return nan
        if a in (math.inf, -math.inf) or b in (math.inf, -math.inf):
            negative = ((b < 0) if b in (math.inf, -math.inf) else (a > 0)) != (h < 0)
            return infinity | (sign if negative else 0)
        exact = (b-a)/(h*(high-low))
    rounded = ieee_round(exact, width, False)
    return rounded if rounded is not None else infinity | (sign if exact < 0 else 0)


def cases():
    rng = random.Random(1501)
    for dtype in (3, 4):
        fraction, bias, width = format_info(dtype)
        sign = 1 << (width-1)
        one = bias << fraction
        two = (bias+1) << fraction
        half = (bias-1) << fraction
        infinity = ((1 << (width-fraction-1))-1) << fraction
        maximum = infinity-1
        pool = [0, sign, 1, sign|1, one, sign|one, maximum, sign|maximum,
                infinity, sign|infinity, infinity|0x42, sign|infinity|0x13]
        for integral in (0, 1):
            for size in ((1,2,3,4,65,129) if integral else (2,3,4,65,129)):
                for trial in range(30):
                    raw = lambda: rng.choice(pool) if trial < 20 else rng.getrandbits(width)
                    samples = [raw() for _ in range(size)]
                    step = raw() if trial < 10 else rng.choice([one, sign|one,1,sign|1,maximum,sign|maximum])
                    initial = raw()
                    for index in sorted({0, size//2, size-1}):
                        yield integral, dtype, samples, index, step, initial
        for samples,step,initial in [([sign|maximum,0,maximum],maximum,0),([0,1],1,0),
             ([0,1],sign|maximum,0),([sign,0],sign|1,0),([one,infinity|0x42,two],one,0)]:
            for index in range(len(samples)):
                yield 0,dtype,samples,index,step,initial
        for samples,step,initial in [([maximum,maximum],two,sign|maximum),([one,one],1,0),
             ([1,1],half,1),([1,1],half,sign|1),([maximum,maximum,sign|maximum,sign|maximum],two,0),
             ([sign|maximum,sign|maximum],two,infinity),([sign,sign],one,sign),
             ([infinity,sign|infinity,infinity|0x42],one,0),([one,one],one,infinity|0x13),
             ([one,one],0,infinity|0x13)]:
            for index in range(len(samples)):
                yield 1,dtype,samples,index,step,initial


def main():
    rows = list(cases())
    encoded, wanted = [], []
    for integral,dtype,samples,index,step,initial in rows:
        encoded.append(f'{integral} {dtype} {len(samples)} {index} {step:x} {initial:x} '+' '.join(f'{raw:x}' for raw in samples))
        value = reference(integral,dtype,samples,index,step,initial)
        wanted.append(value if isinstance(value,str) else f'{value:x}')
    result = subprocess.run([sys.argv[1], sys.argv[2] if len(sys.argv)>2 else 'strict', 'oracle'],
                            input='\n'.join(encoded)+'\n', text=True, capture_output=True, check=True)
    actual = result.stdout.splitlines()
    assert len(actual) == len(wanted), (len(actual),len(wanted),result.stderr)
    for index,(got,expected) in enumerate(zip(actual,wanted)):
        assert got == expected, (index,rows[index],got,expected)
    print(f'{len(rows)} independent exact calculus cases passed ({sys.argv[2] if len(sys.argv)>2 else "strict"})')


if __name__ == '__main__':
    main()
