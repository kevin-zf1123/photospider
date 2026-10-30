---
spec_schema_version: 1
id: NUM-15A
parent_id: NUM-15
function: derivative_1d
proposed_operation_keys:
  - numeric.derivative_1d_strict
  - numeric.derivative_1d_accelerated_apple_silicon
  - numeric.derivative_1d_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-15A: derivative_1d

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md); explicit rules below take precedence.

## Interface

Differentiates one-dimensional samples using two `Result` tensor inputs: samples with `sample_shape()` [N] and step with `sample_shape()` [1]. Each input may use any tensor member key. Both use Float32 or both Float64. Requires 2<=N<=2^40 and a finite nonzero step (a negative step is allowed). The output port is `values`; its Result schema is `photospider.tensor` with tensor member `samples`, shape [N], input dtype, and no attached domain description. The starting coordinate does not affect this operation; callers preserve or connect sampling-axis information separately. There are no static parameters or implicit casts/axis extraction. Static shape/dtype mismatches return TypeMismatch.

## Mathematics

First-order one-sided endpoints and central interior differences:

```text
D[0]   = (samples[1]-samples[0]) / step
D[N-1] = (samples[N-1]-samples[N-2]) / step
D[i]   = (samples[i+1]-samples[i-1]) / (2*step), 0<i<N-1
```

For N=2 both results use the same one-sided quotient. There is no edge-order parameter or automatic higher-order stencil. For finite samples, the whole rational quotient is evaluated exactly and correctly rounded once to the output dtype; accelerated uses the shared FP32-scaled bound. Neither an overflowing naive subtraction nor a 2*step intermediate changes the mathematical result.

Exact zero results are +0. Nonzero exact results that underflow keep their sign; final overflow yields signed infinity successfully. Among required samples, the first NaN in ascending source index order propagates, quieted with payload and sign preserved. Without source NaNs, same-sign infinities in the subtraction yield the fixed positive quiet NaN; otherwise subtraction/division determine the infinity sign. Step is finite and nonzero as an unchecked caller precondition. Fixed NaN bits follow [NUM-04](NUM-04_unary_contract.md). All IEEE sample values are valid.

## Behavior

D[i] depends on samples[i-1] and samples[i+1] (one-sided at the ends) and on the step; an interior center sample does not enter its own stencil. Empty selection reads nothing.

## Acceptance

Samples `[0,1,4]`, step `[1]` gives `[1,2,3]`. Samples `[-MAX,0,MAX]`, step `[MAX]` gives three ones despite naive intermediate overflow. Independent Fraction references: negative and subnormal step, N=2, NaN in the stencil, NaN at an interior center (no effect), infinity arithmetic, signed zero, underflow and final overflow.
