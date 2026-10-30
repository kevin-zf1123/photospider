---
spec_schema_version: 1
id: NUM-04-rational-pi
parent_id: NUM-04
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# NUM-04: exact rational pi-multiple inputs

Numeric profile: strict is the exact reference defined by each operation. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact.

Inherit the [NUM baseline](NUM_common_contract.md) for metadata, platform keys, floating environment and error scope. The two Int64 ports below replace the floating unary interface. This shared file registers no generic dispatcher.

The four independent operations sinpi_rational, cospi_rational, tanpi_rational and sincpi_rational take an exact multiplier p/q of pi. They do not replace the Float32/Float64 sinpi/cospi/tanpi/sincpi operations, and a rounded 1/3 is never interpreted as exact one third.

## Interface

The ordered Result inputs provide numerator and denominator, each a Result with exactly one tensor member in slot 0. Both tensors have Int64 dtype and identical sample shape with rank 1..8, positive extents and at most 2^40 logical elements. The output is a Result on port `values` with the same shape and a static Float32 or Float64 dtype, schema `photospider.tensor`, tensor key `samples`, and no attached domain description. Constructors default the `dtype` parameter to Float64; direct nodes supply it explicitly. There is no automatic broadcast, integer/float mixing or alternate denominator-sign mode. Both input Results are dependencies even when the numerator is zero.

The denominator is positive at every computed position as an unchecked caller precondition; unreduced fractions are valid. Malformed static parameters use `InvalidArgument`; wrong dtype or shape uses `TypeMismatch`. Each operation has strict and independently named Apple Silicon/x86-64 CPU accelerated versions.

## Mathematics

For p = numerator and q = denominator > 0, the exact real multiplier is r = p/q. Equivalent fractions give the same result within a profile. Canonical evaluation reduces by the exact gcd, including for p = INT64_MIN. The original numerator sign determines the signs of integer zeros. Rounding p/q to float before evaluation is not valid.

Strict evaluates the stated function of the exact angle pi*r and rounds once directly to the output dtype. Sincpi means the whole quotient sin(pi*r)/(pi*r), not a rounded sine followed by division, and keeps the full ratio in its denominator. Accelerated allows at most four final FP32-scaled representable steps, with the corresponding floating pi-function's exact classification, zero and pole rules. Sine and cosine outputs stay in [-1,1] in every profile.

Exact landmarks, recognized with integer arithmetic:

- Integer r: sin and tan give zero with sign(p), with p = 0 giving +0; cos gives the parity-dependent value +1 or -1.
- Half-integer r: sin is exactly +1 or -1, cos is +0, and tan is the canonical positive quiet NaN `0x7fc00000` (Float32) or `0x7ff8000000000000` (Float64), returned as a successful result.
- Quarter-integer r: sine and cosine are the correctly rounded signed sqrt(1/2); tan is +1 or -1.
- Reduced denominators 1, 2, 3, 4 and 6 for sinpi_rational, cospi_rational and tanpi_rational use their exact algebraic values, correctly rounded directly to the output dtype, in every profile. Other ratios use the accelerated bound.
- Sincpi at p = 0 is +1 and at every nonzero integer ratio is +0; these are its only exact landmarks.

Examples: p = 2^53+1, q = 1 gives cos(pi*r) = -1 even though converting p to Float64 loses parity; p = 2^53+1, q = 2 is a tan pole even when floating p/q rounds to an integer; p = INT64_MIN, q = INT64_MAX keeps the tiny offset from -1 in the sine argument.

## Behavior

Each output sample depends on the numerator and denominator samples at the same coordinate. Exact poles are successful numeric results. Upstream, resource and cancellation failures keep their operational categories.

## Acceptance

Cases: unreduced equivalent fractions, negative numerators, zero, INT64_MIN/MAX, q = 1 and q = INT64_MAX, all common-angle denominators and their near neighbors, and exact tan poles. Common angles compare bits across every profile; other accelerated results compare final ULP and exact classification separately. The reference uses exact rational reduction and high-precision function or algebraic root evaluation.

## Individual specifications

- [NUM-04S sinpi_rational](NUM-04S_sinpi_rational.md).
- [NUM-04T cospi_rational](NUM-04T_cospi_rational.md).
- [NUM-04U tanpi_rational](NUM-04U_tanpi_rational.md).
- [NUM-04V sincpi_rational](NUM-04V_sincpi_rational.md).
- [Floating unary contracts](NUM-04_unary_contract.md).
- [Trigonometric contracts](NUM-04_trigonometric_contract.md).
- [CIELCh exact-angle dependency](CRV-06D_color_ramp_cielch.md).
