---
spec_schema_version: 1
id: GRD-22A
parent_id: GRD-22
function: split_tone_opponent
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.split_tone_opponent
---

# GRD-22A: Tonal opponent-color tinting

Inherits the [family contract](GRD-22_split_tone_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.split_tone_opponent` performs the transformation below.

## Interface

`input: T[S,C]`, `zones: Float32/64[S,3]`, `colors: Float64[3,2]` [a,b], `strengths: Float64[3]` -> `values: T[S,C]`.

Select CIELAB or OKLab. Nonnegative zone weights have positive sum; strengths>=0, suggested zero. Colors use native opponent units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Copy l/L. Add sum_z(w_z*strength_z*color[z,c])/sum_z(w_z) to a and b. All-zero strengths copy. Colors supply additive tints and can color a neutral source.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

A one-hot shadow mask applies the shadow tint. Zero strength copies; nonzero tint can move neutral a=b=0 off the gray axis.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
