---
spec_schema_version: 1
id: GRD-20A
parent_id: GRD-20
function: feature_curve_periodic
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.feature_curve_periodic
---

# GRD-20A: Periodic feature curve

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.feature_curve_periodic` performs the transformation below.

## Interface

`feature: T[S,1]`, `knots: Float64[K]`, `ordinates: Float64[K]` -> `modulation: Float64[S,1]`.

2<=K<=65536. Required static period:Float64>0 and modulation_unit=delta_h|chroma_scale|saturation_scale|delta_lightness|ev. knots[0]=0 and knots strictly increase below period. Scale ordinates are nonnegative.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Reduce f modulo period into [0,period), then interpolate linearly including the closing segment from the last knot to (period,ordinates[0]). delta_h values are ordinary unwrapped increments. A hue in pi_multiple units has period 2.

### Current native algorithm/profile

This operation uses the periodic reduction, closing segment and exact-knot interpolation in the [current family mathematical profile](GRD-20_feature_curve_contract.md#current-native-mathematical-profile). For `chroma_scale` and `saturation_scale`, ordinary interpolation anchors at the smaller ordinate as specified by that profile, preserving nonnegative finite results and stored knot/edge zero signs. The current parameters and selector encoding are specified in the [family parameter profile](GRD-20_feature_curve_contract.md#current-parameters).

## Behavior

Each output uses the source query at that coordinate and the contributing control-table entries. The complete auxiliary table is an admitted static support superset. A source edit is local; table edits affect observations whose declared support includes the table. Empty and bypass-only requests have no table payload dependency.

## Acceptance

The seam is continuous. Queries f and f+period agree within floating tolerance. Constant ordinates produce constant modulation.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
