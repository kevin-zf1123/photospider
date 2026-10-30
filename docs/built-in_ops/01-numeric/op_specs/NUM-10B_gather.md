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
implementation_status: implemented
---

# NUM-10B: gather

Gathers source slices along a required static Int64 `axis` using a dynamic Int64 indices tensor `[M]`. Inherits the [NUM baseline](NUM_common_contract.md); the rules here take precedence.

## Interface

Source and indices each contain one tensor under any schema/member key. Complete sample shapes include batch axes; source rank is 1..8 and all logical counts are at most 2^40. Source/output dtype is UInt8, Int64, Float32 or Float64. Output `values` uses `photospider.tensor`/`samples`, with ordinary source axes except `output.shape[axis]=M`.

## Mathematics

For global output coordinate o, the source s has non-axis coordinates unchanged and `s[axis]=indices[o[axis]]`. Repeated indices preserve order. Raw source bits, including signaling NaNs, signed zeros and infinities, are copied without arithmetic. This is a single-axis gather.

## Behavior

All indices lie in the source axis extent, including indices unrelated to the selected output, as an unchecked caller premise. A nonempty selection depends on the complete index vector, and on the source region matching the output's non-axis region with the axis expanded to its full source extent; this possible-source coverage can exceed the coordinates used by a particular indices binding. Empty selection reads nothing.

## Acceptance

Nonleading axes, repeated indices, valid unselected indices, sparse/empty selection, raw-bit preservation.
