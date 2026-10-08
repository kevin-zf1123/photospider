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
repository_commit: current working tree
---

# NUM-11D: reduce_mean

Inherit the [NUM baseline](NUM_common_contract.md) and [NUM-11 shared contract](NUM-11_reduction_contract.md). The strict key follows the exact reference; accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md), including its range and fallback rules.

`numeric.reduce_mean_*` reduces complete groups along required static `axes`. Each Result input has one tensor member at any key. The `values` output is a `photospider.tensor` Result with member `samples`, rank-preserving keepdims shape and empty facets. Input accepts UInt8, Int64, Float32 and Float64. Required static `dtype` is Float32 or Float64; authoring helpers default to Float64. Integer inputs participate exactly and are not first converted to floating point.

For group size N, calculate the exact total divided by N, then round once to the selected floating destination. Do not round the total or mean at an intermediate step. The first NaN in logical row-major order wins under the shared sign/payload conversion. With no NaN, opposite signed infinities produce the fixed positive quiet NaN; otherwise infinity determines its signed result. For a finite exact zero, return -0 only when every input is -0, otherwise +0. A singleton sNaN is quieted. Narrowing may yield signed infinity; this is a successful numerical result.

For `input=[[1,2,3],[4,5,6]]` and `axes="1"`, Float64 output is `[[2],[5]]`. For Int64 `[2^53+1,2^53+2]`, compute the exact mean before Float64 rounding; pre-casting elements would produce the wrong result. For finite Float64 `[MAX,MAX]`, the mean is MAX despite a naive sum overflow.

Nonempty requests read and validate the complete input, compute every group and publish the complete output before projection. An unrequested typed failure still fails Whole execution. Empty demand reads no payload or performs no arithmetic. Current fixture coverage and limits are in [NUM-11 Whole execution](../reductions-whole.md).
