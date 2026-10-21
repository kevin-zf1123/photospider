---
spec_schema_version: 1
id: CRV-11F5
parent_id: CRV-11
function: lowpass_nonuniform_gaussian
proposed_operation_keys:
  - curve.lowpass_nonuniform_gaussian_strict
  - curve.lowpass_nonuniform_gaussian_accelerated_apple_silicon
  - curve.lowpass_nonuniform_gaussian_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-11F5: lowpass_nonuniform_gaussian

The dynamic inputs inherit the [family Result tensor-port contract](CRV-11_nonuniform_lowpass_contract.md): each port is a Result with exactly one tensor member  under any structurally valid schema id/version/member key. Shapes use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and mathematical definition

Inherit the complete [nonuniform contract](CRV-11_nonuniform_lowpass_contract.md).
Ordered dynamic ports are `positions` and `values` Results. `positions` has full `sample_shape()` [K], rank 1, finite strictly increasing Float32/64 values, K=2..1048576. `values` has full `sample_shape()` rank 1..8, positive shape and logical count <=2^40; static axis selects extent K. The output `samples` is a `photospider.tensor` v1 Result member preserving values shape/dtype with no attached domain description, evaluated at the original positions.

Required static parameters are axis, support_radius and sigma; boundary is
reflect/replicate/zero/wrap, default reflect. All applicable numeric kernel
parameters are finite; support_radius>0 uses position units.
sigma>0 uses position units and support_radius truncates the Gaussian.

The continuous gaussian formula is the corresponding shared kernel. Integrate
it against the exact piecewise-linear extended signal and divide by the full
continuous kernel integral. No uniform-grid approximation or edge renormalization
defines this operator.

## Numerical and execution obligations

Strict correctly rounds the complete integral quotient. Accelerated permits
<=4 ULP final error, exact constant/zero/sign and finite classification, with
strict fallback. Finite strictly increasing positions, finite contributing endpoint values and finite representable selected outputs are caller preconditions. Precision/capacity limits fail explicitly; cancellation and resource failures return before numerical retries.

All three keys use the [fixed execution and resource contract](CRV-11_nonuniform_lowpass_contract.md#algorithm-resources-and-mapping). Each selected output tile has one CPU task reading complete positions and the full filtered values axis for its other-axis rectangle, closed on source grids. The kernel searches source views directly and streams positive-length pieces, retaining two pieces and fixed polynomial/arithmetic scratch. Only selected outputs are computed and allocated. Empty emits no task; successful calls publish after all writes succeed and retire. Source affine views and escaped output ownership follow the shared contract.

## Acceptance and status

Use the shared affine fixture positions=[0,0.75,2], values=[1,2.5,5] and
support_radius=0.5: at the middle position, samples is exactly 2.5 for any
admitted sigma parameters. Verify collinear-knot insertion invariance against
independent certified continuous integration. Also test constant/impulse hats,
unequal spacing, boundaries/seams, extreme parameters, dtype mixing, 4-ULP
bounds, local negative-zero support, static source closures, budgets/cancellation,
strides and source/result lifetime.

Temporary public acceptance binds positions/values and statics, then requests samples through Compiler/ExecutionContext and compares them with the independent continuous oracle.

## Maintained implementation and validation

This primitive uses the nonuniform family's fixed CPU adapter. Exact streaming partition and paired-affine integration use global Taylor moments with a rigorous tail bound and preallocated coefficient storage. Accelerated keys currently use strict arithmetic.
Certified precision is bounded to 128..4096 bits and order <=512; unresolved
capacity or rounding may return `ResourceExhausted`. See [CRV-11 umbrella](CRV-11_resample_signal.md).
