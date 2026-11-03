---
spec_schema_version: 1
id: GRD-20D
parent_id: GRD-20
function: apply_chroma_scale
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.apply_chroma_scale
---

# GRD-20D: Apply a chroma-scale field

Inherits the [family contract](GRD-20_feature_curve_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.apply_chroma_scale` performs the transformation below.

## Interface

`input: T[S,C]`, `scale: Float64[S,1]` -> `values: T[S,C]`.

Select CIELAB or OKLab; scale>=0, expressed as a dimensionless chroma multiplier.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Copy l/L; multiply a and b by scale. scale=1 copies the corresponding tuple.

### Current native algorithm/profile

The selected a and b coordinates are independently widened to binary64, multiplied by their matching dynamic scale, and converted once to T. Scale one copies original bits; l/L and other components copy independently. The required consumer unit is `modulation_unit="chroma_scale"`. Other current parameters and the native numerical profile are specified by the [family](GRD-20_feature_curve_contract.md#current-parameters).

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

scale=0 yields zero a,b. Scaling (a,b)=(3,4) by 2 gives (6,8).

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
