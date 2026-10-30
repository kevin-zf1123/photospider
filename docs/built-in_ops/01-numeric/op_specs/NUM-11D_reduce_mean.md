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
---

# NUM-11D: reduce_mean

Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-11 shared contract](NUM-11_reduction_contract.md). The strict key follows the exact reference; accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md).

## Interface

`numeric.reduce_mean_*` reduces complete groups along required static `axes`. Each Result input has one tensor member at any key. The `values` output is a `photospider.tensor` Result with member `samples`, rank-preserving keepdims shape and no attached domain description. Input accepts UInt8, Int64, Float32 and Float64. Required static `dtype` is Float32 or Float64; authoring helpers default to Float64.

## Mathematics

Integer inputs participate exactly and are not first converted to floating point. For group size N, the exact total divided by N is rounded once to the selected floating destination; neither the total nor the mean is rounded at an intermediate step. The first NaN in logical row-major order wins under the shared sign/payload conversion. With no NaN, opposite signed infinities produce the fixed positive quiet NaN; otherwise infinity determines its signed result. For a finite exact zero, -0 is returned only when every input is -0, otherwise +0. A singleton sNaN is quieted. Narrowing may yield signed infinity; this is a successful numerical result.

## Behavior

Each output sample depends on every input sample of its group. Empty selection reads nothing.

## Acceptance

`input=[[1,2,3],[4,5,6]]`, `axes="1"` gives Float64 `[[2],[5]]`. Int64 `[2^53+1,2^53+2]`: the exact mean is computed before Float64 rounding (pre-casting elements gives the wrong result). Finite Float64 `[MAX,MAX]` has mean MAX despite a naive sum overflow.
