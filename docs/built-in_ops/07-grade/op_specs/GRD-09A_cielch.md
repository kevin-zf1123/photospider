---
spec_schema_version: 1
id: GRD-09A
parent_id: GRD-09
function: cielch
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.cielch
---

# GRD-09A: CIELCh adjustment

Inherits the [family contract](GRD-09_lch_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.cielch` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[3]` [delta_l,chroma_scale,delta_h] -> `values: T[S,C]`.

Select CIELCh; l=L*/100 and native CIE C*; C>=0, chroma_scale>=0. Suggested [0,1,0]. Required hue_unit=radian|pi_multiple.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The operation key and selector encoding follow the [family parameter profile](GRD-09_lch_contract.md#current-parameters). Both tensors are dynamic inputs; controls retain the Float64 shape and row order in this signature.

## Mathematics

L_out=L+delta_l; C_out=C*chroma_scale. If C>0, h_out=h+delta_h; otherwise copy h. Lightness is unclipped and hue retains winding. Identity component controls copy.

### Current native algorithm/profile

Consumed source coordinates are widened to Float64, evaluated with the stated addition or multiplication, and converted once to the image dtype. Clamping occurs only where required by the member formula. Hue gating reads the original saturation or chroma, including when its scale is zero. Identity controls, achromatic hue and unselected components copy original bytes. Hue increments retain their declared unit and winding under the common native floating profile.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Chroma scale zero makes C zero. A lightness increment of 0.1 changes only the first native coordinate.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
