---
spec_schema_version: 1
id: CRV-11F4
parent_id: CRV-11
function: lowpass_nonuniform_kaiser_sinc
proposed_operation_keys:
  - curve.lowpass_nonuniform_kaiser_sinc_strict
  - curve.lowpass_nonuniform_kaiser_sinc_accelerated_apple_silicon
  - curve.lowpass_nonuniform_kaiser_sinc_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-11F4: lowpass_nonuniform_kaiser_sinc

## Interface and mathematical definition

Inherit the complete [nonuniform contract](CRV-11_nonuniform_lowpass_contract.md).
Ordered dynamic ports are positions[K] and values, independently Float32/Float64.
positions is finite strictly increasing, K=2..1048576; static axis selects the
values extent K. Other axes are independent, values rank 1..8, positive shape
and logical count <=2^40. Named samples retains values shape/dtype with generic
facets and is evaluated at the original positions.

Required static parameters are axis, support_radius and cutoff and beta; boundary is
reflect/replicate/zero/wrap, default reflect. All applicable numeric kernel
parameters are finite; support_radius>0 uses position units.
cutoff>0 uses cycles/position-unit, with no universal 0.5 Nyquist limit.
Kaiser beta>=0 is dimensionless; beta=0 gives a rectangular window.
The continuous kaiser_sinc formula is the corresponding shared kernel. Integrate
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
admitted cutoff and beta parameters. Verify collinear-knot insertion invariance against
independent certified continuous integration. Also test constant/impulse hats,
unequal spacing, boundaries/seams, extreme parameters, dtype mixing, 4-ULP
bounds, invalid remote data nonreads, exact dirty support, budgets/cancellation,
strides and source/result lifetime.

A conceptual public workflow binds positions/values and statics, then requests
samples through Compiler/ExecutionContext. Actual invocation and independently
verified results remain implementation requirements. This spec makes no claim
of current runtime registration or universal antialias rejection.
