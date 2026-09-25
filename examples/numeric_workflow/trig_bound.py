"""Exact-rational certificate for trig_simd.cpp, not a sampled accuracy test.

IEEE binary32 nearest-even/gradual-underflow; each Horner step is one FMA.
Radian domain: |x| <= 1; sinpi/cospi domain: |x| <= 1/4. Full finite-FP32
sincpi uses integer reduction and a binary64 central polynomial. Nonzero sin/sinpi inputs
also require |x| >= 2^-120. Named pi landmarks are handled by CertifiedMath.
The proof includes coefficient errors, RN32(x*x), Horner rounding, Taylor
remainder, sine's final multiply and the correctly-rounded strict reference.
"""
from fractions import Fraction as F
from math import factorial
from pathlib import Path
import re


def power(n):
    return F(2) ** n


def atan_bounds(q, terms):
    # Alternating, strictly decreasing series for atan(1/q).
    total = sum((F((-1)**k, (2*k+1)*q**(2*k+1))
                 for k in range(terms)), F(0))
    next_term = F((-1)**terms, (2*terms+1)*q**(2*terms+1))
    return min(total, total+next_term), max(total, total+next_term)


def half_ulp(maximum, precision=24):
    if not maximum:
        return power(-150 if precision == 24 else -1075)
    exponent = maximum.numerator.bit_length()-maximum.denominator.bit_length()
    if power(exponent) > maximum:
        exponent -= 1
    # At an exact power of two, the endpoint is exactly representable; values
    # below it use the smaller spacing. Include underflow's absolute half-step.
    return max(power(exponent-(precision+1 if maximum == power(exponent) else precision)),
               power(-150 if precision == 24 else -1075))


def coefficients():
    source = (Path(__file__).resolve().parents[2] /
              'plugins/ops/01-numeric/trig_simd.cpp').read_text()
    result = {}
    for dtype, name, body in re.findall(r'constexpr (float|double) k(\w+)\[\] = \{([^}]+)\}', source):
        values = [token.strip().removesuffix('f') if dtype == 'float' else token.strip()
                  for token in body.split(',') if token.strip()]
        result[name.lower()] = [F(float.fromhex(token)) for token in values]
    return result


