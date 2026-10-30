---
spec_schema_version: 1
id: NUM-11G
parent_id: NUM-11
function: reduce_std
proposed_operation_keys:
  - numeric.reduce_std_strict
  - numeric.reduce_std_accelerated_apple_silicon
  - numeric.reduce_std_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-11G: reduce_std

Inherits the [NUM baseline](NUM_common_contract.md), the [NUM-11 shared contract](NUM-11_reduction_contract.md) and [reduce_variance](NUM-11F_reduce_variance.md) for input dtype, axes, `ddof`, shape and NaN conversion. The strict key follows the exact reference; accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md).

## Interface

`numeric.reduce_std_*` accepts a single-member Result with UInt8, Int64, Float32 or Float64 input. The `values` output is a Float32/Float64 `photospider.tensor` Result member `samples`, with rank-preserving keepdims shape. Static `dtype` and `ddof` are required on direct nodes; authoring helpers default to Float64 and zero.

## Mathematics

For finite data, let V be the exact rational variance defined by the variance contract; the result is `RN_dtype(sqrt(V))`. The mean and variance are not rounded before the square root. A finite constant group yields +0; finite positive results may round to a subnormal, +0 or +Inf. The first NaN follows variance's row-major payload rule. An infinity without a NaN yields the fixed positive quiet NaN before root evaluation.

## Behavior

Each output sample depends on every input sample of its group. Empty selection reads nothing.

## Acceptance

Float64 `[MAX,-MAX]` with `ddof=0` gives a standard deviation of exactly MAX even though the variance rounds to infinity; this distinguishes direct exact-root rounding from variance-then-sqrt.
