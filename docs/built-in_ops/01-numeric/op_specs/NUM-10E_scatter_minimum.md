---
spec_schema_version: 1
id: NUM-10E
parent_id: NUM-10
function: scatter_minimum
proposed_operation_keys:
  - array.scatter_minimum_strict
  - array.scatter_minimum_accelerated_apple_silicon
  - array.scatter_minimum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-10E: scatter_minimum

Inherit the [fixed scatter contract](NUM-10_scatter_contract.md) for ports, shape/dtype rules, unchecked index premises, task dependencies, numerical classification, scratch and lifetime. Each input Result contains one tensor; output `values` uses ordinary base axes in `photospider.tensor`/`samples`. Empty selection emits no tasks.

Select the numerical minimum of base and matching updates. The first source NaN is quieted with its payload preserved. Mixed signed zeros select -0. Integer comparisons remain integer comparisons.

If no update hits a coordinate, copy base bits without arithmetic, including signaling NaNs. Every nonempty call constructs the stable index table once in a shared task without index validation. Output tasks compute only selected tiles and read the shared stable index table, their base region and all possible update positions for their non-axis region. Base remains immutable. Indices and normalized positions satisfy unchecked caller premises. Operational resource/cancellation failures prevent successful call publication.

For base `[10,20,30]`, indices `[1,1,2]`, updates `[2,3,4]` and axis 0, expect `[10,2,4]`. Temporary acceptance uses independent grouping and raw-bit or exact arithmetic references. Cover duplicate order, no-hit signaling NaNs, first-NaN precedence, infinities, zero signs, integer boundaries, partial/empty output, changing indices, source strides, precise read sets, artifacts and concurrent Runs. See [fixed indexing execution](../indexing-execution.md). Specification status remains Proposed.
