---
spec_schema_version: 1
id: NUM-10D
parent_id: NUM-10
function: scatter_sum
proposed_operation_keys:
  - array.scatter_sum_strict
  - array.scatter_sum_accelerated_apple_silicon
  - array.scatter_sum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-10D: scatter_sum

Inherit the [fixed scatter contract](NUM-10_scatter_contract.md) for ports, shape/dtype rules, unchecked index premises, task dependencies, numerical classification, scratch and lifetime. Each input Result contains one tensor; output `values` uses ordinary base axes in `photospider.tensor`/`samples`. Empty selection emits no tasks.

Sum base and all matching updates exactly, then perform one destination conversion. Integer final representability is an unchecked caller precondition; intermediate totals may exceed range. For base INT64_MAX and matching updates [1,-1], return INT64_MAX. Floating overflow produces infinity; finite cancellation is evaluated before rounding. No unordered floating atomic accumulation is used.

If no update hits a coordinate, copy base bits without arithmetic, including signaling NaNs. Every nonempty call constructs the stable index table once in a shared task without index validation. Output tasks compute only selected tiles and read the shared stable index table, their base region and all possible update positions for their non-axis region. Base remains immutable. Indices and normalized positions satisfy unchecked caller premises. Operational resource/cancellation failures prevent successful call publication.

For base `[10,20,30]`, indices `[1,1,2]`, updates `[2,3,4]` and axis 0, expect `[10,25,34]`. Temporary acceptance uses independent grouping and raw-bit or exact arithmetic references. Cover duplicate order, no-hit signaling NaNs, first-NaN precedence, infinities, zero signs, integer boundaries, partial/empty output, changing indices, source strides, precise read sets, artifacts and concurrent Runs. See [fixed indexing execution](../indexing-execution.md). Specification status remains Proposed.
