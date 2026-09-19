---
spec_schema_version: 1
id: CRV-11F2
parent_id: CRV-11
function: lowpass_nonuniform_hamming_sinc
proposed_operation_keys:
  - curve.lowpass_nonuniform_hamming_sinc_strict
  - curve.lowpass_nonuniform_hamming_sinc_accelerated_apple_silicon
  - curve.lowpass_nonuniform_hamming_sinc_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-11F2: lowpass_nonuniform_hamming_sinc

## Interface and mathematical definition

Inherit the complete [nonuniform contract](CRV-11_nonuniform_lowpass_contract.md).
Ordered dynamic ports are positions[K] and values, independently Float32/Float64.
positions is finite strictly increasing, K=2..1048576; static axis selects the
values extent K. Other axes are independent, values rank 1..8, positive shape
and logical count <=2^40. Named samples retains values shape/dtype with generic
facets and is evaluated at the original positions.

Required static parameters are axis, support_radius and cutoff; boundary is
reflect/replicate/zero/wrap, default reflect. All applicable numeric kernel
parameters are finite; support_radius>0 uses position units.
cutoff>0 uses cycles/position-unit, with no universal 0.5 Nyquist limit.

The continuous hamming_sinc formula is the corresponding shared kernel. Integrate
it against the exact piecewise-linear extended signal and divide by the full
continuous kernel integral. No uniform-grid approximation or edge renormalization
defines this operator.

## Numerical and execution obligations

Strict correctly rounds the complete integral quotient. Accelerated permits
<=4 ULP final error, exact constant/zero/sign and finite classification, with
reported strict fallback. All demanded values and outputs must be finite; this
is not the uniform family's IEEE nonfinite aggregation. A precision/work/capacity
limit fails explicitly instead of publishing unconverged integration.

Global positions validation and exact mapped positive-length interval support
determine reads. Only required reconstruction endpoints and requested other-axis
coordinates are read; coefficient cancellation does not remove endpoint validation.
Inherit boundary folds/seams, exact dirty maps, typed/upstream closure, immutable
packed mapping, arbitrary strides, owners, cache-off and host accounting from
the shared contract. Integration/refinement and long repeated-boundary support
are fully budgeted and cancellation-aware.

Use the shared compile/preflight/runtime error categories for malformed parameters,
type mismatch, invalid positions, nonfinite demanded samples, output overflow,
resources/backends/cancellation and upstream failures. No failed sample publishes
partially computed data.

## Acceptance and status

Use the shared affine fixture positions=[0,0.75,2], values=[1,2.5,5] and
support_radius=0.5: at the middle position, samples is exactly 2.5 for any
admitted cutoff parameters. Verify collinear-knot insertion invariance against
independent certified continuous integration. Also test constant/impulse hats,
unequal spacing, boundaries/seams, extreme parameters, dtype mixing, 4-ULP
bounds, invalid remote data nonreads, exact dirty support, budgets/cancellation,
strides and source/result lifetime.

The maintained public workflow binds positions/values and statics, then requests
samples through Compiler/ExecutionContext. See the shared workflow link below for
commands and current evidence; the spec does not claim universal antialias rejection.

## Maintained implementation and validation

This primitive is registered in the five-kernel nonuniform low-pass family. Exact partition and paired-affine integration use global Taylor moments with a rigorous tail bound; this is not local adaptive quadrature, and accelerated keys currently use the strict fallback.
Certified precision is bounded to 128..4096 bits and order <=512; unresolved
capacity or rounding may return `ResourceExhausted`. See the [shared workflow](../../../../examples/numeric_workflow/README.md#nonuniform-lowpass)
and [CRV-11 umbrella](CRV-11_resample_signal.md). Native Clang21 Strict/Apple and WSL Clang18 Strict/AVX2 passed the
shared public manual groups and independent numerical references. The linked
workflow records exact counts, commands and installed-consumer checks.
