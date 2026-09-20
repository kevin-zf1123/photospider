---
spec_schema_version: 1
id: NUM-12B
parent_id: NUM-12
function: quantile
proposed_operation_keys:
  - numeric.quantile_strict
  - numeric.quantile_accelerated_apple_silicon
  - numeric.quantile_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-12B: quantile

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute one quantile per line along a required static nonnegative Int64 axis.
Input `input` supports UInt8/Int64/Float32/Float64. Dynamic input `q` is an
independent Float32/Float64 scalar array [1], shared by all lines, finite and in
[0,1]. Output `values` retains input shape except the reduced axis has length 1.
Required static String dtype is float32/float64; constructors write float64 by
default and direct nodes specify it. Output facets are empty.

Input rank is 1..8 with positive extents and the NUM logical array cap 2^40.
There is no q array, rank removal, implicit cast/broadcast or multi-axis quantile
mode. Integer samples are interpreted exactly before output conversion.
For a line of length N, let x[0..N-1] denote stable ascending numerical order.
Compute h=(N-1)*q exactly, j=floor(h), and w=h-j exactly. If w=0, select x[j];
otherwise interpolate (1-w)*x[j]+w*x[j+1]. This also covers q=1 without an
out-of-range x[N] access. For finite samples, only the final selected/interpolated
mathematical result is correctly rounded to output dtype. All three CPU profiles
are bitwise equivalent. No rounded floating h or preliminary Int64-to-float
conversion is permitted. There is no interpolation-method parameter.

## Exceptional values and zero signs

Inspect the entire selected source line for NaNs before selecting order statistics,
even at q=0/1. Propagate its first NaN in original logical axis order, quieting it
and applying the [reduction payload conversion](NUM-11_reduction_contract.md).
No NaN is omitted, and a source NaN cannot hide an invalid q when q is required.
Stable order treats signed zeros as equal and retains original order.

Without source NaNs, w=0 selects that value alone, including an infinity or signed
zero; convert directly to destination dtype with correct rounding/sign. For
0<w<1, same-sign infinities or one infinity paired with a finite sample yield
that signed infinity. Opposite infinities yield the fixed positive quiet NaN.
Two -0 interpolation endpoints yield -0, other exact zero mixtures yield +0;
nonzero exact results that underflow retain their mathematical sign. Unselected
infinities elsewhere in the line do not by themselves force NaN.

## Control and source dependence

For a nonempty output request with N>=2, first read q[0] as Control support and
require its finite [0,1] range. A violation fails before source Data evaluation
with InvalidArgument, FailureReason::InvalidDomain and diagnostic
InvalidQuantileProbability. Negative zero is valid zero. Retain q validation and
control witnesses; no input NaN can erase that failure.

For N=1, do not read q at all or apply its numeric range validation. Convert the
single selected source value directly to output dtype, retaining signed zero and
applying NaN quieting/payload mapping. Static q port dtype/shape checks remain.
This is an explicit exception to the general q requirement.

Each requested output coordinate needs its entire corresponding input axis line,
even at endpoint q. Other source lines have no Data demand; typed Validation
closure is tracked separately. Empty Q reads neither input nor q. Source changes
invalidate the corresponding output line; q changes invalidate all relevant
outputs when N>=2 and have no effect when N=1. Retain exact source/control and
validation witnesses with axis/dtype/profile in cache identity.

## Algorithms, resources and publication

Stable full sorting is a reference algorithm with O(N log N) work and O(N)
state per active line. Exact selection of the two stable order statistics is
allowed if equivalent, with NaN priority and zero tie order preserved. There is
no approximate quantile sketch or rank-error tolerance. Compute h/w and final
interpolation with exact arithmetic or a certified correctly rounded equivalent.

Budget all source owners/windows, selection/permutation state, q/control support,
exact arithmetic limbs, output fragments and scratch. Use bounded active lines
and host workers. A line too large for admitted work/capacity fails
ResourceExhausted, without hidden disk storage, skipped samples or approximation.
Check cancellation during ingestion, selection/sorting and arithmetic refinement.
Publish owned packed requested output fragments with correct global origins;
no full logical output is forced by a partial request. No partial failed result
is published, and final owners remain valid after context destruction. Typed,
upstream, resource and cancellation errors keep their existing Status categories.

## Acceptance and implementation status

Fixture: input=[0,10,20,30], q=[0.25], axis=0 -> values=[7.5].
Test median q=0.5, endpoints, h exactly integral and its floating neighbors,
Int64 values above 2^53, signed-zero ties, selected/unselected infinities, NaNs
anywhere in the source line, and both output dtypes. Use independent exact
position/interpolation arithmetic and stable order statistics as the oracle.

Prove full selected-line source reads and no unrequested-line reads. For N=1,
q's upstream must not execute even if it would fail; for N>=2 invalid q must
fail before source values are requested. Test disjoint outputs, dynamic q/source
invalidation, dtype/payload conversion, scratch exhaustion, cancellation,
cache-off and output lifetime through the public WorkflowDocument execution.
The current three profile keys use exact UInt128 rank selection and 4352-bit
one-final-rounding interpolation. The
[numeric workflow README](../../../../examples/numeric_workflow/README.md)
records manual and independent oracle evidence.
