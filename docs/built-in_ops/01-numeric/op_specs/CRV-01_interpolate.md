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
`photospider/ops/numeric/curves.hpp`. Each workflow input and output is a Result. An
input schema contains one tensor member under any schema id and member key;
`sample_shape()` supplies x[K], y[K] or y[K,C], and query[N]. Each input can use
Float32 or Float64 independently. The `values` output uses schema
`photospider.tensor`, member `samples`, no attached domain description, and shape [N] or [N,C]; the
multi-function column is an ordinary sample axis.

Strict and Float64 output use exact rational evaluation of the whole formula followed by one RN-even conversion. The arithmetic arena has 352 limbs (22,528 bits) per integer and 96 slots, with at most 49 live formula slots. Accelerated Float32 evaluation propagates conservative intervals through the complete formula and accepts a candidate only when both enclosure endpoints round to the same Float32 value. This preserves monotonicity across queries and exact fallbacks. Knots and selected clamps remain exact. Exact cross products recognize a collinear complete local PCHIP stencil and use the equivalent linear formula.

The mathematical stencil is local: an exact knot or clamp uses one y, linear interpolation uses two endpoints, and PCHIP uses up to four neighboring y values. Unused generic y samples undergo no additional finite scan. These mathematical selections differ from the physical read superset declared below.

## Fixed execution, resources and caller preconditions

Each selected output tile lowers to one fixed CPU Host task. For output rows `[r,r+R)` and, in the multi form, columns `[c,c+C)`, its immutable reads cover all `x[0:K]`, `query[r:r+R]` and `y[0:K,c:c+C]`. The scalar form reads all y. Each region is closed through its input's own tile grid, so batch, spatial or atomic axes can enlarge physical tile support. These reads are the static superset required for data-dependent knot search; the numerical stencil remains local.

The task evaluates only its selected global rows and columns. It searches x directly through fixed views, once per query row within that task, and reuses the classification across the task's columns. Query order and repetitions are unrestricted. Empty selection emits no payload task. Output tiles have separate dense allocations. `select_outputs` retains the closure of saved tasks; fresh compilation determines narrower task regions.

Finite x with strictly increasing values, finite consumed query/y values, representable final outputs and queries within the knot interval under `reject` are unchecked caller preconditions. Preparation validates schemas, parameters, canonical output views, the exact input tile set and fixed scratch capacity. The kernel performs no global numeric validation scan. Clamp, endpoint selection, extrapolation and exact rounding retain their mathematical definitions.

Each invocation constructs one `ExactCurve` in preallocated Host scratch of `sizeof(ExactCurve)`, containing the complete 96-slot integer arena and ratio workspace. Binary search uses bounded stack indices, and no knot vector or full slope table is allocated. The 352 limbs per integer are an arithmetic precision bound, not the total scratch byte count. Scratch is private to the invocation; prepared views are immutable and shared by concurrent Runs.

Search costs `O(R log K)` x reads per task plus `R*C` local evaluations (C=1 for scalar). Input tile owners, selected output allocations and granted-lane scratch require resource admission. Cancellation is polled during search, column traversal and exact arithmetic. Internal capacity/zero-denominator guards and accelerated quality certification remain active. Static errors retain `InvalidArgument` or `TypeMismatch`; unavailable profiles, resource exhaustion and cancellation retain their operational status. Violating numerical caller preconditions has no specified numeric diagnostic.

Tasks share their IR call's publication outcome. Successful task tiles can supply dependent tasks internally; public call Results appear only after all required tasks succeed and ordered retirement occurs. Rebinding reuses immutable preparation and produces independent Run-owned output. Content caching and repeated-demand identity are not provided. Result owners retain allocations beyond context lifetime.
