---
spec_schema_version: 1
id: NUM-11B
parent_id: NUM-11
function: reduce_minimum
proposed_operation_keys:
  - numeric.reduce_minimum_strict
  - numeric.reduce_minimum_accelerated_apple_silicon
  - numeric.reduce_minimum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-11B: reduce_minimum

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
minimum of the group, treating infinities in the ordinary extended order.
Use exact integer comparison, including Int64 values beyond 2^53. Propagate the
first NaN by the shared rule, without ignoring it. On a singleton group, sNaN
is still quieted; this is a numeric reducer, not a copying shortcut.

For mixed zeros, return -0; same-sign zeros retain their sign. Nonzero
ties have the same representable value, so no rounding tolerance is needed.

## Acceptance and implementation distinction

Fixture: input=[[1,2,3],[4,5,6]], axes="1" -> [[1],[4]], shape
[2,1], in the selected output dtype. The public manual target binds this through
WorkflowDocument, compiles the selected key and reads values through ExecutionContext. Use
independent exact grouping and integer/rational/bit-selection oracles. Cover
singleton groups, non-leading/multiple axes, NaN payload order/conversion,
signed-zero groups, infinity combinations, subnormals and source dtype extrema.

The formal keys execute Whole and preserve the numerical rules above. See
[NUM-11 Whole execution](../reductions-whole.md) for current public workflow,
validation and timing. Earlier regional platform records predate Whole.
