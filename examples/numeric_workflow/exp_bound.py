"""Exact-rational enclosure for the normal-range IQK expf polynomial.

This is an analytic certificate over intervals, not sampled exp evaluations.
Run with Python 3; production does not import it. Mirrors exp_simd.cpp's
explicit FMA graph, IEEE nearest/gradual binary32, |x| <= 80.
"""
from fractions import Fraction as F
from math import comb, factorial


def binary(text):
    return F(float.fromhex(text))


def power(n):
    return F(2) ** n


def main():
    # ln(2) = 2 atanh(1/3), positive series with geometric tail bound.
    terms = 48
    ln_low = sum((F(2, (2*k+1)*3**(2*k+1)) for k in range(terms)), F(0))
    ln_high = ln_low + F(2, (2*terms+1)*3**(2*terms+1)) / (1-F(1, 9))
    log2e = binary('0x1.715476p+0')
    log_error = max(abs(log2e-1/ln_low), abs(log2e-1/ln_high))
    hi, lo = binary('0x1.62e4p-1'), binary('0x1.7f7d1cp-20')
    split_error = max(abs(hi+lo-ln_low), abs(hi+lo-ln_high))
    # FMA with 1.5*2^23 has spacing 1 throughout this domain; subtraction
    # gives an exact integer n, |n| <= 116. No separate x*log2e rounding.
    magic = 3*power(22)
    assert power(23) < magic-80*log2e < magic+80*log2e < power(24)
    n_error = F(1, 2)+80*log_error
    assert 80*log2e+F(1, 2) < 116
    radius = F(347, 1000)
    exact_reduced = n_error*ln_high
    first = exact_reduced + 116*max(abs(ln_low-hi), abs(ln_high-hi))
    assert first < radius < F(1, 2)
    # The first reduction FMA is exact; the second has error <= 2^-26.
    # n*hi is exactly representable; x and n*hi satisfy Sterbenz for
    # n != 0 since .75 < hi*log2e < 1. For n=0 the first FMA is x.
    assert (116*hi.numerator).bit_length() <= 24
    assert F(3,4) < hi*log2e < 1
    reduction_error = power(-26) + 116*split_error
    assert exact_reduced + reduction_error < radius

    coefficients = [F(1), binary('0x1.ffffecp-1'), binary('0x1.fffdb6p-2'),
                    binary('0x1.555e66p-3'), binary('0x1.573e2ep-5'),
                    binary('0x1.0e4020p-7')]
    degree = 14
    # P(b)-Taylor14(exp,b), translated to each interval center exactly.
    difference = [(coefficients[k] if k < 6 else F(0))-F(1, factorial(k))
                  for k in range(degree+1)]
    pieces = 256
    half_width = radius/pieces
    polynomial_errors = [F(0), F(0)]
    for i in range(pieces):
        center = -radius+(2*i+1)*half_width
        translated = [sum((difference[k]*comb(k,j)*center**(k-j)
                           for k in range(j, degree+1)), F(0))
                      for j in range(degree+1)]
        bound = sum((abs(c)*half_width**j for j,c in enumerate(translated)), F(0))
        side = int(center >= 0)
        polynomial_errors[side] = max(polynomial_errors[side], bound)
    # exp(radius) < 1/(1-radius); same bound controls Taylor's remainder
    # and exp's Lipschitz constant between the two reduced arguments.
    exp_upper = 1/(1-radius)
    polynomial_errors = [e + exp_upper*radius**(degree+1)/factorial(degree+1)
                         for e in polynomial_errors]

    # Absolute error propagation over the exact explicit FMA graph.
    # Bounds on intermediate magnitudes justify each half-ULP below.
    c1,c2,c3,c4,c5 = coefficients[1:]
    u = radius**2
    eu = power(-28)  # b*b < 1/8
    a = c2+c3*radius
    ea = power(-25)  # c2+c3*b < 1
    d = c4+c5*radius
    ed = power(-29)  # c4+c5*b < 1/16
    assert u < F(1,8) and a < 1 and d < F(1,16)
    q = a+d*u
    eq = ea + ed*u + (d+ed)*eu + power(-25)
    assert q+eq < 1
    v = c1*radius
    ev = power(-26)
    assert v < F(1,2)
    j = v+q*u
    ej = ev + eq*u + (q+eq)*eu + power(-26)
    assert j+ej < F(1,2)
    # Final FMA k+j*k is exact power-of-two scaling followed by one
    # binary32 rounding. |n| <= 116 keeps all output values normal.
    # The exponent-bit construction is exact: z has spacing one, so its
    # word is word(magic)+n. Shifting by 23 and adding word(1) gives 2^n;
    # -116 <= n <= 116 stays away from both exponent-field endpoints.
    # Outside |b| <= 10^-6, exp is at least |b|*(1-radius) away from 1,
    # less the reduction error. A coarse error with the larger half-ULP
    # excludes crossing that binade before choosing the tighter spacing.
    # Inside this neighborhood both roundings use the larger half-ULP,
    # while distance uses the smaller spacing.
    near_zero = F(1, 1000000)
    for side, polynomial_error in enumerate(polynomial_errors):
        spacing = power(-24 if side == 0 else -23)

        total = polynomial_error + exp_upper*reduction_error + ej + spacing/2
        bound = total + spacing/2
        print('side', side, 'polynomial_error', float(polynomial_error),
              'reference_steps <', float(bound/spacing))
        coarse = polynomial_error + exp_upper*reduction_error + ej + power(-24)
        assert coarse < near_zero*(1-radius)-exp_upper*reduction_error
        assert bound < 4*spacing, 'four-step certificate failed'
    # Bound |P(b)-exp(b)| near zero by Taylor coefficient differences.
    near_poly = sum((abs(difference[k])*near_zero**k
                     for k in range(degree+1)), F(0))
    near_poly += exp_upper*near_zero**(degree+1)/factorial(degree+1)
    near_total = near_poly + exp_upper*reduction_error + ej + power(-23)
    assert near_total < 4*power(-24)
    print('near zero reference_steps <', float(near_total/power(-24)))
    print('PASS: exact rational interval certificate, finite Float32 [-80,80]')


if __name__ == '__main__':
    main()
