---
spec_schema_version: 1
id: NUM-13B
parent_id: NUM-13
function: integral_image
proposed_operation_keys:
  - numeric.integral_image_strict
  - numeric.integral_image_accelerated_apple_silicon
  - numeric.integral_image_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-13B: integral_image

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute two-axis rectangular prefix sums with leading zero boundaries. Required
static String axes names exactly two distinct nonnegative input axes; normalize
the pair to increasing order. Input rank is 2..8 with positive extents. Output
values retains all other axes and adds one to each selected axis. All other axes
are independent batches, with no image/color semantics inferred. Input/output
logical counts must not exceed 2^40; output facets are empty.

For selected axes a,b and an output coordinate o, sum the input rectangle
s[a] in [0,o[a]), s[b] in [0,o[b]), with all other coordinates equal to o.
This is the exact rectangle sum, not a composition of rounded row and column
prefix outputs. Inherit reduce_sum's four input dtypes, same-domain destination
selection (integer default Int64, float default Float64) and final range checking
or correct rounding. Static dtype is explicit in direct nodes.

## Prefix semantics and exact demand

Inherit [prefix_sum's arithmetic and failure rules](NUM-13A_prefix_sum.md), using
rectangle contributors rather than line contributors. If either selected output
boundary is zero, the rectangle is empty: return integer zero or floating +0
without source Data reads. Nonempty rectangle NaN priority follows original
logical row-major coordinates, independent of which selected axis is processed
first. Preserve the shared payload conversion and mixed-infinity/zero rules.

Only requested rectangle results are rounded or range-checked. Internal exact
partial sums may exceed the destination range without failing an unrequested
result. Do not treat already rounded integral_image outputs as exact reusable
arithmetic state. All three CPU profiles are bitwise equivalent.

For Q, source Data support is the exact union of its anchored rectangles, keeping
all batch coordinates separate. Disjoint requested corners can produce an L-shaped
union; reading the missing bounding-box corner is not authorized. Add recognized
typed-input Validation closure separately. Empty Q or only zero-boundary requests
read no source Data. A source change at (i,j) affects output boundaries with
o[a]>i and o[b]>j in the same batch, plus retained validation dependencies.

## Algorithms, resources and errors

Use bounded exact row/column scan state or another proved exact rectangle-sum
algorithm. If using two passes, keep their internal accumulators exact rather
than rounding an intermediate table. Irregular request unions must retain their
exact read restriction; a convenient dense bounding pass cannot read gaps.
Account scanned source cells, repeated work if chosen, exact accumulator widths,
active row/column state, source owners/windows, frontier/checkpoint metadata,
output fragments and scratch. Output bytes cover requested coordinates only.

Host-account every state/checkpoint and key it by source/metadata/axes/dtype and
validation witnesses. No full integral table is implicitly required for a tiny
request. Work/capacity/stage limits may reject expensive support/state with
ResourceExhausted; do not broaden reads or approximate sums to evade them.
Check cancellation during scan blocks and extended arithmetic, preserving the
prefix contract's publication, cache-off, lifetime and release rules.

Compile/preflight rejects invalid axes/rank/dtype, overflowed +1 shape arithmetic
and input/output counts above 2^40. Requested integer final overflow uses
OperationFailed/ArithmeticOverflow with output coordinate. Floating exceptional
results are numeric outcomes; upstream/typed/resource/cancellation failures keep
their existing categories. Failed observations publish no partial result.

## Acceptance and implementation status

Conceptual fixture: input=[[1,2],[3,4]], axes="0,1" yields
[[0,0,0],[0,1,3],[0,4,10]]. Use independent exact rectangle summation as the
oracle. Test zero boundaries with no source reads, non-leading integral axes,
independent batches, disjoint corners with an L-shaped source union, cancellation
that makes a requested final sum representable despite overflowing unrequested
partials, NaN priority across axes and all inherited dtype/stride/resource cases.

Four-corner rectangle queries on rounded floating outputs may introduce their
own rounding/cancellation error; this operator guarantees each prefix result,
not exact recovery from arbitrary downstream subtraction. Test exact integer
fixtures separately from floating retrieval accuracy. Deliver public workflow
runs and lifetime/invalidation checks; the implementation evidence below records the checks actually run.

## Implementation and executable acceptance

Six suffixed keys are registered through `numeric_scans.cpp`; public authoring
helpers live in `photospider/numeric/scans.hpp`. Each operation retains an exact
accumulator and a separate conversion snapshot, preserving original source
NaN/Inf/zero classification. Empty boundaries emit positive zero without Data.
Windows transport at most 64 numeric source values; typed Validation closure
is accounted separately and can require additional channel values.

Regional prefix requests group outputs by line and increasing boundary, scanning
only through each line's largest requested boundary. Integral rectangles are
streamed independently in original row-major order; repeated arithmetic is
charged. Separate executions and `execute_atoms` observations can recompute
source values. No persistent checkpoint or integral table is retained.

The output plan and complete per-observation association rows are host-accounted.
Each Need stage processes all requested rows; dense prefix boundary requests
can therefore require quadratic association work even though source terms are
scanned once. Work/capacity/stage limits reject excessive requests explicitly.
This is not a whole-execution linear-time or once-per-Run guarantee.

The manual `photospider_numeric_scans` target and `scan_oracle.py` are described
in [the workflow example](../../../../examples/numeric_workflow/README.md).
Local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 runs passed
2,280 independent Fraction/raw-bit cases per profile on 2026-09-19, plus
public workflow fixtures, sparse/L-shaped support,
integer Atom isolation, typed/Empty/zero boundaries, negative strides, fenv,
output-cap checks, sorting work/cancellation cleanup, and 4,096 source values
through 65 windows with four sparse results and at most 16 KiB payload. The
installed strict/Apple consumers, focused compiler unit, formatting/lint and
independent math/entry reviews passed. No integration test or CTest
registration is added; specification status remains Proposed.
