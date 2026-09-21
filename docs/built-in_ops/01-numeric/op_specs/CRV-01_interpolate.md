---
spec_schema_version: 1
id: CRV-01
kind: shared_operator_contract
category: 01-numeric
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

## Existing implementation facts

Source inspected at the front-matter commit:

- [curve.sample_linear](../../../../plugins/ops/01-numeric/curve_sample_linear.cpp)
  and [curve.sample_monotone](../../../../plugins/ops/01-numeric/curve_sample_monotone.cpp)
  each use one Float32/Float64 controls[K,2] input and Whole output execution.
- Static domain_min/domain_max and count generate an increasing uniform grid;
  count is 2..1048576 and out_of_domain is reject/clip. Output is a rank-1
  array with input dtype. These are not explicit-query input interfaces.
- The legacy `curve.sample_linear`/`curve.sample_monotone` implementation uses
  Whole output and static domains; it is separate from the maintained explicit-
  query CRV-01 keys. The current explicit-query implementation and its exact
  demand behavior are recorded in the maintained section below and in the
  public workflow README.

## Related specifications

- [Curve category](../curves.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Foundation execution contract](../../00-foundation/contracts.md).
- [Existing implementation summary](../../../kernel-architecture/Basic-Operations.md).
- [CRV-02 sampled Bezier function](CRV-02_sample_bezier_function.md).

## Maintained implementation and validation

The four Proposed interfaces are implemented by twelve registered keys in `plugins/ops/01-numeric/curve_interpolation.cpp` and the public
constructors in `photospider/numeric/curves.hpp`. Strict and Float64 outputs use
exact rational whole-formula evaluation with 352 limbs (22,528 bits) and a
96-slot callback workspace; the maximum live formula slots are bounded at 49.
Accelerated Float32 evaluation propagates conservative intervals through the
complete formula and publishes only when both endpoints round to the same
Float32 value. This stronger condition preserves monotonicity across queries
and strict fallbacks. Named knots/clamps remain exact. Exact cross products
recognize a collinear complete local PCHIP stencil and use the equivalent linear
formula. Rounded slope equality is not used. NEON/AVX2 integer comparison and
publication helpers remain available; the Whole callback does not expose DependencySession fallback counters.

For every nonempty request, one CPU Whole callback collects complete x, y and
query inputs, including recognized typed validation and upstream failures. It
validates all x knots and all query controls before y arithmetic, evaluates every query and every output column, and returns
one immutable dense output of shape [N] or [N,C]. Empty reads no payload; static
metadata validation still applies. Sparse demand restricts publication coverage,
but does not reduce input collection, computation or the complete output owner.

The mathematical stencil remains unchanged: an exact knot/clamp uses one y;
linear uses two endpoints; PCHIP uses its fixed local stencil. Generic y values
outside every evaluated stencil do not undergo an additional finite scan. Typed
validation and upstream execution cover complete inputs, including unused values.
All query rows and output columns are evaluated, so errors in unrequested rows
or columns can fail the run. Reject still performs full upstream collection.

Any input change invalidates the recorded output demand. Cache identity includes
complete input versions, profile, metadata and parameters. Numerical failures
have Run scope and publish no partial successful output; they do not provide
independent per-column Atom success. Input strides, offsets, zero strides and
negative strides remain legal. Packed output storage outlives the context.

Search/classification costs O(K+N log K), followed by N scalar evaluations
(or N*C for multi). Classification is shared across columns. The complete dense
output costs b*N (or b*N*C) bytes; reserve it even for one requested cell. Input
collection and retained owners also require admission. The callback declares its
fixed exact arithmetic workspace, and the host-accounted knot vector has 8*K
element bytes plus allocator/metadata overhead. K<=65536 bounds its elements at
524288 bytes. No full slope table is required.

Poll work/cancellation during reads, binary search, exact arithmetic and before
publication. Capacity and work exhaustion return ResourceExhausted, with failed
output/workspace released. A giant logical broadcast can therefore fail a small
payload budget even for sparse demand. Resource limits do not authorize weaker
arithmetic. Backend, typed, upstream, stale and cancellation failures retain their
categories. Numeric failures are OperationFailed/InvalidDomain or final-output
ArithmeticOverflow, with Run scope and offending port/index where available.

See [the numeric workflow README](../../../../examples/numeric_workflow/README.md)
and [math implementation](../math-implementation.md) for public commands and
algorithm details. `implementation_status: implemented` records the current
manual acceptance boundary while the specification remains Proposed. Current Whole acceptance uses the public workflow and independent Fraction oracle
on local Clang 21 strict/Apple. The executable checks full support/dirty and Run
failure behavior, mathematical knot selection, lifetime, strides/fenv, managed
work/output/workspace budgets and active cancellation, metadata/Empty, cache and
upstream collection, giant-output rejection and complete typed-mask validation.
Cross-platform historical results do not establish current Whole validation.
The manual target is EXCLUDE_FROM_ALL and has no CTest registration.
