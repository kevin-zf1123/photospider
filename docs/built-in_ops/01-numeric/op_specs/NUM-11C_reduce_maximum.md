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
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: current working tree
---

# NUM-11C: reduce_maximum

Inherit the [NUM baseline](NUM_common_contract.md) and [NUM-11 shared contract](NUM-11_reduction_contract.md). The strict key follows the exact reference. Floating selection and special-value behavior are exact in all profiles.

`numeric.reduce_maximum_*` reduces complete groups along required static `axes`. Its single Result input contains exactly one tensor member at any key. The `values` Result uses schema `photospider.tensor` and member `samples`; it preserves input dtype and rank, replaces reduced extents with one, and drops facets. There is no dtype parameter.

Select the numeric maximum with infinities in ordinary extended order. Int64 comparison remains integer comparison, including values above 2^53. The first NaN in logical row-major order wins, preserving its sign and payload while setting the quiet bit. A singleton sNaN is quieted. For mixed signed zeros, return +0; same-sign zeros retain their sign.

For `input=[[1,2,3],[4,5,6]]` and `axes="1"`, the result is `[[3],[6]]` with shape `[2,1]`. For every nonempty Whole request, the kernel reads and validates the complete input and computes every output group before consumer projection. A typed-validation failure in an unrequested group still fails the request. Empty demand reads no payload and performs no comparisons. Current fixture coverage and limits are in [NUM-11 Whole execution](../reductions-whole.md).
