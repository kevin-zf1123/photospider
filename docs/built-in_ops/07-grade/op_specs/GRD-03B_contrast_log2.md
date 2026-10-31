---
spec_schema_version: 1
id: GRD-03B
parent_id: GRD-03
function: contrast_log2
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.contrast_log2
---

# GRD-03B: Contrast on the exposure axis

Inherits the [family contract](GRD-03_contrast_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.contrast_log2` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[Csel,2]` with rows [contrast,pivot] -> `values: T[S,C]`.

Linear RGB or linear-Y Gray; x>=0; contrast>0 and pivot>0. Suggested [1,0.18].

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The operation key and current selector encoding follow the [family parameter profile](GRD-03_contrast_contract.md#current-parameters). The image and control tensors are required dynamic inputs.

## Mathematics

For x>0, `y=pivot*exp2(contrast*log2(x/pivot))`. Zero copies its sign; contrast=1 copies x.

### Current native algorithm/profile

The current native profile evaluates the published function in binary64, using equivalent forms `exp2(contrast*log2(x)) * exp2((1-contrast)*log2(pivot))` and `exp2(contrast*log2(x)+(1-contrast)*log2(pivot))` where their intermediate ranges are suitable. Binary64 range reduction and polynomial evaluation use the common continuous acceptance profile; exceptional ranges use ordinary library powers, logarithms and exponentials. The scale may be prepared as `pow(pivot,1-contrast)`; normal library power and scale intermediates may be multiplied directly before resorting to a logarithmic ratio. The scalar ratio form uses `log1p((x-pivot)/pivot)/ln(2)` near the pivot and combines the ratio exponent with `log2(pivot)` when a separate ratio or exponential is outside its ordinary range. Zero and `contrast=1` copy the source sign/bits as specified; unselected components copy original bytes. The calculation value is converted once to T.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

pivot=0.25, contrast=2, x=0.5 gives 1. Zero remains zero.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
