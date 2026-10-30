---
spec_schema_version: 1
id: GRD-14A
parent_id: GRD-14
function: threshold
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.threshold
---

# GRD-14A: Fixed threshold

Inherits the [family contract](GRD-14_threshold_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.threshold` performs the transformation below.

## Interface

`feature: T[S,1]`, `threshold: Float64[1]` -> `mask: UInt8[S,1]`.

Suggested threshold=0.5 in the feature unit. Static keepdims:Bool=true; false removes the feature axis, provided rank remains positive.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Return 1 if x>=threshold and 0 otherwise. Compare in the common calculation dtype. Equality belongs to 1.

## Behavior

E support on the feature and threshold. The mask is binary data, with values 0/1.

## Acceptance

[0.25,0.5,0.75] at threshold 0.5 gives [0,1,1].

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