def main():
    # Machin's identity pi = 16 atan(1/5) - 4 atan(1/239).
    a, b = atan_bounds(5, 40)
    c, d = atan_bounds(239, 12)
    pi_low, pi_high = 16*a-4*d, 16*b-4*c
    assert 3 < pi_low < pi_high < F(22, 7)
    stored = coefficients()
    for name in ('sin', 'cos', 'sinpi', 'cospi', 'sinc'):
        normalized = 'pi' in name
        sine = name in ('sin', 'sinpi')
        cosine = name in ('cos', 'cospi')
        degree = 6 if cosine else 5
        radius = F(1, 4) if normalized else F(1)
        tmax = radius**2
        angle = pi_high*radius if normalized else radius
        coeff = stored['sinc' if name == 'sin' else name]
        assert len(coeff) == degree+1
        # Parse the actual C++ literals; exact dyadics must fit binary32.
        for ck in coeff:
            odd = abs(ck.numerator)
            while odd and odd % 2 == 0:
                odd //= 2
            assert odd.bit_length() <= 24
        coefficient_error = F(0)
        for k, ck in enumerate(coeff):
            exponent = 2*k+(name == 'sinpi')
            denominator = factorial(2*k if cosine else 2*k+1)
            low, high = ((pi_low**exponent/denominator,
                          pi_high**exponent/denominator) if normalized else
                         (F(1, denominator), F(1, denominator)))
            if k % 2:
                low, high = -high, -low
            coefficient_error += max(abs(low-ck), abs(high-ck))*tmax**k
        # t_hat is in [0,tmax]: tmax is exactly representable and rounding is
        # monotone. Natural intervals bound each computed FMA's magnitude.
        low = high = coeff[-1]
        arithmetic_error = F(0)
        for index in range(len(coeff)-2, -1, -1):
            ck = coeff[index]
            if index == 0:
                # Last correction is nonpositive. With c0=1 the even kernels
                # and the shared sinc factor cannot exceed one after RN32.
                assert high < 0
            low, high = ck+min(F(0), low*tmax), ck+max(F(0), high*tmax)
            rounding = half_ulp(max(abs(low), abs(high)))
            arithmetic_error = arithmetic_error*tmax+rounding
            low -= rounding
            high += rounding
        derivative = sum((k*abs(coeff[k])*tmax**(k-1)
                          for k in range(1, len(coeff))), F(0))
        square_error = half_ulp(tmax)
        # Alternating Taylor terms decrease on |angle|<=1; the next term
        # bounds the remainder, including pi factor in sinpi(x)/x.
        assert angle <= 1
        remainder = angle**(2*degree+2)/factorial(
            2*degree+2 if cosine else 2*degree+3)
        if name == 'sinpi':
            remainder *= pi_high
        error = (coefficient_error + arithmetic_error +
                 derivative*square_error + remainder)
        if sine:
            # P(x^2)>=1-x^2/6. sinpi(x)/x additionally has a factor pi.
            lower = (pi_low if name == 'sinpi' else 1)*(1-angle**2/6)
            assert power(-120)*lower*(1-power(-24)) > power(-126)
            # Error relative to the true sine: polynomial plus RN32(x*P).
            relative = error/lower + power(-24)*(1+error/lower)
            # Strict RN32 contributes <=u relative. Each adjacent step between
            # the two normal outputs is >= min(magnitudes)*2^-24, also when
            # crossing a binade. This accounts for both endpoints shrinking.
            total = relative+power(-24)
            steps = total/(1-total)*power(24)
        else:
            # Cosine has lower bound 1-a^2/2+a^4/24-a^6/720, decreasing for
            # 0<=a<=1. Sinc >=1-a^2/6. True outputs lie in (0.5,1].
            lower = (1-angle**2/2+angle**4/24-angle**6/720 if cosine
                     else 1-angle**2/6)
            assert lower-error-power(-25) > F(1, 2)
            # Strict reference error <=2^-25 on [0.5,1]; all intervening
            # representable steps are >=2^-24, even if a candidate exceeds 1.
            steps = (error+power(-25))/power(-24)
        assert steps < 4, (name, steps)
        print(f'{name}: absolute polynomial error <= {float(error):.16g}; '
              f'ordered reference steps < {float(steps):.16g} < 4')
    # Full finite-FP32 sincpi: exact n=RN-even(|x|), r=|x|-n. For x<2^23,
    # subtraction is exact (n=0, or x/n in [1/2,2], Sterbenz). r is itself
    # binary32 and |r|<=1/2; r*r has <=48 significand bits, hence exact in FP64.
    # x>=2^23 is integer and returns +0; x=0 returns 1. Nonzero integers below
    # 2^23 also return +0, separately from the floating sign reconstruction.
    coeff = stored['sincpiwide']
    assert len(coeff) == 9
    tmax = F(1, 4)
    coefficient_error = F(0)
    for k, ck in enumerate(coeff):
        low, high = pi_low**(2*k)/factorial(2*k+1), pi_high**(2*k)/factorial(2*k+1)
        if k % 2:
            low, high = -high, -low
        coefficient_error += max(abs(low-ck), abs(high-ck))*tmax**k
    low = high = coeff[-1]
    arithmetic_error = F(0)
    for index in range(len(coeff)-2, -1, -1):
        ck = coeff[index]
        if index == 0:
            assert high < 0  # central P cannot round above one
        low, high = ck+min(F(0), low*tmax), ck+max(F(0), high*tmax)
        rounding = half_ulp(max(abs(low), abs(high)), 53)
        arithmetic_error = arithmetic_error*tmax+rounding
        low -= rounding
        high += rounding
    angle = pi_high/2
    # Alternating terms still decrease at pi/2; next omitted term is degree18.
    assert angle**2 < 6
    error = coefficient_error+arithmetic_error+angle**18/factorial(19)
    lower = 1-angle**2/6
    assert lower > F(1, 2)
    # For x>=1/2, nonzero r is at least one binary32 spacing of x. Thus
    # |r/x|>2^-24. Both quotient and final result remain normal in FP32/FP64.
    assert lower*power(-24)*(1-power(-24)) > power(-126)
    # Hardware binary64 division and product each incur <=u64 relative error.
    relative = (1+error/lower)*(1+power(-53))**2-1
    # Final FP32 conversion, then independent RN32 strict-reference rounding.
    assert (2/pi_low)*(1+relative)*(1+power(-24)) < 1
    total = (1+relative)*(1+power(-24))-1+power(-24)
    steps = total/(1-total)*power(24)
    assert steps < 4
    print(f'sincpi full finite FP32: central absolute error <= {float(error):.16g}; '
          f'ordered reference steps < {float(steps):.16g} < 4')
    # x=+/-0 is handled exactly: even kernels return one, odd kernels multiply
    # a positive constant by signed zero. NaN/Inf, tiny sine arguments, and
    # named quarter-turn roots never enter an uncertified lane.


if __name__ == '__main__':
    main()
