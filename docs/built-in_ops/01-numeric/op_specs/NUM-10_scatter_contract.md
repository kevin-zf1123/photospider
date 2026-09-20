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

For every nonempty requested output Q, scan and validate all indices before
source value reads. Every index must be nonnegative and less than the base axis
extent; any invalid index fails the observation, even outside Q. No negative
index interpretation, wrap, clipping or ignored out-of-range update is allowed.
Empty Q reads nothing. Retain the full index scan as Control/validation evidence.

For output q, let J(q)={j:indices[j]=q[axis]}. If J is empty, read and copy base[q].
For replace with nonempty J, read only updates at j=max J, with other coordinates
from q; do not read overwritten base or earlier updates. For aggregate variants,
read base[q] and all updates selected by J. No unrelated update value is read.
Recognized typed-input validation may add separately declared support.

## Types, storage and resource contract

Support UInt8, Int64, Float32 and Float64 for base/updates/output, with no implicit
conversion. Shapes have rank 1..8 with positive extents; base/output and updates
logical element counts are each <=2^40, indices has 1<=M<=2^40 subject to update
shape constraints. Output facets are empty; recognized typed input obligations
remain attached to actually required reads. There are no static parameters other
than axis and no layout/atomic-order mode.

Output is newly owned dense storage per requested rectangle; it never mutates
base. Preserve original bits on replacement or no-hit paths. Legal source strides,
offsets, zero/negative strides and unaligned elements are supported. Return exact
requested coverage with correct global origins; do not allocate the entire base
or fabricate values outside Q.

For M indices, N requested output elements and C selected contributions, required
work is O(M+N+C) plus coordinate/set and exact arithmetic work. An implementation
may build bounded target buckets or scan indices in budgeted chunks, but must
finish required global validation before publishing. Charge index scan work even
for tiny Q, as well as index/source owners, target buckets, exact accumulators,
output bytes, typed validation, fragment metadata and scratch. There is no
unbudgeted full-index cache or unordered atomic accumulation.

Retain full index-control/validation witnesses plus exact base/update supports.
Index changes invalidate and replan observations that depended on the scan;
source changes invalidate only their retained contributors or validation support.
Earlier overwritten replace updates do not form value dependencies. Account
optional immutable index-plan caches, keyed by source versions and geometry;
cache-off does not change results or ownership.

Check cancellation per scan/mapping stage, at least every 4096 copied values,
and during extended arithmetic/refinement. Use host workers and budgets. Budget
failure returns ResourceExhausted rather than broadening reads, dropping updates
or changing numeric order. Published result owners survive context destruction;
failed observations publish no partial result and release unpublished state.

## Common errors and acceptance

Compile/preflight rejects unsupported dtype, mismatched rank/non-axis shape,
wrong index dtype/shape, invalid axis or logical count excess. Runtime invalid
indices use InvalidArgument, FailureReason::InvalidDomain and diagnostic
IndexOutOfBounds with j, index and destination extent. Integer result overflow
uses OperationFailed with FailureReason::ArithmeticOverflow at the requested
coordinate. Upstream/typed/resource/cancellation errors retain existing categories.

The twelve scatter keys use the public `scatter_replace_node`, `scatter_sum_node`,
`scatter_minimum_node` and `scatter_maximum_node` helpers. Regional Atomic
callbacks retain an explicit association row per requested output: full index
Control/validation, then exact base/update Data and typed Validation. Stable
radix grouping retains increasing update position within each target. Bounded
binary lookup and source-set construction charge their actual mapping work;
replacement reads only the last match and aggregates retain all contributors.
The fixed exact workspace admits all limb storage through its continuation.

`examples/numeric_workflow/indexing.cpp` and `index_oracle.py` provide the
public manual acceptance path. On 2026-09-14, local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/AVX2 passed the complete manual workflows and 3858 independent
coordinate/contributor/Fraction cases per profile. The installed public consumer
passed. Checks include exact reads and dirty support, typed actual-read closure,
raw/quiet NaN and zero rules, strided input, four fenv modes, changed-index cache
replanning, Empty, cancellation, work/state limits and failed-attempt diagnostics.
Focused compiler/dependency/fragments/resources units and independent scoped
reviews passed. Manual acceptance has no integration-test registration.
Specification status remains Proposed; no performance claim is inferred.

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
