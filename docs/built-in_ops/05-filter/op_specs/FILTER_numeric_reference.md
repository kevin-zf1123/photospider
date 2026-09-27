---
spec_schema_version: 1
id: FILTER-numeric-reference
kind: shared_operator_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
---

# FILTER numerical reference

## Exact expressions, baked constants and staged states

RN_t(x) rounds an exact finite real directly to Float32 or Float64, nearest,
ties-to-even, with gradual underflow. Finite stored floating values are exact binary
rationals. Unmarked arithmetic denotes mathematical evaluation, not host instructions.

* **E:** evaluate the complete specified expression, then round once at its output.
  Convolution is RN_t(sum(k_i*x_i)/sum(k_i)+bias), without rounded products or partial
  sums. Strict separable convolution is the full double sum, not two dtype writebacks.
* **B:** explicitly generated RN64 coefficients are observable algorithm constants.
  Gaussian g[j]=RN64(exp(-j²/(2*sigma²))) uses symmetric absolute indices. sigma=0
  requires radius=0 and coefficient 1; positive sigma permits radius=0. A separately
  stored 2D outer product adds rounding and is not the same filter.
* **S:** public intermediates, pyramid levels and declared iterative stages round at
  their specified boundaries and become exact inputs to the next stage. TV u/p/ubar,
  diffusion and RL images, and blind image/gradient stages use the main image dtype
  t. Blind PSF simplex state is exact rational. Explicit baked64 coefficients retain
  RN64. Iteration order/ROI/thread count cannot move these rounding boundaries.

Finite E intermediates cannot fail or become infinity merely because host arithmetic
overflows. A finite exact value rounded beyond the destination range produces signed
infinity under NUM. That infinity is an observable input to the next S stage.
Convex-combination bounds do not justify clipping signed kernels or restoration.

## Exceptional values and operand order

Inherit [NUM binary](../../01-numeric/op_specs/NUM-05_binary_contract.md),
[NUM reductions](../../01-numeric/op_specs/NUM-11_reduction_contract.md),
[NUM quantile](../../01-numeric/op_specs/NUM-12B_quantile.md) and the corresponding
NUM elementary function for each constituent operation. Evaluate special-value
classification before finite exact arithmetic. Arithmetic NaNs are quieted with
sign/payload conversion specified by NUM; a newly generated NaN is the fixed positive
quiet NaN. Binary operand priority follows written left-to-right operand order.
A sum/reduction visits its contributing terms in logical row-major index order;
within a term use written operand order. The first input NaN to that reduction
wins over a newly generated invalid-arithmetic NaN. A member defining another
logical order must say so. Repeated boundary taps retain multiplicity and tap order.
Do not skip required reads or validation after observing a NaN.

Zero kernel taps explicitly excluded by a member have no source arithmetic. Other
zeros are operands: 0*Inf generates NaN; Inf-Inf of equal sign and Inf/Inf generate
NaN. Opposite signed infinities in a sum generate NaN. Finite nonzero divided by
signed zero yields signed infinity; 0/0 generates NaN unless a specified model
branch defines that case. NaN is not silently converted to zero by min/max, clamping,
projection or a false host comparison. For a numeric result whose controlling
comparison is NaN, propagate the determining NaN through that result; validity
masks are false. A structural output requiring an integer selection cannot encode
NaN: the member must specify its selection/error rule before registration.

Ranking/quantiles inspect the entire selected sample sequence for NaNs, propagate
its first NaN in original order, and retain stable signed-zero ties. Quantile
interpolation of same-sign infinities or infinity and finite yields that infinity;
opposite infinities yield positive quiet NaN. Pure selection/copy preserves bits.
For sums use NUM reduction zero rules; for binary operations use NUM signed-zero
rules. Finite exact cancellation yields +0 except explicitly inherited all-negative
zero/selection cases. Nonzero underflow preserves its mathematical sign.

Explicit parameter/control domains remain enforced (for example finite q in [0,1],
coverage, positive finite spacing). Finite-only exceptions require an actual model
or corresponding NUM rule, not a category-wide convenience check.

## Accelerated acceptance

Apply [NUM acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md) to
final outputs. Float32 uses at most four ordered finite IEEE steps. For Float64
reference r in the normal finite Float32 range:

```
abs(candidate-r) <= 4 * 2^(floor(log2(abs(r)))-23)
```

Zero, values below 2^-126, values above 0x1.fffffep127, special values and uncertain
classification require strict handling. Four ULP per stage is not a final-output
proof. Selection, ties, support, stopping predicates and metadata remain exact.
Asynchronous scheduling changes only which valid round is returned, not predicate
arithmetic. Approximate local Laplacian algorithms require separate profiles.

## Oracle evidence

ExactRational means Fraction/int evaluation with direct IEEE rounding; avoid a
binary64 intermediate when rounding to binary32. DirectedMPFR means directed
interval evaluation whose two endpoints round to the same output bits; unresolved
branches, denominators or rounding cells are Inconclusive. Matching two precisions
is not a certificate. HighPrecisionDiagnostic includes numerical matrix/reference
comparisons and the uncertified Anscombe mean inverses. PinnedThirdPartyComparison
requires actual version/configuration/resource/input records and explained numerical
differences; it does not establish unconditional bit identity or NUM compliance.
ContractFixture checks logical schema/control properties without runtime ownership
claims. See [oracle](../../../../oracle/ops/filter/README.md) for implemented subsets.
