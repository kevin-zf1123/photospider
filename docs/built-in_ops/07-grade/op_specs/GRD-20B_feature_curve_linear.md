---
spec_schema_version: 1
id: GRD-20B
parent_id: GRD-20
function: feature_curve_linear
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.feature_curve_linear
---

# GRD-20B: Nonperiodic feature curve

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.feature_curve_linear` performs the transformation below.

## Interface

`feature: T[S,1]`, `knots: Float64[K]`, `ordinates: Float64[K]` -> `modulation: Float64[S,1]`.

2<=K<=65536; knots strictly increase. Required modulation_unit as GRD-20A. edge=reject|clamp, default clamp. Scale ordinates are nonnegative.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Apply the linear interpolation and endpoint rule of GRD-05A with Float64 output. The declared modulation unit is a static consumer parameter, not tensor metadata.

### Current native algorithm/profile

This operation uses the true-interval linear interpolation, exact-knot selection and endpoint policy in the [current family mathematical profile](GRD-20_feature_curve_contract.md#current-native-mathematical-profile). For `chroma_scale` and `saturation_scale`, ordinary interpolation anchors at the smaller ordinate as specified by that profile, preserving nonnegative finite results and stored knot/edge zero signs. The current parameters and selector encoding are specified in the [family parameter profile](GRD-20_feature_curve_contract.md#current-parameters).

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

Knots [0,1], ordinates [1,2] map 0.25 to 1.25. Clamped endpoints select stored ordinates.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
