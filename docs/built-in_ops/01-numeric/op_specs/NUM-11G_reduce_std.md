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
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-11G: reduce_std

Inherit the [NUM baseline](NUM_common_contract.md), [NUM-11 shared contract](NUM-11_reduction_contract.md) and [reduce_variance](NUM-11F_reduce_variance.md) for input dtype, axes, `ddof`, shape, NaN conversion, resources and failure behavior. The strict key follows the exact reference; accelerated floating results follow the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md).

`numeric.reduce_std_*` accepts a single-member Result with UInt8, Int64, Float32 or Float64 input. The `values` output is a Float32/Float64 `photospider.tensor` Result member `samples`, with rank-preserving keepdims shape. Static `dtype` and `ddof` are required on direct nodes; authoring helpers default to Float64 and zero.

For finite data, let V be the exact rational variance defined by the variance contract, then return `RN_dtype(sqrt(V))`. Do not round the mean or variance before the square root. A finite constant group yields +0; finite positive results may round to a subnormal, +0 or +Inf. The first NaN follows variance's row-major payload rule. An infinity without a NaN yields the fixed positive quiet NaN before root evaluation.

For Float64 `[MAX,-MAX]` with `ddof=0`, standard deviation is exactly MAX even though variance rounds to infinity. This distinguishes direct exact-root rounding from a variance-then-sqrt implementation. The exact moments and root refinement are charged to host work and memory budgets; if the final rounding cannot be established within the budget, return the host resource failure. Nonempty Whole demand processes all input values and groups before projection; Empty reads no payload. Current fixture evidence and limits are in [NUM-11 Whole execution](../reductions-whole.md).
