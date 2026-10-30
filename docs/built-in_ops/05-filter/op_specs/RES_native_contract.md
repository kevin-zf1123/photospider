---
spec_schema_version: 1
id: RES-native
kind: shared_operator_contract
parent_id: 05-filter
implementation_status: partially_implemented
category: 05-filter
status: Proposed
numeric_reference: F
---

# RES01–12: Native floating-point arithmetic

This contract applies only to RES-01 through RES-12 and takes precedence over inherited NUM or FILTER exact-expression, baked64, strict/accelerated, and FP32-scaled four-ULP requirements. RES-13/14 and unrelated FIL/FRQ members keep their existing contracts. Native arithmetic does not remove structural, dependency, ownership, resource, cancellation, masking, or copy requirements.

## Working precision and stages

The main input dtype selects native IEEE Float32 or Float64 arithmetic, coefficients, intermediates, iterative state and reductions. Explicitly specified output types remain unchanged: RES-01A publishes one Float64 scale per plane. Real parameters are supplied as Float64 and converted once to the working dtype before arithmetic; callers guarantee representable parameters and the member's numerical/model domain. An implementation must not convert Float64 computation to Float32 or run every Float32 stage in a hidden Float64 state.

Formulas describe the mathematical algorithm. Native rounding occurs during ordinary operations; FMA, SIMD, stable algebraic rearrangement, and reduction regrouping are permitted. `RN_t` in an iterative formula means storing native state in dtype t, not exactly evaluating the whole expression followed by one rounding. Stage boundaries, synchronous iterations, transform normalization, spatial support, masks, rank ordering, tie rules and explicit comparison operators remain normative. An approximate quantile must not replace an exact rank of the computed native samples. A helper calling an existing non-RES operator preserves that operator's contract at its interface. Exact transpose requirements refer to the algebraic finite-domain operator and coefficient/index mapping, not bitwise equality of separately rounded bilinear evaluations.

No strict/accelerated quality keys, arbitrary-precision production fallback, exact-rational iterative state, or NUM FP32-scaled four-ULP bound are required. Numerical algorithms may use wider scalars for explicitly documented bookkeeping or a fixed Float64 output conversion; such use does not license silently replacing the working precision of the whole algorithm.

## Exceptional values and control

Native IEEE arithmetic determines rounding, overflow, underflow and exceptional results. Do not enable flush-to-zero or fast-math assumptions that invalidate a specified domain, comparison, or copy branch. NaN sign/payload and bitwise equality across architectures are not promised for arithmetic. Explicit copy/bypass branches preserve input bits and consume only their declared dependencies. Domain/model premises remain unchecked in production. Structural errors and explicit valid-input no-solution outcomes keep their specified status.

Order-statistic and finite comparison algorithms require finite participating native values and finite intermediates where their mathematical ordering requires it. Masked-out payloads do not acquire that premise and must not be consumed by arithmetic. Stopping checks involving a non-finite value are inconclusive; native residual/energy checks are numerical stopping criteria, not exact certificates of global optimality.

## Accuracy and acceptance

Validate each native dtype independently against a separately implemented staged native reference and a higher-precision mathematical reference. Record maximum absolute error, scale-aware relative error, conditioning/cancellation cases and algorithm invariants. Tolerances must be justified for the member, input scale, dimensions and number of stages; a performance result alone is not numerical acceptance. Rank indices, masks, selected outputs, support, exact copies and specified discrete ties are checked exactly against the computed native values. An implementation cannot loosen its threshold solely to pass a failing fixture.

RES-01A uses native residuals, native absolute deviations and exact median ranks of those computed samples. Independently verify both center ranks and MAD ranks against the actual native sample arrays produced by the permitted evaluation order. The 4*epsilon64 comparison (with an absolute subnormal floor) applies only to Float64(native_MAD)*scale64, using that same native MAD; it is not a Float64-relative bound on earlier fp32 stages. For the fixed four-tap estimator, use the conservative local acceptance envelope `32*epsilon_t*max_abs_input*scale64 + 32*denorm_min_t*scale64 + 4*epsilon64*abs(reference_sigma)` when comparing the ideal high-precision reference or two permitted native groupings, with finite participating intermediates and no FTZ. This allowance covers residual grouping, both midpoint roundings, deviation subtraction and final scaling; exact median/MAD selection is checked separately and cannot be replaced by a scalar-error check. It is an algorithm-specific acceptance tolerance, not a certified bound for arbitrary algebraic rewrites or a NUM final-output tier. Record actual absolute and scale-aware errors, including cancellation and large dynamic range. RES-01B validates native subtraction, clamp, read-noise square, fused multiply-add and square root independently, including output subsets. A scale-aware bound of 8*epsilon_t*max(abs(gain*max(mean-offset,0)), read_sigma^2, min_normal_t) applies to variance against the staged reference; sigma is checked against the square root of that native variance with 4*epsilon_t relative tolerance and a subnormal absolute floor. These local checks are not a NUM final-output guarantee for RES02–12.

Existing rational and directed-MPFR oracles remain mathematical references. Their certified exact-expression bits are not native execution goldens unless the fixture explicitly describes a native stage. Unimplemented members remain unimplemented; changing their arithmetic contract does not resolve missing profiles, oracle coverage or runtime acceptance.
