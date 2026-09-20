---
spec_schema_version: 1
id: NUM-10-scatter
parent_id: NUM-10
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

# NUM-10: scatter into a base array

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Provide four independent operations: scatter_replace, scatter_sum,
scatter_minimum and scatter_maximum, each with strict and the two platform CPU
accelerated keys under array. Inputs in order are `base`, `indices`, `updates`.
Indices is a dynamic Int64 array [M]. Required static Int64 axis selects a
nonnegative base axis less than rank. Updates has the same dtype/rank and
non-axis extents as base, with axis length M. Output `values` has base dtype
and shape; no mutation of base is permitted.

For update-axis index j, the update slice targets base axis coordinate indices[j].
Output coordinates with no matching index retain base. This is the single-axis
counterpart to gather, not coordinate-tuple scatter_nd.

Repeated targets are allowed. Replace chooses the matching update with greatest
j; sum/minimum/maximum include base first and then all matching updates ordered
by increasing j, defining priority for exceptional values. Numeric aggregate
rounding is specified separately; this order is not an implicit rounded-sum rule.

Every nonempty request collects and validates complete base, indices and updates
before callback. All indices must be in [0,base.shape[axis]); an invalid index
fails Domain/Run with IndexOutOfBounds even outside the consumer projection.
Empty reads nothing. All active source edits invalidate the complete output.

For numerical evaluation, J(q)={j:indices[j]=q[axis]}. No hit copies base raw
bits. Replace copies updates[max J] raw bits. Aggregates evaluate base followed
by increasing matching j. Earlier overwritten or unrelated values do not enter
the arithmetic, but their upstream/typed failures can fail Whole preparation.

## Types, storage and resource contract

Support UInt8, Int64, Float32 and Float64 for base/updates/output, with no implicit
conversion. Shapes have rank 1..8 with positive extents; base/output and updates
logical element counts are each <=2^40, indices has 1<=M<=2^40 subject to update
shape constraints. Output facets are empty; recognized typed input obligations
cover all active input values. There are no static parameters other
than axis and no layout/atomic-order mode.

Output owns complete packed base.shape storage, including for sparse demand.
Inputs may also require complete packed collection; immutable negative/zero
strides and unaligned elements remain valid. No writable alias is returned.

For M indices and N complete output elements, stable eight-pass radix grouping
costs O(M); per-output range lookup costs O(log M), followed by actual contributor
arithmetic. The plan and sort scratch each contain M pairs of two uint64 values
(peak32*M metadata bytes), with sort scratch released after grouping. The exact
accumulator is a fixed admitted workspace. Bounded rank<=8 coordinate vectors
are reused. No per-output dependency descriptors, source-set dedup or numeric
atom diagnostics remain. All metadata allocations and numerical work obey the
host ledger; cancellation is checked during scan, sorting, each output and
extended exact arithmetic. Any failure releases unpublished output/state.

Index/source edits invalidate all output observations. Content caching retains
full logical input witnesses. Aggregate order, raw selection and immutable
owner lifetime are independent of cache policy.

## Common errors and acceptance

Compile/preflight rejects unsupported dtype, mismatched rank/non-axis shape,
wrong index dtype/shape, invalid axis or logical count excess. Runtime invalid
indices use InvalidArgument, FailureReason::InvalidDomain and diagnostic
IndexOutOfBounds with j, index and destination extent. Integer result overflow
uses OperationFailed with FailureReason::ArithmeticOverflow at the failing complete-output
coordinate with Domain/Run attribution. Upstream/typed/resource/cancellation errors retain existing categories.

All formal profile keys use CPU Whole. Current public workflows, independent
coordinate/contributor/Fraction oracles, failure/resource checks and performance
are in [NUM-10 Whole execution](../indexing-whole.md). Earlier 2026-09-14
regional strict/Apple/WSL and installed checks predate this migration.

Floating environment, fixed NaN bit patterns and basic arithmetic conventions
follow [NUM-04](NUM-04_unary_contract.md), with the explicit no-hit/replacement
copy exceptions below.

## Aggregate exceptional values

If no update hits an output coordinate, every variant copies base bits exactly,
including signaling NaNs; there is no arithmetic quieting on that path. For a
hit coordinate, aggregate variants classify all contributing values in the order
base, then increasing update j. The first NaN wins, preserving sign/payload and
setting its quiet bit. NaN priority precedes generated exceptional results.

For sum without NaNs, simultaneous positive and negative infinities produce the
fixed positive quiet NaN; otherwise any infinity determines its own signed
infinity result. Finite contributors are summed exactly with only final rounding
or integer range checking. Exact zero is -0 only when all contributors are -0;
otherwise it is +0. Nonzero exact underflow retains its sign. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.

Minimum/maximum support all four dtypes and follow NUM-05 numerical ordering and
signed-zero selection: minimum of mixed zeros is -0, maximum is +0, same-sign
zeros retain their sign. Integer comparisons never pass through float. These
aggregate rules operate only when there is at least one matching update.

## Individual specifications

| Spec | Duplicate target behavior |
| --- | --- |
| [NUM-10C scatter_replace](NUM-10C_scatter_replace.md) | Last matching update wins |
| [NUM-10D scatter_sum](NUM-10D_scatter_sum.md) | Exact sum including base |
| [NUM-10E scatter_minimum](NUM-10E_scatter_minimum.md) | Minimum including base, NaN propagating |
| [NUM-10F scatter_maximum](NUM-10F_scatter_maximum.md) | Maximum including base, NaN propagating |
