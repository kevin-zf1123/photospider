---
spec_schema_version: 1
id: GRD-03A
parent_id: GRD-03
function: contrast_linear
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.contrast_linear
---

# GRD-03A: Linear contrast around a pivot

Inherits the [family contract](GRD-03_contrast_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.contrast_linear` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[Csel,2]` with rows [contrast,pivot] -> `values: T[S,C]`.

contrast>=0; suggested [1,0.18]. Pivot uses the source coordinate unit.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

The operation key and current selector encoding follow the [family parameter profile](GRD-03_contrast_contract.md#current-parameters). The image and control tensors are required dynamic inputs.

## Mathematics

`y=pivot+(x-pivot)*contrast`. contrast=1 copies x; contrast=0 selects the pivot. Results are unclipped.

### Current native algorithm/profile

Selected values are evaluated as `fma(contrast, Float64(x)-pivot, pivot)` and converted once to T. `contrast=1` and unselected components copy original bytes; `contrast=0` selects the binary64 pivot before the final conversion. This is an unclipped native binary64 algorithm under the common floating acceptance profile. Fused arithmetic is permitted; it does not impose strict whole-expression rounding.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

With pivot=0.25 and contrast=2, x=0.5 gives 0.75; x=pivot remains fixed.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
