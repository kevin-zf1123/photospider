---
spec_schema_version: 1
id: NUM-11E
parent_id: NUM-11
function: reduce_count
proposed_operation_keys:
  - numeric.reduce_count_strict
  - numeric.reduce_count_accelerated_apple_silicon
  - numeric.reduce_count_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-11E: reduce_count

Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-11 shared contract](NUM-11_reduction_contract.md). Count is discrete and exact in every profile.

## Interface

`numeric.reduce_count_*` takes one Result containing exactly one tensor member at any key. Static `axes` chooses a nonempty set of distinct axes. The `values` output is an Int64 Result using schema `photospider.tensor` and member `samples`; it has the input rank, reduced extents set to one, ordinary axes and no attached domain description. Input schema and axes are validated statically; a change of shape, type or axes requires a new compilation.

## Mathematics

Every group value is the product of the input extents on the reduced axes. Since the input element count is at most 2^40, each count fits Int64.

## Behavior

The result depends only on static schema metadata; no input sample payload is read, so upstream sample producers are not required when no other consumer needs them. Empty selection reads nothing.

## Acceptance

Input shape `[2,3,4]`, `axes="1,2"`: output shape `[2,1,1]`, both groups 12. Shape `[2^20,2^20]` reduced along axis 1 gives `[2^20,1]` with every value `2^20`.
