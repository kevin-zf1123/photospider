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
repository_commit: current working tree
---

# NUM-11A: reduce_sum

Inherit the [NUM baseline](NUM_common_contract.md) and [NUM-11 shared contract](NUM-11_reduction_contract.md). The strict key follows the exact reference; accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules. Integer results and copies remain exact.

`numeric.reduce_sum_*` reduces complete input groups selected by required static `axes`. Each input is a Result with exactly one tensor member at any member key. The `values` output is a Result using `photospider.tensor` / `samples`, with rank-preserving keepdims shape, ordinary axes and empty facets. Input supports UInt8, Int64, Float32 and Float64. Static `dtype` selects UInt8 or Int64 for integer input and Float32 or Float64 for floating input. The authoring helper defaults to Int64 and Float64, respectively; integer/floating domain conversion is rejected.

## Numeric semantics

Sum all elements of a group exactly. Convert or range-check only the final total. Intermediate partial sums outside the destination range do not fail if the exact final total is representable. Integer final overflow is a Domain/Run `OperationFailed` / `ArithmeticOverflow` with the failing linear output group index. Floating overflow returns signed infinity; underflow follows the exact sign.

The first NaN in logical row-major order wins and is converted by the shared payload rule. With no NaN, opposite signed infinities produce the fixed positive quiet NaN; otherwise an infinity determines its signed result. Exact zero is negative zero only if every contributor is negative zero, and positive zero otherwise. Singleton groups still apply numeric conversion and quiet an sNaN.

For `input=[[1,2,3],[4,5,6]]` and `axes="1"`, the output is `[[6],[15]]` with shape `[2,1]`. Exact wide cancellation cases include Float64 `[MAX,MAX,-MAX] -> MAX` and Int64 `[INT64_MAX,1,-1] -> INT64_MAX`. UInt8 output must check only the final result; `[100,155]` over axis 0 produces 255.

For nonempty demand the Whole program reads and validates the complete input, computes every group and publishes the complete dense output before projection. An unrequested group's integer overflow or input validation failure fails the selected result. Empty demand reads no payload and performs no arithmetic. Current public fixture coverage and limits are summarized in [NUM-11 Whole execution](../reductions-whole.md).
