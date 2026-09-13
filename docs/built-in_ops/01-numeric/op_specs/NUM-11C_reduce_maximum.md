---
spec_schema_version: 1
id: NUM-11C
parent_id: NUM-11
function: reduce_maximum
proposed_operation_keys:
  - numeric.reduce_maximum_strict
  - numeric.reduce_maximum_accelerated_apple_silicon
  - numeric.reduce_maximum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-11C: reduce_maximum

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Reduce input groups selected by required static axes. Inherit the
[reduction contract](NUM-11_reduction_contract.md) for input/values ports,
positive rank-1..8 shapes, size cap, fixed keepdims=true, generic output facets,
full selected-group support, strided reads, NaN priority/payload conversion,
invalidation, returned mapping, resources, errors and lifetime. Input supports
UInt8, Int64, Float32 and Float64. All three CPU profiles are bitwise equivalent.

## Type and numeric semantics

Output preserves input dtype with no dtype parameter. Select the numerical
maximum of the group, treating infinities in the ordinary extended order.
Use exact integer comparison, including Int64 values beyond 2^53. Propagate the
first NaN by the shared rule, without ignoring it. On a singleton group, sNaN
is still quieted; this is a numeric reducer, not a copying shortcut.

For mixed zeros, return +0; same-sign zeros retain their sign. Nonzero
ties have the same representable value, so no rounding tolerance is needed.

## Acceptance and implementation distinction

Conceptual fixture: input=[[1,2,3],[4,5,6]], axes="1" -> [[3],[6]], shape
[2,1], in the selected output dtype. Bind through WorkflowDocument, compile the
selected key and read values through ExecutionContext when implemented. Use
independent exact grouping and integer/rational/bit-selection oracles. Cover
singleton groups, non-leading/multiple axes, NaN payload order/conversion,
signed-zero groups, infinity combinations, subnormals and source dtype extrema.

Apply shared exact source-read/invalidation, resource/cancellation and owner
lifetime fixtures. Existing legacy mean/variance behavior is distinguished in
the shared contract. These versioned target keys remain unimplemented and no
runtime test is claimed by this specification.
