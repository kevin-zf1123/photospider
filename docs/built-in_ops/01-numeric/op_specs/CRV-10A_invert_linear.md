---
spec_schema_version: 1
id: CRV-10A
parent_id: CRV-10
function: invert_linear
operation_family: curve.invert_linear
proposed_operation_keys:
  - curve.invert_linear_strict
  - curve.invert_linear_accelerated_apple_silicon
  - curve.invert_linear_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-10A: invert_linear

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Invert the mathematical piecewise linear curve defined by x/y, before
its forward output rounding. Do not invert a rounded lookup or construct a new
swapped-control interpolator. The [CRV-10 shared contract](CRV-10_invert.md) is
normative for every interface, error, numerical and execution requirement.

Ordered Result ports x[K],y[K],query[N] each contain one tensor member under any schema id/key and independently accept Float32/Float64; extents come from `sample_shape()`.
x is finite strictly increasing; y is finite strictly increasing or decreasing,
without plateaus. K=2..65536,N=1..2^40. Output port `values` is a Result with schema `photospider.tensor`, member `samples`, shape [N], no attached domain description and Float32/Float64 dtype selected by static `dtype`, default Float64. The other required static String
parameter out_of_domain is reject/clamp, default reject. No extrapolated tail
is inverted. Endpoints correctly convert corresponding x, preserving zero sign.

Use the exact selected-segment linear inverse formula and correctly round once; accelerated uses the shared FP32-scaled bound.
Other mathematical exact zeros are +0 and nonzero underflow retains sign.
Consumed queries and selected final outputs must be finite and representable as caller preconditions.

All three profile keys use the [family fixed execution contract](CRV-10_invert.md#fixed-execution-resources-and-caller-preconditions). One task per selected output tile reads complete x/y and the corresponding query interval through the source grids, then computes only its global output coordinates. Each invocation has private preallocated ExactCurve scratch. Finite globally monotone controls, valid consumed queries and representable outputs are unchecked caller preconditions. Static schemas and fixed invocation signatures remain checked.

Empty selection emits no task. Rebinding reuses immutable preparation, with independent output owners and no content-cache identity guarantee. Signed/zero input strides, resource admission, cancellation, exact arithmetic guards and ordered call publication follow the family. Unreturned endpoint conversion does not invalidate a finite root. Fixed invocations expose no per-value numerical/fallback counters.

Conceptual fixture: x=[0,1,3],y=[0,2,4],query=[3,1,3] -> values=[2,0.5,2].
Negating y and query retains the result. Apply CRV-10's independent exact oracle,
boundary/rounding/monotonicity, fixed-read/sparse-output, stride, budget, cancellation
and owner-lifetime acceptance. A temporary public Compiler/ExecutionContext
workflow should supply x/y/query and static dtype/policy. Create this check for the current change and remove it after validation.

## Maintained implementation and validation

`curve_inverse.cpp` registers the inverse keys through the shared fixed adapter in `plugins/ops/01-numeric/fixed_curve.hpp`. Public constructors are `invert_linear_node` and `invert_pchip_node` in [`inverse_curves.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/inverse_curves.hpp). The exact inverse arithmetic remains in `ExactCurve::inverse`.

Temporary public validation follows the family: independent exact references, supported profiles, mixed dtypes, sparse/empty selections, immutable affine bindings, fixed read/write/scratch signatures, concurrent Runs, cancellation, capacity admission and owner lifetime. Large logical outputs require only selected output tiles and fixed scratch; no full-output capacity rejection is mandated. Remove temporary runtime checkers after validation and preserve the oracle.
