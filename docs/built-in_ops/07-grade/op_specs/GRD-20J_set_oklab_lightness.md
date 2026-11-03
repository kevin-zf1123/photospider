---
spec_schema_version: 1
id: GRD-20J
parent_id: GRD-20
function: set_oklab_lightness
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.set_oklab_lightness
---

# GRD-20J: Set OKLab lightness while retaining a and b

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.set_oklab_lightness` performs the transformation below.

## Interface

`input: T[S,C]`, `target: Float64[S,1]` -> `values: T[S,C]`.

Select relative linear RGB with an explicit basis. Target unit and conversion route are `oklab_l` in the common [brightness routes](GRD_common_contract.md#brightness-routes).

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Convert the original RGB through the `oklab_l` route in Float64, replace only the designated coordinate by target, retain the other two coordinates, then convert back. When target equals the extracted coordinate, copy the original RGB.

### Current native algorithm/profile

The current native algorithm composes the working-white-to-D65 matrices and reconstructs through the difference of LMS cubes. Its identity neighborhood uses the absolute sum of the individual lightness terms before cancellation and reevaluates the scalar extraction sequence. Equations, current parameters and basis interpretation are specified by the [family profile](GRD-20_feature_curve_contract.md#current-native-mathematical-profile).

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

An unchanged target is bit-preserving identity. Verify retention of the other two model coordinates before inverse conversion; allow floating tolerance in a round trip.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
