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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-11F5: lowpass_nonuniform_gaussian

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and mathematical definition

Inherit the complete [nonuniform contract](CRV-11_nonuniform_lowpass_contract.md).
Ordered dynamic ports are positions[K] and values, independently Float32/Float64.
positions is finite strictly increasing, K=2..1048576; static axis selects the
values extent K. Other axes are independent, values rank 1..8, positive shape
and logical count <=2^40. Named samples retains values shape/dtype with generic
facets and is evaluated at the original positions.

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
strict fallback. All values and outputs must be finite; this
is not the uniform family's IEEE nonfinite aggregation. A precision/work/capacity
limit fails explicitly instead of publishing unconverged integration.

All three formal keys use Whole. Nonempty requests collect complete positions
and values, validate positions globally, and compute every output. Each numerical
integral still uses only positive-length pieces and their exact endpoint values;
coefficient cancellation cannot remove validation. Any input edit invalidates all
outputs. Nonfinite samples or overflow anywhere, including undelivered columns,
fail Domain/Run. Empty reads nothing. Output storage is the complete dense shape;
only one output's managed piece vector is retained at a time. Legal strides,
immutable owners, cache-off, work/capacity/cancellation follow the shared contract.

Use the shared compile/preflight/runtime error categories for malformed parameters,
type mismatch, invalid positions, nonfinite demanded samples, output overflow,
resources/backends/cancellation and upstream failures. No failed sample publishes
partially computed data.

## Acceptance and status

Use the shared affine fixture positions=[0,0.75,2], values=[1,2.5,5] and
support_radius=0.5: at the middle position, samples is exactly 2.5 for any
admitted sigma parameters. Verify collinear-knot insertion invariance against
independent certified continuous integration. Also test constant/impulse hats,
unequal spacing, boundaries/seams, extreme parameters, dtype mixing, 4-ULP
bounds, invalid remote data failures, complete dirty support, budgets/cancellation,
strides and source/result lifetime.

The maintained public workflow binds positions/values and statics, then requests
samples through Compiler/ExecutionContext. See the shared workflow link below for
commands and current evidence; the spec does not claim universal antialias rejection.

## Maintained implementation and validation

This primitive is registered in the five-kernel nonuniform low-pass family. Exact partition and paired-affine integration use global Taylor moments with a rigorous tail bound; this is not local adaptive quadrature, and accelerated keys currently use the strict fallback.
Certified precision is bounded to 128..4096 bits and order <=512; unresolved
capacity or rounding may return `ResourceExhausted`. See the [shared workflow](../../../../examples/numeric_workflow/README.md#nonuniform-lowpass)
and [CRV-11 umbrella](CRV-11_resample_signal.md). Native Clang21 Strict/Apple validation for the Whole revision is recorded in
that workflow and the math implementation notes. WSL/AVX2 and installed-package
consumers have not been rerun. Whole numerical/fallback counters are N/A.
