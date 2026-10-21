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
---

# NUM-11B: reduce_minimum

Inherit the [NUM baseline](NUM_common_contract.md) and [NUM-11 shared contract](NUM-11_reduction_contract.md). The strict key follows the exact reference. Floating selection and special-value behavior are exact in all profiles.

`numeric.reduce_minimum_*` reduces complete groups along required static `axes`. Its single Result input contains exactly one tensor member at any key. The `values` Result uses schema `photospider.tensor` and member `samples`; it preserves input dtype and rank, replaces reduced extents with one, and carries no domain description. There is no dtype parameter.

Select the numeric minimum with infinities in ordinary extended order. Int64 comparison remains integer comparison, including values above 2^53. The first NaN in logical row-major order wins, preserving its sign and payload while setting the quiet bit. A singleton sNaN is quieted. For mixed signed zeros, return -0; same-sign zeros retain their sign.

For `input=[[1,2,3],[4,5,6]]` and `axes="1"`, the result is `[[1],[4]]` with shape `[2,1]`. Each selected output tile lowers to a fixed CPU task with all source tiles needed for its complete groups. The task uses private preallocated state and preserves logical group order across physical tile boundaries. Empty selection emits no task. See [fixed reduction execution](../reductions-execution.md).
