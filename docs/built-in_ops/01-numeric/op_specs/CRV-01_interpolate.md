---
spec_schema_version: 1
id: CRV-01
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-01: interpolation family

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

This family records the completed clarification of four independent interfaces.
Each selected operator has a separate primitive specification, rather than a
generic mode-dispatch operation. Current registration and implementation facts
are recorded below; this family document does not change the Proposed status.

## Confirmed purpose and split

Interpolate supplied known points to evaluate one-dimensional functions at
explicit dynamic query positions. The curve passes through its known points;
this input model differs from CRV-02's Bezier anchors and off-curve handles.
Queries can be supplied by a numeric sequence generator or an arbitrary
position array rather than being generated internally from a static domain.

The maintainer selected separate single-function and multiple-function
implementations/specifications:

- Single-function interface: x[K], y[K], query[N] -> values[N].
- Multiple-function interface: x[K], y[K,C], query[N] -> values[N,C],
  with multiple functions sharing the knot/query x coordinates. The independent
  multi-function specifications define column-wise mathematical rules and product limits.

Do not overload one operation with both ranks or silently squeeze/insert the
function axis. Linear and PCHIP interpolation also use independent operation
names, with their separate multiple-function counterparts. There is no static
method switch.

The single-function linear contract is
[CRV-01A](CRV-01A_interpolate_linear.md), whose clarification is complete.
The single-function PCHIP contract is
[CRV-01B](CRV-01B_interpolate_pchip.md), whose clarification is complete.
The independent multi-function variants are
[CRV-01C linear](CRV-01C_interpolate_linear_multi.md) and
[CRV-01D PCHIP](CRV-01D_interpolate_pchip_multi.md), both fully clarified.
All four target specifications remain Proposed; their current implementation
status is recorded below. No provisional recommendation in the category index
becomes a confirmed rule
merely through this family record.

## Separate interpolation interface

The registry also contains `curve.sample_linear` and `curve.sample_monotone`,
which use a controls tensor and a generated static-domain grid. They remain
separate operation contracts. The twelve explicit-query keys in this family
accept x, y and query tensors and do not alias those controls-based operations.

## Related specifications

- [Curve category](../curves.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).
- [Existing implementation summary](../../../kernel-architecture/Basic-Operations.md).
- [CRV-02 sampled Bezier function](CRV-02_sample_bezier_function.md).

## Maintained implementation and validation

The four Proposed interfaces are implemented by twelve registered keys in
`plugins/ops/01-numeric/curve_interpolation.cpp` and public constructors in
`photospider/numeric/curves.hpp`. Each workflow input and output is a Result. An
input schema contains one tensor member under any schema id and member key;
`sample_shape()` supplies x[K], y[K] or y[K,C], and query[N]. Each input can use
Float32 or Float64 independently. The `values` output uses schema
`photospider.tensor`, member `samples`, empty facets, and shape [N] or [N,C]; the
multi-function column is an ordinary sample axis.

All four operations use Whole execution. For a nonempty request, the Result
continuation requests Data, Validation and Descriptor (role 13) for all three
inputs, then validates all x knots and query controls before ordinate arithmetic.
It evaluates every query and output column and publishes the complete dense
output, even when the request is sparse. Input `ResultTensorSpec.facets` carries
typed semantics such as Mask validation; recognized typed validation and required
upstream failures cover complete active inputs, including values outside the
arithmetic stencil. A sparse request records its requested dependency region Q;
the returned Result can still have full computed coverage and global sample
coordinates. Empty requests read no payload, while static schema and metadata
checks still apply.

Strict and Float64 output use exact rational evaluation of the whole formula,
followed by one RN-even conversion. The fixed arithmetic arena has 352 limbs
(22,528 bits) and 96 slots, with at most 49 live formula slots. Accelerated
Float32 evaluation propagates conservative intervals through the whole formula
and publishes only when both endpoints round to the same Float32 value. This
condition preserves monotonicity across queries and strict fallbacks. Knots and
selected clamps remain exact. Exact cross products recognize a collinear
complete local PCHIP stencil and use the equivalent linear formula; rounded slope
equality does not trigger this reduction.

The mathematical stencil remains local: an exact knot or clamp uses one y,
linear interpolation uses two endpoints, and PCHIP uses its fixed local stencil.
Generic y values outside every evaluated stencil receive no additional finite
scan, although typed validation and upstream execution still cover the complete
active input. Every query and output column is evaluated, so an invalid value in
an unrequested row or column can fail the run. `reject` also collects all active
inputs before classifying the domain.

Cache identity includes complete input versions, profile, metadata and
parameters. Replacing source Results updates the associations to their current
ObjectIds; a reusable static preparation does not retain old source owners.
Numerical failures have Run scope and publish no partial output. The operations do
not report independent per-column Atom success. Input offsets, positive, zero
and negative strides are supported. Published output storage and an authorized
read window retain their owner beyond context destruction.

Searching/classification costs O(K+N log K), followed by N scalar evaluations
(or N*C for multi), with classification shared across columns. The complete
dense output needs b*N (or b*N*C) bytes even for a one-cell request. Input
collection, retained owners, the fixed arithmetic workspace and the knot vector
also require admission. The vector has 8*K element bytes plus allocator and
metadata overhead; K<=65536 bounds the element storage at 524288 bytes. No full
slope table is required. Work and cancellation checks run during input reads,
binary search, exact arithmetic and before publication. Capacity or work
exhaustion returns ResourceExhausted and releases failed output and scratch
storage; resource limits do not weaken the arithmetic contract. Numeric failures
are OperationFailed/InvalidDomain or final-output ArithmeticOverflow, with Run
scope and the offending port/index where available. Other typed, upstream, stale
and cancellation failures preserve their failure categories.

The manual Result fixture, exact Fraction oracle and installed consumer are
defined in the [numeric workflow README](../../../../examples/numeric_workflow/README.md).
Their command lines and coverage are documented there. The bounded benchmark passed eight rows per profile with two
polls, N*C computed elements and `timing_scope=result_execute_fragments`; its
Root Payload peaks are listed in the workflow README. These bounded timing
checks do not establish a performance improvement.
