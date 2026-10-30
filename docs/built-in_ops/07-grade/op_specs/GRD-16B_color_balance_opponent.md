---
spec_schema_version: 1
id: GRD-16B
parent_id: GRD-16
function: color_balance_opponent
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.color_balance_opponent
---

# GRD-16B: Tonal opponent-color balance

Inherits the [family contract](GRD-16_color_balance_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.color_balance_opponent` performs the transformation below.

## Interface

`input: T[S,C]`, `zones: Float32/64[S,3]`, `offsets: Float64[3,2]` -> `values: T[S,C]`.

Select CIELAB or OKLab. Zone conventions are as GRD-16A. Suggested offsets are zero, in native a/b units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Copy the native l/L coordinate. Add normalized weighted offsets to a and b using GRD-16A. This preserves stored model lightness.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

A nonzero a offset leaves l/L and b unchanged. Zero offsets preserve all source bits.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
