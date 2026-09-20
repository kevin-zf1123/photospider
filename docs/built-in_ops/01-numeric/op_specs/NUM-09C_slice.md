---
spec_schema_version: 1
id: NUM-09C
parent_id: NUM-09
function: slice
proposed_operation_keys:
  - array.slice_strict
  - array.slice_accelerated_apple_silicon
  - array.slice_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-09C: slice

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Slice input `input` without changing rank, using dynamic Int64[rank] inputs
`starts` and `steps`. Required static String `counts` is a canonical list of
positive per-axis output extents, fixing output `values` shape at compilation.
For output coordinate o, read source s[j]=starts[j]+o[j]*steps[j]. Negative
steps support reverse traversal. Starts/steps numeric values can change between
executions while counts remains static.

Support four input dtypes UInt8/Int64/Float32/Float64, with identical output
dtype and bit-preserved elements including signaling NaNs. Input/output shapes
have rank 1..8, positive extents and logical element count <=2^40. Output facets
are empty, with actual-read typed input validation retained. No implicit cast,
rank squeezing, omitted axes, Python end-index interpretation or empty slice is
provided. All three CPU profiles preserve identical values.

For every nonempty output request, validate the whole slice domain rather than
just requested coordinates. Starts are nonnegative absolute indices. Steps must
be nonzero, with no wrap, clipping or negative-index reinterpretation. Compute
last[j]=starts[j]+(counts[j]-1)*steps[j] by exact widened integer arithmetic;
require both starts[j] and last[j] within input axis bounds. Monotonic stepping
then proves the entire axis valid. Invalid parameters fail the observation even
when the actual requested output subset would have valid source coordinates.

## Single-sample axes and staged support

If counts[j]=1, do not read steps[j] at all and do not require it to be nonzero.
Validate starts[j] only; the effective mapping on that axis is constant. This
is an explicit exception to the step rule. For nonempty Q, starts has Control
support all rank entries, and steps has Control support only axes with count>1.
Retain these control witnesses and validate the whole slice before dependent
source Data reads. Input metadata/dtype constraints remain compile-time checks.

After controls are valid, Data support is the exact mapped set S(Q). Nonunit
steps may produce disconnected input coordinates; do not replace them with a
bounding box or read gaps. Typed source Validation closure is tracked separately.
Empty Q reads neither control vector nor source Data. Changing any read control
invalidates and replans the whole output mapping; changing an ignored step has
no effect. Map changed source Data back using exact divisibility and index bounds
on each axis, then add retained validation invalidation.

## Layout, resources and errors

Required static String layout is auto/view/dense with constructor default auto;
direct nodes specify it. Inherit [reshape's per-request-rectangle layout policy](NUM-09A_reshape.md):
one rectangle must fit one owner/offset/stride representation for view; auto
otherwise copies, and explicit view reports ViewUnavailable. View byte strides
are source_stride[j]*steps[j] for count>1, and zero for singleton output axes.
Compute addresses and strides with checked widened arithmetic; a stride not
representable by Value's layout is not a valid view. Dense copying can still
succeed when the logical source coordinates are valid. No negative writable
alias or fabricated cross-owner view is published.

Inherit reshape's source-owner retention, exact-set/fragment mapping, immutable
output, cache, host budget, cancellation and final release requirements. Mapping
cost is O(M*r), with O(r) control validation plus actual sparse-support metadata.
Step sizes do not authorize allocating their bounding interval. Account all
control/source owners, run/index metadata and output/scratch bytes; fail
ResourceExhausted if exact support cannot be represented within the budget.

Invalid counts/rank/type/layout is a compile/preflight error. Invalid dynamic
starts/used steps or any full-domain endpoint outside the source axis fails with
InvalidArgument, FailureReason::InvalidDomain and diagnostic InvalidSlice,
including axis and offending integer values. Mathematical index arithmetic must
not wrap. Resource/upstream/typed/cancellation errors remain separate. Publish
no partial failed observation and check cancellation as required by reshape.

## Acceptance and implementation status

Conceptual fixture: input=[0,1,2,3,4,5], starts=[4], steps=[-2], counts="3"
yields [4,2,0]. Independent exact integer coordinate mapping and raw-bit reads
are the oracle. Verify a partial output request still rejects a full slice that
would run out of bounds. For counts="1", prove steps is never requested even if
its upstream would fail; source read is exactly starts. Include multi-axis mixed
positive/negative steps, signed-zero/sNaN data, dynamic control changes, singleton
axes, metadata limits, source strides/owners, auto/view/dense and disjoint reads.

The current nine `array.*` keys include the three explicit slice profiles.
`slice_node` in `photospider/numeric/layouts.hpp` emits static `counts` and
dynamic Int64[rank] `starts`/`steps`; `counts[j]==1` omits `steps[j]` from both
validation and dependency reads. Control dependencies use exact roles and
Data support contains only requested mapped points, with no bounding gap.
View strides are source stride times step, with zero stride on singleton axes;
`auto` falls back to packed output when a requested rectangle is not affine.

On 2026-09-14, local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/AVX2 passed the public manual examples and 636 independent integer/raw-bit
oracle cases per profile. The installed public consumer passed. Coverage includes
exact support/dirty mapping, whole versus regional layout policy, unaligned and
negative/zero strides, shared versus independent owners, ignored singleton steps,
full slice endpoint validation, typed Validation closures, schema/Empty behavior,
work/cancellation/capacity failures, fenv and escaped Value lifetime. Focused
compiler/dependency/fragments/resources units and independent scoped review passed.
Layout operations are `cacheable=false` because the content cache does not witness
physical owner/stride partitions. Managed metadata and its remaining host-container
boundaries are documented in [Managed Resources](../../../kernel-architecture/Managed-Resources.md).
The manual target is not registered in integration tests. Specification status
remains Proposed; no performance claim follows from correctness checks.
