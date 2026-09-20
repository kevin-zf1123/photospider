---
spec_schema_version: 1
id: NUM-11
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-11: reductions

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Seven independent operations are reduce_sum, reduce_minimum, reduce_maximum,
reduce_mean, reduce_count, reduce_variance and reduce_std. Each has strict and
the two independently named platform CPU accelerated keys under numeric.

Each takes array `input` and emits `values`. Required static String `axes` is a
canonical comma-separated list of distinct nonnegative axis indices, at least
one and within input rank. The set is order-independent; constructors canonicalize
to increasing order. There is no keepdims parameter: every reduced axis remains
present with extent 1. Nonreduced extents remain unchanged, including for full
reduction, which yields rank-preserving all-one extents rather than a rank-zero
scalar. Input rank is 1..8 with positive extents and logical element count <=2^40.
Output facets are empty; recognized input typed semantics retain actual-read
validation obligations. No empty reduction group arises
under these shape rules.

At an output coordinate, a reduction group contains all input coordinates that
match its nonreduced axes, spanning the full extent of every reduced axis.
Reduce_count counts every logical group element, including zeros and nonfinite
values, returning Int64 and depending only on metadata, with no input sample read.
Reduce_sum supports all four dtypes. Integer inputs default to Int64 output,
floating inputs to Float64; an explicit dtype may select UInt8/Int64 for integer
inputs or Float32/Float64 for floating inputs. Sum uses exact total arithmetic
with one final range check or correctly rounded conversion.

Remaining dtype mappings and numeric differences are specified by the individual
operator files below. These specifications do not implement
or change the current runtime's existing statistics operations.

## NaN ordering and payload conversion

For numeric reducers, the first input NaN in logical row-major order within the
reduction group wins; preserve its sign and quiet it. This order is independent
of the axes parameter ordering, block size, thread schedule and physical strides.
Do not skip later required group reads or typed validation because a NaN was seen.

When input/output float dtypes match, preserve payload bits and set the quiet bit.
When they differ, extract payload excluding the quiet bit (22 bits for Float32,
51 for Float64). For Float32->Float64, shift that payload left 29 bits; for
Float64->Float32, shift right 29 bits, and if the source payload was nonzero but
the retained payload is zero, set destination payload bit 0. Set the destination
quiet bit and retain sign. This mapping explicitly permits information loss on
narrowing; it is independent of native cast behavior. A newly generated NaN is
the fixed positive quiet pattern in the selected output dtype: Float32
0x7fc00000 or Float64 0x7ff8000000000000.

## Group demand, mapping and invalidation

Except reduce_count, each requested output coordinate requires its entire
reduction group as exact Data support. Union those groups for requested Q;
unrequested groups are not read or numerically validated. Add recognized typed
Validation closure separately. Empty Q reads no samples. A numeric exceptional
value does not permit short-circuiting required group dependencies or validation.
Reduce_count instead uses its explicit metadata-only contract.

Map changed source coordinates to output by setting reduced-axis coordinates to
zero and retaining nonreduced coordinates. Retain corresponding Data/Validation
witnesses. Shape, dtype, normalized axes, output dtype/profile and any numeric
parameters belong to inference/cache identity. Output descriptors are static.
For value-reading reducers, publish owned packed fragments for requested output
coverage, with correct global Region/storage origins; no implicit zero groups or
whole-output allocation is required to serve a partial request.

## Bounded execution, ownership and errors

Read valid immutable strides/offsets through logical coordinates, preserving
original dtype bits for classification and exact arithmetic. Groups can be
streamed through bounded blocks; do not retain an entire input group merely for
a sum or moment. Work scales with actual requested group elements plus exact
arithmetic/refinement and validation. Account active group accumulators, retained
source owners/windows, dependency/set metadata, output fragments and scratch.
Exact accumulator width depends on dtype exponent/significand range and group
size; host-account every limb or fixed-width capacity. Precision or workspace
limits cause ResourceExhausted rather than silently changing quality.

