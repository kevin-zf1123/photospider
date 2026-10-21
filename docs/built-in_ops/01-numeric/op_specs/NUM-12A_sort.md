---
spec_schema_version: 1
id: NUM-12A
parent_id: NUM-12
function: sort
proposed_operation_keys:
  - array.sort_strict
  - array.sort_accelerated_apple_silicon
  - array.sort_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-12A: sort

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration and shared error conventions. This member specifies stable
ascending sorting of every logical line along one static axis. Its specification
status remains Proposed; that status does not imply the keys are absent from the
runtime.

## Tensor ports and output selection

The `input` port is a `Result` containing exactly one tensor member at index
zero. Its `ResultTensorSpec::key` may be any key. Supported element types are
UInt8, Int64, Float32 and Float64. The input `sample_shape()` includes declared
batch axes. Its rank is 1..8, all extents are positive, and the complete logical
element count is at most 2^40.

`axis` is a required static Int64 parameter in `0..rank-1`, measured against
the full sample shape, including batch-prefix axes. There is no descending,
unstable, implicit-cast or image-semantic mode.

The `values` output is a Result with schema `photospider.tensor` v1, member
`samples`, the source dtype and full input sample shape. `indices` has the same
schema and shape with Int64 elements naming original coordinates on the sorted
axis. Batch axes become ordinary output axes. Neither output carries a domain
description.

The two ports are independently selectable. Selected tiles with identical non-axis origins and extents share one fixed CPU task. That task sorts each corresponding complete source line once and writes only declared tiles, including either or both outputs. Each tile owns an independent dense allocation. No complete internal values/indices pair is allocated.

## Ordering and value preservation

For each line, sort by `(numerical key, original axis index)`. Integer keys use
exact signed order without floating conversion. Floating values, including
infinities, use ascending numerical order. Signed zeros compare equal. Every
NaN sorts after every non-NaN, and NaNs compare equal for ordering. The original
axis index breaks ties, so equal values and NaNs retain their source order.

The `values` output copies original bits. Signaling NaNs and payloads are not
quieted or otherwise changed by sorting. The `indices` output records the
corresponding source-axis position. All three declared CPU profiles preserve
these discrete results.

## Fixed execution

For each selected tile, keep its non-axis rectangle and expand the sorted axis to the full source interval. Lowering declares source tiles covering that rectangle using the source's own TileGrid. An indices-only request needs the same complete lines to determine order. Source tile rounding can cover additional samples, but those samples are not sorted. Empty selection emits no task or payload reads.

A task may write several sorted-axis tiles and both public ports. Selecting outputs from a saved Plan retains all reads and writes of a retained task; fresh compilation can form smaller groups. All tasks share their IR call's publication outcome. An operational task failure prevents successful publication of that call.

## Algorithm, resources and errors

The implementation classifies each sample of a visited line once and applies iterative heapsort to `(key, original index)`. Axis length L requires O(L log L) ordering work per line and `16*L + sizeof(StableOrderWorkspace)` bytes of preallocated invocation scratch. The keys and permutation are reused across lines. Runtime allocates no payload or dynamic permutation vector. Root reserves selected output storage and the maximum scratch requirement per CPU lane, plus executor metadata.

Cancellation is checked during classification, comparisons and writes. Source, resource, backend and cancellation failures retain their operational status. Accepted tasks drain before failed outputs are released. Successful output owners can outlive ExecutionContext. Floating modes and exception flags are restored.

Metadata validates dtype, complete rank/count and static axis. Prepare validates both output schemas, canonical Host writes and their common non-axis ROI, exact complete source reads, empty task parameters and scratch. Static parameter errors return InvalidArgument; incompatible schemas or invocation signatures return TypeMismatch. All source bit patterns, including NaNs and infinities, are valid.

## Acceptance

Compare with the retained [NUM12 oracle](../../../../oracle/ops/numeric/num12/reference.py). Verify `[3,1,1,2]` produces values `[1,1,2,3]` and indices `[1,2,3,0]`; include signed zeros, signaling NaNs and extreme integers. Check separate and mixed output selection, shared task count, exact source tile closure, non-last axes, tail tiles, rank eight, batch/atomic input geometry and reversed strides. Exercise concurrent Runs, artifact import, rebinding, cancellation, capacity rejection, malformed task signatures and retained output ownership. See [fixed ordering execution](../ordering-execution.md) for resource and selection rules.
