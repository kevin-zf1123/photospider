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

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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

## Single-sample axes and Whole input support

A singleton count ignores that axis's step numerically. If all counts are one,
static projection excludes the complete step port, including a failing producer;
metadata is still checked. Otherwise Whole collects all steps, starts and source
before callback. An unused step entry can then cause an upstream/typed failure.
Source gaps and unselected coordinates are included in full-input preparation.
Empty reads nothing. Any active input edit invalidates the complete output;
control/domain/typed/upstream failures are Run-wide. Endpoint validation still
uses widened arithmetic and covers the full slice.

## Layout, resources and errors

Required String layout is auto/view/dense, default auto in the helper. Inherit
[reshape's complete-output policy](NUM-09A_reshape.md). View requires one affine
input and output owner; same-owner compatible fragments may join, while multiple
owners fail View and may be collected for Auto/Dense. Slice strides equal
source_stride[j]*steps[j] for count>1, and zero for singleton outputs. Widened
stride overflow makes a view unavailable; Dense can still copy valid logical
coordinates. Auto never hides upstream/validation/resource/cancellation errors.
Dense allocates N*dtype_size output bytes and may collect all source/control
payloads even for sparse demand; fixed state and O(rank) controls are admitted.
Mapping costs O(N*rank), with cancellation/work checks per element and publication.

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
its upstream would fail; numerical selection is starts while Whole reads the source. Include multi-axis mixed
positive/negative steps, signed-zero/sNaN data, dynamic control changes, singleton
axes, metadata limits, source strides/owners, auto/view/dense and disjoint reads.

All formal profiles use Whole. Current workflow, independent numerical/physical
layout oracles and resource/performance results are recorded in
[NUM-09 Whole execution](../layouts-whole.md). Earlier regional validation does
not establish current Whole behavior.
