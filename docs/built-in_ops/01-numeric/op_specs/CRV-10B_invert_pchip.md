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
repository_branch: ops-specs
repository_commit: current working tree
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
without plateaus. K=2..65536,N=1..2^40. Output port `values` is a Result with schema `photospider.tensor`, member `samples`, shape [N], empty facets and Float32/Float64 dtype selected by static `dtype`, default Float64. The other required static String
parameter out_of_domain is reject/clamp, default reject. No extrapolated tail
is inverted. Endpoints correctly convert corresponding x, preserving zero sign.

Use exactly the CRV-01B rational PCHIP slopes/polynomial, bracket the unique root and correctly round final x in strict. Accelerated permits <=4 ULP x error, exact classification/zero/endpoint rules, segment bounds and monotonicity; strict fallback is used when needed. Residual alone is not the quality bound.
Other mathematical exact zeros are +0 and nonzero underflow retains sign.
Demanded queries and outputs are finite, with narrowing overflow failure.

All three formal profile keys use Whole. Every nonempty request collects all x/y/query Results with Data, Validation and Descriptor (role 13), validates global topology and all query controls, then computes a complete dense Result. Any input edit invalidates the complete output; dynamic numerical
failures are Domain/Run, including invalid or overflowing undelivered positions.
Empty requests read no payload. Inherit CRV-10's typed/upstream closure, arbitrary strides, source associations, output ownership, cache-off, complete input/output storage, work and cancellation rules. Lookup work is O(K+N log K) plus exact arithmetic. Numerical stencils and
rounding rules remain unchanged; unused endpoint conversion cannot reject a
finite root. No partial output is published on failure. Whole fallback counters
are N/A; the actual strict fallback remains available.

Conceptual fixture: x=[0,1,2],y=[0,1,4],query=[0.3125,2.1875] -> values=[0.5,1.5].
Negating y and query retains the result. Apply CRV-10's independent exact oracle,
boundary/rounding/monotonicity, dirty/partial-read, stride, budget, cancellation
and owner-lifetime acceptance. The maintained public Compiler/ExecutionContext
workflow supplies x/y/query and static dtype/policy. See the inverse-curves example linked below for commands and
current validation evidence.

## Maintained implementation and validation

The dedicated `test_numeric_inverse_result` CTest passed 1/1 in 0.89 seconds (0.94 seconds for its focused selection). The separate `test_numeric_result_math` integration test passed with the baking test in a distinct selection, 2/2 in 5.22 seconds (4.83 seconds for the shared math executable). The installed package 0.32.0 consumer selection for baking, inverse and LUT3D passed 3/3 in 5.71 seconds; `installed_numeric_inverse_result` took 0.94 seconds. Its direct Apple run passed all five groups. The source/contract reviewer and worker verification found no blocker or required change. Historical package 0.18 Value-path timings are retained in the [math implementation notes](../math-implementation.md#crv-10-inverse-curves); they do not measure current Result performance.

`plugins/ops/01-numeric/curve_inverse.cpp` registers the six Whole Result keys; the public constructors are `invert_linear_node` and `invert_pchip_node` in [`inverse_curves.hpp`](../../../../include/photospider/numeric/inverse_curves.hpp). The current Result implementation retains the exact inverse algorithm and resource accounting described above.

The maintained manual fixture is `examples/numeric_workflow/inverse.cpp`; it keeps source arrays as Value backing, binds their immutable Results through the public workflow, returns Results and reads them through authorized windows. Its five behavior groups pass under Strict and Apple. The independent 407-case Fraction oracle passes under both profiles, with exact Strict expectations and the shared FP32-scaled acceptance bound for Apple. These are separate from `test_numeric_result_math`'s `inverse_workflows` and `inverse_boundaries` integration cases. The fixture covers both caller and actual computation-worker floating environments, mixed dtypes, negative/zero strides, dirty scope, knot/clamp signed zero, static preparation reuse, repeated-demand ObjectId retention, a true fresh-source cache hit with current direct associations, Empty propagation without polling a failed producer, `SampledSignal` type failure attributed to input 2, cancellation, and source-owner retirement. Retained Float32 and Float64 outputs/read windows account for 12 and 24 Payload bytes; releasing the final window returns Root live capacity to zero. K=65536 executes successfully. A public N=2^40 constant View is rejected as `ResourceExhausted/CapacityLimit` at node 1 because Whole execution must allocate the complete output; this is not a numerical run at that physical output size. The installed-consumer target is configured to compile and link the public workflow source against package 0.32.0. Historical package 0.18 Value-path timings are retained in the [math implementation notes](../math-implementation.md#crv-10-inverse-curves); they do not measure current Result performance.
