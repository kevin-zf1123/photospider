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

Inherit the [NUM baseline](NUM_common_contract.md) and [NUM-11 shared contract](NUM-11_reduction_contract.md). Count is discrete and exact in every profile.

`numeric.reduce_count_*` takes one Result containing exactly one tensor member at any key. Static `axes` chooses a nonempty set of distinct axes. The `values` output is an Int64 Result using schema `photospider.tensor` and member `samples`; it has the input rank, reduced extents set to one, ordinary axes and no attached domain description. Every group value is the product of the input extents on the reduced axes. Since input element count is at most 2^40, each count fits Int64.

Count uses static schema metadata and declares no input payload read. It records a schema-only reference, so upstream sample tasks are elided when no other consumer needs them. Complete input schema and axes validation still apply. Static shape/type metadata and axes determine output and are checked before execution; changes require a new Plan. Empty selection emits no task.

Each selected output tile is a separate dense Int64 allocation filled with the checked extent product. Computing the product takes O(rank) work during preparation; filling the selected output takes O(selected samples). Task arithmetic scratch is zero. Resource and cancellation failures discard unpublished storage and retain accepted owners through completion.

For input shape `[2,3,4]` and `axes="1,2"`, output shape is `[2,1,1]` and both groups contain 12. A shape `[2^20,2^20]` reduced along axis 1 yields `[2^20,1]` with every value equal to `2^20`. Selecting two tiles of extent 2 materializes four Int64 values in two allocations, totaling 32 payload bytes, while requiring no source sample storage. See [fixed reduction execution](../reductions-execution.md).
