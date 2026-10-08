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

The operation exposes two independently selectable named outputs. `values` is
a `Result` with schema `photospider.tensor`, tensor key `samples`, source
element type and full input shape. `indices` has the same shape and schema but
Int64 elements; each value is the original coordinate on the sorted axis.
Batch axes become ordinary output axes and both output tensor specs have empty
facets. Each selected output produces its own complete Result payload. A joint
workflow therefore has distinct Result owners for `values` and `indices`.

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

## Whole demand and execution

Every nonempty selected-output request uses Whole execution. It requests the
complete source tensor with Data, Validation and Descriptor roles, sorts every
line, writes the complete selected output and then satisfies the consumer
projection. An indices-only request still reads the complete source to build
the permutation. Any source change invalidates every recorded observation for
each selected output. An Empty request reads no input payload and performs no
sample arithmetic; it publishes the declared schema with empty sample
coverage.

`values` and `indices` are independent outputs, not one callback that always
allocates both. Selecting them separately invokes independent Whole callbacks;
selecting both in one workflow produces two Results and two payloads. A callback
builds one permutation per line and uses it to write its selected output. No
cross-output permutation cache is promised.

## Algorithm, resources and errors

The implementation classifies each source sample once to form a numerical key,
then uses iterative stable heapsort on `(key, original index)`. A line of length
L takes O(L log L) ordering work. Its key and permutation vectors use 16*L element bytes plus
allocator headers, alignment and Entries reservations. They are charged as
Root metadata. Keys are released after sorting; the permutation is released
after that line is written. Fixed `OrderingState` uses the phase allocator's
Payload capacity. Authorized input read windows and the selected full output
payload coexist with this state; the operation does not first pack the complete
input.

Work is O(M log L) for M total input elements and axis length L. The callback
charges reads, comparisons, permutation construction and output writes to the
execution Root and observes cancellation through those work calls. Capacity,
work-limit, cancellation or source failure aborts the transactional Whole
writer; unpublished output and ordering state are released, and no partial
coverage is published.

Malformed dtype, rank, shape or axis is rejected during metadata specialization
or static parameter validation. NaNs and infinities are successful sort values.
Arbitrary valid affine source strides are read through owning tensor windows.

## Current behavior checks

The current public behavior entry is `examples/numeric_workflow/ordering.cpp`,
registered as `test_numeric_ordering_result`. It checks
`[3,1,1,2]` values and indices, separate output selection, full publication for
sparse demand, stable non-last-axis sorting, dirty support, and ordering of
signed zeros, infinities and signaling-NaN payloads. See [NUM-12 Result Whole
execution](../ordering-whole.md) for commands and current evidence limits.

 The checks do not claim x86 arithmetic execution, performance
or complete typed-facet/error/resource coverage. The specification remains
Proposed.