Use host workers and cancellation, polling per group/block and at least every
4096 processed values as well as during refinement. Group chunking, thread order
and SIMD width cannot change logical output bits for exact profiles. Retain NaN
priority by logical index rather than first thread completion. No unbudgeted
private pools, Whole input copies or unordered floating atomic accumulation.
Source execution may have its own transitive support; this reducer declares only
the exact support it requires and does not suppress actually required upstream
failures.

Compile/preflight rejects malformed axes, unsupported type/shape, invalid static
numeric parameters or descriptors beyond the array cap. Integer final overflow
uses OperationFailed with FailureReason::ArithmeticOverflow and output coordinate.
Floating domain results follow individual numeric tables, not generic Status
failure. Upstream/typed/resource/cancellation failures retain existing categories;
a failed observation publishes no partial output. Published owners remain valid
after context destruction until final release, with cache-off equivalent behavior.

## Shared acceptance and implementation distinction

Use independent exact integer/rational or directed high-precision oracles, with
explicit NaN bit mapping. Test whole versus nonzero/disjoint output requests,
multiple axes, singleton groups, strided inputs, selected versus unrequested group
failures, deterministic partitioning, source invalidation, low accumulator/index
budgets, cancellation and output-owner lifetime. The current implementation
registers 21 keys, with public constructors in
`photospider/numeric/reductions.hpp`, the manual workflow in
`examples/numeric_workflow/reductions.cpp`, and the independent oracle in
`reduction_oracle.py`. Value-reading reducers stream groups through windows of
at most 64 logical values, use one scalar observation group per requested output
coordinate, and retain exact accumulator state in bounded host continuations.
Diagnostics count admitted accumulator input attempts as `evaluated_values`;
output observations are reported separately as `computed_elements`.
`reduce_count` evaluates zero numeric inputs and can publish one 8-byte
zero-stride owner for repeated counts.

Local Clang 21 strict and Apple full manual workflows passed, including streamed
4096-element groups under a 16 KiB live-payload limit, giant 2^40 count with an
8-byte owner and zero producer calls, atom support/dirty/overflow isolation,
typed validation, Empty, cancellation, ddof and strided-NaN cases. The strict
and Apple installed consumers passed. Ubuntu WSL Clang 18.1.3
strict/x86 full manual workflows passed, including the required third-window
source failure after an earlier NaN. The updated oracle passed 4740 cases per
profile, and the additional cross-window cases passed. Scoped implementation
and arithmetic reviews closed all required findings. This does not change the
Proposed status.

Existing numeric.mean and numeric.variance use
[ordered reduction](../../../../plugins/ops/01-numeric/ordered_reduction.hpp):
Float32/Float64 input, all-element dependency scans with a rounded Float64 sum,
finite-only checks and optional block_size. That current staged implementation
is not the target multi-axis keepdims/exact-formula contract. No existing runtime
behavior is changed by these Proposed files.

## Individual specifications

| Spec | Output dtype | Formula quality |
| --- | --- | --- |
| [NUM-11A reduce_sum](NUM-11A_reduce_sum.md) | Same integer/float domain, default Int64/Float64 | Exact total, final range check/rounding |
| [NUM-11B reduce_minimum](NUM-11B_reduce_minimum.md) | Input | Exact numeric selection |
| [NUM-11C reduce_maximum](NUM-11C_reduce_maximum.md) | Input | Exact numeric selection |
| [NUM-11D reduce_mean](NUM-11D_reduce_mean.md) | Float32/64, default Float64 | Exact sum/count, final rounding |
| [NUM-11E reduce_count](NUM-11E_reduce_count.md) | Int64 | Exact metadata count |
| [NUM-11F reduce_variance](NUM-11F_reduce_variance.md) | Float32/64, default Float64 | Exact variance, final rounding |
| [NUM-11G reduce_std](NUM-11G_reduce_std.md) | Float32/64, default Float64 | Exact standard deviation, final rounding |
