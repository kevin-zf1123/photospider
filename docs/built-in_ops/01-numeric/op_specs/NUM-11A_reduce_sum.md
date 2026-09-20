---
spec_schema_version: 1
id: NUM-11A
parent_id: NUM-11
function: reduce_sum
proposed_operation_keys:
  - numeric.reduce_sum_strict
  - numeric.reduce_sum_accelerated_apple_silicon
  - numeric.reduce_sum_accelerated_x86_64
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

# NUM-11A: reduce_sum

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
UInt8, Int64, Float32 and Float64. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound.

## Type and numeric semantics

Required static String dtype selects output: integer inputs permit uint8/int64,
floating inputs float32/float64. Constructors default to int64 for integer input
and float64 for floating input. Direct nodes supply dtype. No implicit crossing
between these integer/floating domains is allowed.

Sum every finite group element exactly. Perform only a final UInt8/Int64 range
check or correctly rounded floating destination conversion. Intermediate partial
sums outside the destination range do not cause failure. Integer final overflow
uses the shared ArithmeticOverflow failure; floating final overflow yields signed
infinity, and gradual underflow follows exact result sign.

For nonfinite groups, first apply shared NaN priority/conversion. With no NaN,
opposite signed infinities yield the fixed positive quiet NaN; otherwise any
infinity determines its signed result. For finite exact-zero results, return -0
only when every source element is -0, otherwise +0. A singleton group still
applies these numeric and dtype-conversion rules, including sNaN quieting.

## Acceptance and implementation distinction

Fixture: input=[[1,2,3],[4,5,6]], axes="1" -> [[6],[15]], shape
[2,1], in the selected output dtype. The public manual target binds this through
WorkflowDocument, compiles the selected key and reads values through ExecutionContext. Use
independent exact grouping and integer/rational/bit-selection oracles. Cover
singleton groups, non-leading/multiple axes, NaN payload order/conversion,
signed-zero groups, infinity combinations, subnormals and source dtype extrema.

For Float64, [MAX,MAX,-MAX] must return MAX without intermediate overflow.
For Int64, [INT64_MAX,1,-1] must return INT64_MAX. Explicit UInt8 output requires
final range validation independently of the integer default. Test each supported
destination and rejection of cross-domain dtype selection.

The current three profile keys use `reduce_sum_node` from
`photospider/numeric/reductions.hpp`. They stream selected groups in at most
64-value windows and use exact aggregate state with one final destination
conversion. The public fixture checks `[[1,2,3],[4,5,6]]`, axes `1`, producing
`[[6],[15]]`, plus dtype selection, overflow and exceptional values. The shared
reduction contract records the complete strict/Apple/WSL and installed-consumer
evidence. Proposed status is unchanged.
