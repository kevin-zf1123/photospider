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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-12B: quantile

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
mathematical result is correctly rounded to output dtype. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound. No rounded floating h or preliminary Int64-to-float
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

For N>=2, Whole collects and validates the complete source and q before callback.
The callback requires finite q in [0,1]; invalid q returns Domain/Run
InvalidArgument/InvalidDomain with InvalidQuantileProbability and raw q bits.
Negative zero is valid. A failing source may now fail before invalid-q callback
validation. No source NaN can erase a q error after successful preparation.

For N=1, static runtime projection excludes q entirely, including a failing
producer, while metadata checks remain. Convert the sole source sample with
signed-zero/NaN rules. Every nonempty request reads all active source lines and
computes the complete keepdims output. Any active input change invalidates all
recorded output observations; q bytes have no effect when N=1. Empty reads nothing.

## Algorithms, resources and publication

Stable full sorting is a reference algorithm with O(N log N) work and O(N)
state per active line. Exact selection of the two stable order statistics is
allowed if equivalent, with NaN priority and zero tie order preserved. There is
no approximate quantile sketch or rank-error tolerance. Compute h/w and final
interpolation with exact arithmetic or a certified correctly rounded equivalent.

Use the same once-classified key/permutation algorithm and metadata accounting as
[sort](NUM-12A_sort.md), plus fixed exact interpolation state. Each line is sorted
once per Whole callback. Complete input and output payloads are retained/allocated,
even for a sparse consumer projection. No permutation block cache or once-per-Run
sharing promise remains. Sorting/refinement checks host work/cancellation; errors
release all unpublished state. Source/typed failures retain their categories.

## Acceptance and implementation status

Fixture: input=[0,10,20,30], q=[0.25], axis=0 -> values=[7.5].
Test median q=0.5, endpoints, h exactly integral and its floating neighbors,
Int64 values above 2^53, signed-zero ties, selected/unselected infinities, NaNs
anywhere in the source line, and both output dtypes. Use independent exact
position/interpolation arithmetic and stable order statistics as the oracle.

Prove complete source reads, including unrequested lines. For N=1,
q's upstream must not execute even if it would fail; for N>=2 source failures may
precede invalid-q callback validation. Test disjoint outputs, dynamic q/source
invalidation, dtype/payload conversion, scratch exhaustion, cancellation,
cache-off and output lifetime through the public WorkflowDocument execution.
The formal keys execute Whole while retaining exact UInt128 rank and4352-bit
one-final-rounding interpolation. See [NUM-12 Whole execution](../ordering-whole.md)
for current workflow, validation and performance evidence.
