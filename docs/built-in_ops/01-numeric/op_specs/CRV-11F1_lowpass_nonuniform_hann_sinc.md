---
spec_schema_version: 1
id: CRV-11F1
parent_id: CRV-11
function: lowpass_nonuniform_hann_sinc
proposed_operation_keys:
  - curve.lowpass_nonuniform_hann_sinc_strict
  - curve.lowpass_nonuniform_hann_sinc_accelerated_apple_silicon
  - curve.lowpass_nonuniform_hann_sinc_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
verification_status: focused_result_validation_passed
repository_commit: current working tree
---

# CRV-11F1: lowpass_nonuniform_hann_sinc

The dynamic inputs inherit the [family Result tensor-port contract](CRV-11_nonuniform_lowpass_contract.md): each port is a Result with exactly one tensor member and no fields under any structurally valid schema id/version/member key. Shapes use complete `sample_shape()` values, including batch axes.



Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and mathematical definition

Inherit the complete [nonuniform contract](CRV-11_nonuniform_lowpass_contract.md).
Ordered dynamic ports are `positions` and `values` Results. `positions` has full `sample_shape()` [K], rank 1, finite strictly increasing Float32/64 values, K=2..1048576. `values` has full `sample_shape()` rank 1..8, positive shape and logical count <=2^40; static axis selects extent K. The output `samples` is a `photospider.tensor` v1 Result member preserving values shape/dtype with generic facets, evaluated at the original positions.

Required static parameters are axis, support_radius and cutoff; boundary is
reflect/replicate/zero/wrap, default reflect. All applicable numeric kernel
parameters are finite; support_radius>0 uses position units.
cutoff>0 uses cycles/position-unit, with no universal 0.5 Nyquist limit.

The continuous hann_sinc formula is the corresponding shared kernel. Integrate
it against the exact piecewise-linear extended signal and divide by the full
continuous kernel integral. No uniform-grid approximation or edge renormalization
defines this operator.

## Numerical and execution obligations

Strict correctly rounds the complete integral quotient. Accelerated permits
<=4 ULP final error, exact constant/zero/sign and finite classification, with
strict fallback. All values and outputs must be finite; this
is not the uniform family's IEEE nonfinite aggregation. A precision/work/capacity
limit fails explicitly instead of publishing unconverged integration.

All three formal keys use Whole Result programs. Nonempty requests declare Data,
Validation and Descriptor needs (role 13) for both complete input members. Typed
and upstream validation covers all inputs. The callback reads positions through authorized windows into Root-owned
promoted coordinate storage, then reads source values through authorized
windows without collecting or copying the complete values tensor. It computes every output using the
same positive-length pieces and exact endpoint values; coefficient cancellation
does not remove endpoint validation. Input changes invalidate complete recorded
output demand. Nonfinite samples or overflow, including outside delivery, fail
Domain/Run. Empty reads no payload after static preflight. The Result writer
publishes the complete dense output transactionally with full coverage and global
coordinates. Root accounts the promoted positions vector, one reused piece
vector, full output and workspaces. Legal strides and owner lifetime follow the
shared contract; failure/cancellation publishes no partial output and releases
temporary state.

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
bounds, invalid remote data failures, complete dirty support, budgets/cancellation,
strides and source/result lifetime.

The maintained public workflow binds positions/values and statics, then requests
samples through Compiler/ExecutionContext. See the shared workflow link below for
commands and current evidence; the spec does not claim universal antialias rejection.

## Maintained implementation and validation

This primitive is registered in the five-kernel nonuniform low-pass family. Exact partition and paired-affine integration use global Taylor moments with a rigorous tail bound; this is not local adaptive quadrature, and accelerated keys currently use the strict fallback.
Certified precision is bounded to 128..4096 bits and order <=512; unresolved
capacity or rounding may return `ResourceExhausted`. See the [shared workflow](../../../../examples/numeric_workflow/README.md#nonuniform-lowpass)
and [CRV-11 umbrella](CRV-11_resample_signal.md). The focused Result CTest, Strict/Apple manual workflows, independent Result oracles and installed consumer pass. Historical performance measurements are from the earlier Value implementation; no Result performance or x86 numerical execution was run. See the shared workflow for exact coverage and commands.
