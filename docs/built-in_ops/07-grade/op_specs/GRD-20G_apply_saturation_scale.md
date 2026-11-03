---
spec_schema_version: 1
id: GRD-20G
parent_id: GRD-20
function: apply_saturation_scale
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.apply_saturation_scale
---

# GRD-20G: Apply an HSL/HSV saturation-scale field

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.apply_saturation_scale` performs the transformation below.

## Interface

`input: T[S,C]`, `scale: Float64[S,1]` -> `values: T[S,C]`.

Select HSL or HSV; source S in [0,1]; scale>=0.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

S_out=clamp(S*scale,0,1). Copy hue and L/V independently. scale=1 copies S.

### Current native algorithm/profile

The selected saturation is widened to binary64, multiplied by the matching dynamic scale and clamped to [0,1] before conversion to T. Scale one copies original saturation bits; hue and L/V copy independently. The required consumer unit is `modulation_unit="saturation_scale"`. Other current parameters and the native profile are specified by the [family](GRD-20_feature_curve_contract.md#current-parameters).

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

S=0 remains zero, S=0.75 with scale=2 becomes 1, and hue retains all windings.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
