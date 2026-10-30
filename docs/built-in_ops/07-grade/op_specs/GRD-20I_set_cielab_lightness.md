---
spec_schema_version: 1
id: GRD-20I
parent_id: GRD-20
function: set_cielab_lightness
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.set_cielab_lightness
---

# GRD-20I: Set CIELAB lightness while retaining a and b

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.set_cielab_lightness` performs the transformation below.

## Interface

`input: T[S,C]`, `target: Float64[S,1]` -> `values: T[S,C]`.

Select relative linear RGB with an explicit basis. Target unit and conversion route are `cielab_l` in the common [brightness routes](GRD_common_contract.md#brightness-routes).

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Convert the original RGB through the `cielab_l` route in Float64, replace only the designated coordinate by target, retain the other two coordinates, then convert back. When target equals the extracted coordinate, copy the original RGB.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

An unchanged target is bit-preserving identity. Verify retention of the other two model coordinates before inverse conversion; allow floating tolerance in a round trip.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
