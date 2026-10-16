---
spec_schema_version: 1
id: CRV-10B
parent_id: CRV-10
function: invert_pchip
operation_family: curve.invert_pchip
proposed_operation_keys:
  - curve.invert_pchip_strict
  - curve.invert_pchip_accelerated_apple_silicon
  - curve.invert_pchip_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-10B: invert_pchip

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Invert the mathematical PCHIP curve defined by x/y, before
its forward output rounding. Do not invert a rounded lookup or construct a new
swapped-control interpolator. The [CRV-10 shared contract](CRV-10_invert.md) is
normative for every interface, error, numerical and execution requirement.

Ordered Result ports x[K],y[K],query[N] each contain one tensor member under any schema id/key and independently accept Float32/Float64; extents come from `sample_shape()`.
x is finite strictly increasing; y is finite strictly increasing or decreasing,
without plateaus. K=2..65536,N=1..2^40. Output port `values` is a Result with schema `photospider.tensor`, member `samples`, shape [N], no attached domain description and Float32/Float64 dtype selected by static `dtype`, default Float64. The other required static String
parameter out_of_domain is reject/clamp, default reject. No extrapolated tail
is inverted. Endpoints correctly convert corresponding x, preserving zero sign.

Use exactly the CRV-01B rational PCHIP slopes/polynomial, bracket the unique root and correctly round final x in strict. Accelerated permits <=4 ULP x error, exact classification/zero/endpoint rules, segment bounds and monotonicity; strict fallback is used when needed. Residual alone is not the quality bound.
Other mathematical exact zeros are +0 and nonzero underflow retains sign.
Demanded queries and outputs are finite, with narrowing overflow failure.

All three formal profile keys use complete-input. Every nonempty request collects all x/y/query Results with ordinary payload tile Needs, validates global topology and all query controls, then computes a complete dense Result. Any input edit invalidates the complete output; dynamic numerical
failures are Domain/Run, including invalid or overflowing undelivered positions.
Empty requests read no payload. Inherit CRV-10's typed/upstream closure, arbitrary strides, source associations, output ownership, cache-off, complete input/output storage, work and cancellation rules. Lookup work is O(K+N log K) plus exact arithmetic. Numerical stencils and
rounding rules remain unchanged; unused endpoint conversion cannot reject a
finite root. No partial output is published on failure. complete-input fallback counters
are N/A; the actual strict fallback remains available.

Conceptual fixture: x=[0,1,2],y=[0,1,4],query=[0.3125,2.1875] -> values=[0.5,1.5].
Negating y and query retains the result. Apply CRV-10's independent exact oracle,
boundary/rounding/monotonicity, dirty/partial-read, stride, budget, cancellation
and owner-lifetime acceptance. A temporary public Compiler/ExecutionContext
workflow should supply x/y/query and static dtype/policy. Create this check for the current change and remove it after validation.

## Maintained implementation and validation



`plugins/ops/01-numeric/curve_inverse.cpp` registers the six complete-input Result keys; the public constructors are `invert_linear_node` and `invert_pchip_node` in [`inverse_curves.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/inverse_curves.hpp). The current Result implementation retains the exact inverse algorithm and resource accounting described above.

For temporary validation, bind immutable source Results and read outputs through
authorized windows. Cover caller/worker floating environments, mixed dtypes,
negative/zero strides, dirty scope, knot/clamp signed zero, preparation reuse,
repeated-demand identity and current source associations. Include Empty without
polling a failing producer, SampledSignal errors attributed to input 2,
cancellation and source-owner retirement. A three-element Float32/Float64 output
and its windows retain 12/24 Payload bytes after source retirement; releasing
the last window releases that payload. A constant View with N=2^40 must fail
`ResourceExhausted/CapacityLimit` when complete complete-input output storage exceeds the
configured Payload cap. This case checks admission, not numerical execution at
that logical size.
