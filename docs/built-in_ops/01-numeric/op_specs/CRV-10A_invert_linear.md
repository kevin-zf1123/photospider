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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
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

Ordered ports x[K],y[K],query[N] independently accept Float32/Float64.
x is finite strictly increasing; y is finite strictly increasing or decreasing,
without plateaus. K=2..65536,N=1..2^40. Output values[N] is generic
Float32/Float64, static dtype default Float64. The other required static String
parameter out_of_domain is reject/clamp, default reject. No extrapolated tail
is inverted. Endpoints correctly convert corresponding x, preserving zero sign.

Use the exact selected-segment linear inverse formula and correctly round once; accelerated uses the shared FP32-scaled bound.
Other mathematical exact zeros are +0 and nonzero underflow retains sign.
Demanded queries and outputs are finite, with narrowing overflow failure.

Every nonempty request validates all x/y and reads only query[Q]; empty requests
read no payload. Inherit global topology dirty support, local query invalidation,
typed/upstream closure, arbitrary strides, immutable packed mapping, ownership,
cache-off, host accounting and cancellation from CRV-10. Lookup work is
O(K+M log K) plus the shared exact arithmetic cost for M requested samples.
No cumulative coordinate updates or rounded intermediate quotients define the inverse.

Compile/preflight type/static errors, global topology errors, requested query
domain errors, final overflow, resource/backend/cancellation/stale failures use
the shared error categories and observation scopes. A failed sample has no
partial publication. Unrequested output overflow cannot cause a failure.

Conceptual fixture: x=[0,1,3],y=[0,2,4],query=[3,1,3] -> values=[2,0.5,2].
Negating y and query retains the result. Apply CRV-10's independent exact oracle,
boundary/rounding/monotonicity, dirty/partial-read, stride, budget, cancellation
and owner-lifetime acceptance. The maintained public Compiler/ExecutionContext
workflow supplies x/y/query and static dtype/policy. See the inverse-curves example linked below for commands and
current validation evidence.

## Maintained implementation and validation

The public helper is `invert_linear_node` from
[`inverse_curves.hpp`](../../../../include/photospider/numeric/inverse_curves.hpp).
The exact rational fallback is bitwise identical across profiles; accelerated
root brackets may return within the shared FP32-scaled bound.
See the [inverse-curves workflow](../../../../examples/numeric_workflow/README.md#inverse-curves)
for the public fixture, command and shared validation evidence. Native Clang21
Strict/Apple and WSL Clang18 Strict/AVX2 passed all four manual groups and 407
independent Fraction cases per profile. Installed0.16 consumers passed both
native profiles; WSL is used for numerical correctness only.
