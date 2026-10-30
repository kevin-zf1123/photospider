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

Stable ascending sort of every logical line along one static axis. Inherits the [NUM baseline](NUM_common_contract.md) for registration and shared error conventions.

## Interface

The `input` port is a `Result` containing exactly one tensor member at index zero, with any `ResultTensorSpec::key`. Supported element types are UInt8, Int64, Float32 and Float64. The input `sample_shape()` includes declared batch axes; rank is 1..8, all extents are positive, and the complete logical element count is at most 2^40.

`axis` is a required static Int64 parameter in `0..rank-1`, measured against the full sample shape, including batch-prefix axes. There is no descending, unstable, implicit-cast or image-semantic mode.

The `values` output is a Result with schema `photospider.tensor` v1, member `samples`, the source dtype and full input sample shape. `indices` has the same schema and shape with Int64 elements naming original coordinates on the sorted axis. Batch axes become ordinary output axes. Neither output carries a domain description. The two outputs are independently selectable. Invalid axis or dtype returns InvalidArgument/TypeMismatch statically; all source bit patterns, including NaNs and infinities, are valid.

## Mathematics

Each line is sorted by `(numerical key, original axis index)`. Integer keys use exact signed order without floating conversion. Floating values, including infinities, use ascending numerical order. Signed zeros compare equal. Every NaN sorts after every non-NaN, and NaNs compare equal for ordering. The original axis index breaks ties, so equal values and NaNs keep their source order.

`values` copies original bits; signaling NaNs and payloads are not quieted or otherwise changed. `indices` records the corresponding source-axis position. All three CPU profiles give identical results.

## Behavior

An output sample on a line depends on the complete source line along `axis` at the same non-axis coordinates, including for an indices-only request. Empty selection reads nothing. Selected outputs share one call: an operational failure prevents successful publication of that call's outputs.

## Acceptance

`[3,1,1,2]` gives values `[1,1,2,3]` and indices `[1,2,3,0]`. Cases: signed zeros, signaling NaNs, extreme integers, non-last axes, rank eight, separate and mixed output selection.
