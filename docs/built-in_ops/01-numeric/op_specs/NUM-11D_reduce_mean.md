---
spec_schema_version: 1
id: NUM-11D
parent_id: NUM-11
function: reduce_mean
proposed_operation_keys:
  - numeric.reduce_mean_strict
  - numeric.reduce_mean_accelerated_apple_silicon
  - numeric.reduce_mean_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-11D: reduce_mean

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

Required static String dtype is float32/float64, constructor default float64.
Direct nodes supply it. Integer source values participate exactly without a
preliminary floating cast. Compute the mathematical exact sum divided by positive
group count N, with one final correctly rounded destination conversion. Do not
round the sum or mean at intermediate steps. Float64 sources narrowed to Float32
may overflow the output; return the correctly signed infinity successfully.

For nonfinite groups, first apply shared NaN priority/conversion. With no NaN,
opposite signed infinities yield the fixed positive quiet NaN; otherwise any
infinity determines its signed result. For finite exact-zero results, return -0
only when every source element is -0, otherwise +0. A singleton group still
applies these numeric and dtype-conversion rules, including sNaN quieting.

## Acceptance and implementation distinction

Fixture: input=[[1,2,3],[4,5,6]], axes="1" -> [[2],[5]], shape
[2,1], in the selected output dtype. The public manual target binds this through
WorkflowDocument, compiles the selected key and reads values through ExecutionContext. Use
independent exact grouping and integer/rational/bit-selection oracles. Cover
singleton groups, non-leading/multiple axes, NaN payload order/conversion,
signed-zero groups, infinity combinations, subnormals and source dtype extrema.

For Int64 [2^53,2^53+1,2^53+2], evaluate the exact rational mean before
rounding; no preliminary float conversion is allowed. Also test Int64
[2^53+1,2^53+2]: the correctly rounded Float64 mean is 2^53+2; casting both
inputs first would incorrectly yield 2^53. For Float64 [MAX,MAX],
mean is MAX despite an overflowing naive floating sum. Test both destinations
and the shared deterministic NaN payload narrowing/expansion.

The current three profile keys use `reduce_mean_node` from
`photospider/numeric/reductions.hpp`. Integer sources remain exact through the
sum/count calculation and convert only at final Float32/Float64 rounding. The
public fixture checks the `[2,3]` axes-`1` result `[[2],[5]]` and large-integer
no-premature-conversion cases. The shared reduction contract records the
complete strict/Apple/WSL and installed-consumer evidence. Proposed status is
unchanged.
