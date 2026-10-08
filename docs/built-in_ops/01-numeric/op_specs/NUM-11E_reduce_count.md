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

`numeric.reduce_count_*` takes one Result containing exactly one tensor member at any key. Static `axes` chooses a nonempty set of distinct axes. The `values` output is an Int64 Result using schema `photospider.tensor` and member `samples`; it has the input rank, reduced extents set to one, ordinary axes and empty facets. Every group value is the product of the input extents on the reduced axes. Since input element count is at most 2^40, each count fits Int64.

Count reads only static schema metadata. It requests no runtime input ports, performs no sample-data or typed-payload validation, and does not schedule source sample producers solely to calculate count. Complete input schema and axes validation still apply. Byte-only edits do not invalidate count. Static schema shape/type metadata and axes determine its output and are checked during specialization and seal; changing them requires a newly specialized and sealed plan. The empty runtime input projection does not subscribe to a Descriptor Need. Empty demand publishes no values and does no runtime source work.

The kernel calculates the checked extent product in O(rank), allocates one 8-byte Int64 owner and publishes the complete keepdims shape with zero strides. A dense physical output is not required. Its output mapping, coverage and lifetime follow the shared immutable Result rules. Resource, work and cancellation failures retain host statuses and release unpublished storage.

For input shape `[2,3,4]` and `axes="1,2"`, output shape is `[2,1,1]` and both groups contain 12. For full reduction, output keeps rank with every extent set to one. The current fixture verifies count from a `2^40`-element schema whose upstream producer would fail if started. The producer remains unstarted, source observations and association are empty, the `[2^20,1]` output reads `2^20` at both ends, and the output adds exactly 8 payload bytes. First and last values share a pointer; the result remains readable after context retirement and releases the 8-byte backing with its last owner. See [NUM-11 Whole execution](../reductions-whole.md) for the test command and wider evidence boundary.
