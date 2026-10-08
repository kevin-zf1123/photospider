# Certified SIMD trigonometric kernels

The SIMD implementation accelerates the existing Float32 NUM-04 profiles on
macOS ARM NEON and Linux x86 AVX2/FMA.
The public definitions, four ordered-step bound, exact special/landmark rules,
and strict default remain unchanged. Float64 inputs keep their existing paths;
these Float32 certificates are not used as NUM-01 RN64 AST enclosures or as
independent per-tap allowances in CRV filters.

## Algorithms and proved domains

| Function | Polynomial/identity | Admitted Float32 arguments |
| --- | --- | --- |
| sin | `x*P(x*x)`, sharing P with sinc | zero, or `2^-120 <= abs(x) <= 1` |
| cos | even degree-12 Taylor polynomial | `abs(x) <= 1` |
| sinpi | `x*Ppi(x*x)`, pi powers in coefficients | zero, or `2^-120 <= abs(x) < 1/4` |
| cospi | even polynomial with pi powers in coefficients | `abs(x) < 1/4` |
| sinc | direct even degree-10 polynomial | `abs(x) <= 1` |
| sincpi | exact integer reduction and central polynomial | all finite Float32 |

The radian sinc polynomial is

```
P(t) = 1 - t/3! + t^2/5! - t^3/7! + t^4/9! - t^5/11!
```

Its SIMD core is one square and five explicit FMAs, with no sine call or
division. Sin adds one multiplication whose rounding is proved separately.
Cos uses six FMAs. Sinpi/cospi coefficients approximate the exact pi-scaled
function of the original binary input; a rounded `pi*x` is not substituted.
The exact quarter-turn roots continue through the algebraic implementation.

For normalized sinc, first use evenness and let `a=abs(x)`. Every finite
Float32 with `a>=2^23` is an integer and returns +0. Otherwise compute
`n=round_even(a)` and `r=a-n` exactly. Sterbenz applies when `n>=1`; the `n=0`
case simply has `r=a`. Then

```
sincpi(x) = (-1)^n * (r/a) * sincpi(r),  abs(r) <= 1/2.
```

A degree-16 even Taylor polynomial evaluates the central sincpi. Binary64
SIMD intermediates are used for this polynomial, hardware division and final
multiply, followed by RN32. This deliberately spends additional precision on
the division/product budget. It uses two NEON or four AVX2 lanes, while the
other kernels use four/eight Float32 lanes. The `n=0` branch returns the central
polynomial directly, including sincpi(±0)=1; nonzero integers explicitly return
positive zero. Odd/even integer parity supplies the sign bit. No approximate
reciprocal or rounded large `pi*x` reduction is used.

Nonfinite inputs are classified by bits and retain the existing payload,
classification and signed-zero rules. Rejected lanes in a batch are replaced by
safe zero operands before SIMD and evaluated by the existing scalar semantic
layer. Finite sincpi needs no strict refinement after admission. Other functions
retain their existing full-domain fallback beyond the listed fast intervals.

## Analytic certificate

Run `python3 oracle/ops/numeric/trig_bound.py`. It reads the actual C++
hexadecimal coefficients and uses exact Python Fractions, not sampled libm
errors. Pi is enclosed using Machin's identity and alternating atan series.
The certificate bounds coefficient quantization, the square, every Horner FMA,
the Taylor remainder, any final multiply/divide, and strict-reference rounding.
It checks final range invariants as well as the accuracy budget.

| Function | Analytic upper bound on ordered steps |
| --- | ---: |
| sin | <2.981927 |
| cos | <1.597639 |
| sinpi | <3.417932 |
| cospi | <1.596624 |
| sinc | <1.318272 |
| sincpi, full finite Float32 | <2.000002 |

Sinc/cos outputs on the small domains stay above 0.5, making an absolute spacing
bound valid. Sine instead uses a relative error and the minimum intervening
representable spacing, including binade crossings. Its tiny-input exclusion
ensures normal outputs. For sincpi, the residual is itself representable in
Float32, so its square is exact in binary64. Outside the central interval,
nonzero `abs(r/a)>2^-24`; hence every noninteger result stays far above the
Float32 subnormal range. Integer zeros are handled exactly. The observed zero
error in the sincpi corpus is not a proof of correct rounding for every input;
the maintained guarantee is the analytic bound above and the public four-step
contract.
