---
spec_schema_version: 1
id: NUM-10B
parent_id: NUM-10
function: gather
proposed_operation_keys:
  - array.gather_strict
  - array.gather_accelerated_apple_silicon
  - array.gather_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-10B: gather

The strict key follows its exact numeric reference. Accelerated floating
results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md)
where arithmetic applies; raw copies and discrete results remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements. The rules below
specify the NUM-10 behavior.

Gather slices from `input` along a required static Int64 `axis`, using a dynamic
Int64 `indices` tensor of shape `[M]`. Each input is a Result with exactly one
tensor member at an unrestricted key. The member's `sample_shape()` includes
batch axes and has rank 1..8. Input and output contain at most 2^40 elements;
`1 <= M <= 2^40`, subject to the output limit. The output port `values` is a
Result with schema `photospider.tensor`, member `samples`, and the complete
output shape as ordinary axes. Facets and batch topology are dropped.

The input and output dtype is UInt8, Int64, Float32 or Float64. Output shape
matches input shape except `output.shape[axis] = M`. For output coordinate `o`,
read `input` at `s[j] = o[j]` for `j != axis` and
`s[axis] = indices[o[axis]]`. Repeated indices retain their output order.
This is single-axis gather, not coordinate-tuple `gather_nd`.

## Whole demand and support

Static schemas and parameters are checked during specialization. For nonempty
demand, the Whole program requests the complete input and index vector with
data, validation and descriptor support (role 13), triggering typed-payload
validation. Every index must be in `[0, input.shape[axis])`, including indices
whose output coordinates are outside consumer demand. Empty demand requests no
payload and performs no sample access. Edits to either input dirty the complete
output.

## Storage, resources and errors

The complete output is packed and owned, costing `N * element_size` bytes.
Source and index data are read through authorized windows rather than collected
into additional complete input buffers. The index plan uses 16*M metadata
bytes, one key/position pair per index. It preserves source bits, including
signaling NaN payloads, signed zeros and unaligned elements. Work is charged
while validating indices, building the plan and copying output; cancellation and
resource failures publish no partial output.

Preflight rejects dtype, rank, shape or axis violations and output counts above
2^40. An out-of-range index returns `InvalidArgument`,
`FailureReason::InvalidDomain`, with an `IndexOutOfBounds` diagnostic that
identifies the index position, value and source extent. Source,
typed-validation, resource and cancellation failures retain their established
categories.

The public workflow checks nonleading-axis gather, repeated and invalid indices,
strided reads, Empty demand, and raw-bit preservation. The existing
`test_numeric_result_math` integration fixture has additional nonleading-axis
UInt8 and negative-stride index cases; it was not rerun for this Result
migration. Current commands and evidence boundaries are listed in
[NUM-10 Whole execution](../indexing-whole.md).
