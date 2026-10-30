---
spec_schema_version: 1
id: NUM-15B
parent_id: NUM-15
function: integrate_1d
proposed_operation_keys:
  - numeric.integrate_1d_strict
  - numeric.integrate_1d_accelerated_apple_silicon
  - numeric.integrate_1d_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-15B: integrate_1d

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md); explicit rules below take precedence.

## Interface

Computes cumulative trapezoidal integration from three `Result` tensor inputs in port order: samples with `sample_shape()` [N], step [1], and initial [1]. Each input may use any tensor member key; all share Float32 or Float64 dtype. Requires 1<=N<=2^40. The output port is `values`; its Result schema is `photospider.tensor` with tensor member `samples`, shape [N], input dtype, and no attached domain description. There are no static parameters or implicit casts/axis extraction. Static schema incompatibility returns TypeMismatch.

## Mathematics

```text
values[0] = initial
values[i] = initial + step * sum_{k=0..i-1}(samples[k]+samples[k+1])/2
```

Output zero copies initial raw bits, including sNaN, Inf and signed zero. Positive outputs require a finite nonzero step as an unchecked caller numerical precondition; a negative step is valid.

At i>0, the entire initial-plus-area expression is evaluated mathematically and rounded only once to the output dtype. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound. For finite sources an equivalent exact expression is

```text
initial + step/2 * (samples[0] + 2*sum(samples[1:i]) + samples[i])
```

Trapezoids, prefixes and the area are not rounded before adding initial. Unrequested intermediate output overflows do not fail a later representable requested value. Exact zero at i>0 is +0; nonzero exact underflow keeps its sign. Final overflow yields correctly signed infinity as a successful numeric result.

For i>0, source NaN priority is initial first, then samples by increasing index within the contributing prefix; the NaN is quieted with payload and sign preserved. With no NaN, opposite signed sample infinities make the trapezoidal area undefined and yield the fixed positive quiet NaN. Otherwise the sample infinity sign is multiplied by step's sign, then combined with initial: opposite infinities yield the canonical NaN, and a single infinity sign determines the result. Finite weighted sums and products stay exact until final output classification. Output 0 keeps its raw-copy contract; a generated prior-output NaN is never treated as a new source operand. Fixed NaN patterns follow [NUM-04](NUM-04_unary_contract.md); output 0 is the explicit raw-copy exception. Source and initial support all IEEE patterns.

## Behavior

values[0] depends only on initial; the samples and step payloads (and their producers) are not required for a zero-only selection, though their metadata is checked. values[i] for i>0 depends on samples[0..i], step and initial. Empty selection reads nothing.

## Acceptance

Samples `[0,1,2]`, step `[1]`, initial `[0]` gives `[0,0.5,2]`. Constant samples `[2,2,2]`, step `[0.5]`, initial `[3]` gives `[3,4,5]`. Independent weighted-prefix Fraction arithmetic and IEEE tables: negative and subnormal step, raw zero-output NaNs, signed zero, infinity cancellation, NaN priority, and representable outputs following large intermediate areas. Failing samples/step producers connected to a zero-only selection (N=1, or larger N with output extent one) do not run.
