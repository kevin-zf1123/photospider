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

## Whole demand, mapping and invalidation

Except reduce_count, each nonempty request reads and validates the complete
input and computes every output group before projection. A source/typed failure
or integer overflow in any group fails Domain/Run, including groups outside the
consumer projection. Empty reads nothing. Any input edit invalidates every
recorded output observation. Numerical group membership and row-major NaN
priority remain unchanged. Reduce_count uses an empty static runtime-input
projection and metadata-only semantics, while validating the complete schema.

## Execution, ownership and errors

Numeric reducers allocate complete dense output plus one fixed exact state,
reset for each group. Whole input preparation may own a complete packed input;
the former64-sample streaming memory guarantee no longer applies. There are no
per-group dependency descriptors, numeric atom counters or intermediate windows.
Work is proportional to full input count and exact arithmetic/refinement.
Rank<=8 coordinate vectors are reused; state/output capacities and exact work
are charged to host budgets. Cancellation is checked per input and during exact
refinement and before publication. No partial failed output is published.

Count computes only the shape/axes product, owns8 bytes, and exposes the complete
keepdims output through zero strides even for huge logical arrays. No source
payload is read or retained, and source-byte edits do not invalidate counts.
Output backing/resources survive context destruction until their final release.
Precision/budget failures never change numerical quality.

Compile/preflight rejects malformed axes, unsupported type/shape, invalid static
numeric parameters or descriptors beyond the array cap. Integer final overflow
uses OperationFailed with FailureReason::ArithmeticOverflow, output coordinate and Domain/Run scope.
Floating domain results follow individual numeric tables, not generic Status
failure. Upstream/typed/resource/cancellation failures retain existing categories;
a failed observation publishes no partial output. Published owners remain valid
after context destruction until final release, with cache-off equivalent behavior.

## Shared acceptance and implementation distinction

The21 formal keys use Whole through public constructors in
photospider/numeric/reductions.hpp. Numerical state retains exact accumulation,
NaN conversion, ddof validation and final-rounding/root rules. Current public
workflow, independent Fraction/midpoint-square oracle, layout/error/resource
checks and separate public/core timing are in
[NUM-11 Whole execution](../reductions-whole.md). Earlier regional
strict/Apple/WSL/installed checks predate this implementation. Proposed status
is unchanged.

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
