---
spec_schema_version: 1
id: GRD-08B
parent_id: GRD-08
function: hsv
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.hsv
---

# GRD-08B: Native HSV adjustment

Inherits the [family contract](GRD-08_hsl_hsv_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.hsv` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[3]` [delta_h,saturation_scale,value_scale] -> `values: T[S,C]`.

Select native HSV; S,V in [0,1]; scales>=0. Suggested [0,1,1]. Required hue_unit=radian|pi_multiple.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The operation key and selector encoding follow the [family parameter profile](GRD-08_hsl_hsv_contract.md#current-parameters). Both tensors are dynamic inputs; controls retain the Float64 shape and row order in this signature.

## Mathematics

When S>0, h_out=h+delta_h; otherwise copy h. S_out=clamp(S*saturation_scale,0,1); V_out=clamp(V*value_scale,0,1). Stored hue retains winding. Identity component controls copy.

### Current native algorithm/profile

Consumed source coordinates are widened to Float64, evaluated with the stated addition or multiplication, and converted once to the image dtype. Clamping occurs only where required by the member formula. Hue gating reads the original saturation or chroma, including when its scale is zero. Identity controls, achromatic hue and unselected components copy original bytes. Hue increments retain their declared unit and winding under the common native floating profile.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

V=0.25 with scale=2 gives 0.5. Gray hue is unchanged.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
