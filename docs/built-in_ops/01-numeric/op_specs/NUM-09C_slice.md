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
repository_branch: ops-specs
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

Slice input `input` without changing rank, using dynamic Int64[rank] Result
inputs `starts` and `steps`. The source is a single-tensor Result with any schema
id/member key; its complete `sample_shape()` includes batch axes. Required
static String `counts` is a canonical list of positive per-axis output extents,
fixing output `values` shape at compilation.
For output coordinate o, read source s[j]=starts[j]+o[j]*steps[j]. Negative
steps support reverse traversal. Starts/steps numeric values can change between
executions while counts remains static.

Support UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64, with identical
output dtype and bit-preserved elements including signaling NaNs. Input/output
shapes have rank 1..8, positive extents and logical element count <=2^40. The
output is a `photospider.tensor`/`samples` Result whose full shape is ordinary
axes; facets and batch-axis metadata are empty. Actual-read typed input
validation remains required. No implicit cast,
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
static schema and parameter checks still run. Otherwise the Whole program
requests the active source, starts and steps through full Tensor Needs using
Data, Validation and Descriptor roles (role 13), which trigger typed-payload
validation. An unused step entry can then cause an upstream/typed failure.
Source gaps and unselected coordinates remain part of the complete input
preparation. The program publishes a complete Result with global output coordinates; `Q` limits
observed dependencies and downstream reads, not publication shape. Empty output
has empty coverage and support. Edits to observed input support invalidate the
recorded dependency. Endpoint validation still uses widened arithmetic and
covers the full slice.

## Layout, resources and errors

Required String layout is auto/view/dense, default auto in the helper. Inherit
[reshape's complete-output policy](NUM-09A_reshape.md). View requires one affine
input/output owner; same-owner compatible fragments may join after address-map
proof. Multiple owners make explicit View unavailable. Auto materializes a
complete packed output only for an unavailable view; Dense always materializes
it. Slice strides equal `source_stride[j] * steps[j]` for counts greater
than one and zero for singleton outputs. Widened stride overflow makes a view
unavailable; Dense can still copy valid logical coordinates. Auto never hides
upstream, validation, resource or cancellation errors. Dense allocates
`N * dtype_size` output bytes. The program reads source and control data through
authorized windows rather than collecting complete input payloads into second
buffers. Mapping costs O(N*rank), with cancellation and work checks during
computation and publication.

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

All formal profiles use Whole and disable cross-run content caching because
content does not prove physical owner/stride identity; same-Run sharing remains
available. The current Result workflow and oracle are recorded in
[NUM-09 Whole execution](../layouts-whole.md). Timing there is historical Value
Whole evidence, not a performance measurement of this Result implementation.
