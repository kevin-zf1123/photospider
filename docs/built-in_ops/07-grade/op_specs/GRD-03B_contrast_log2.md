---
spec_schema_version: 1
id: GRD-03B
parent_id: GRD-03
function: contrast_log2
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
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

## Mathematics

For x>0, `y=pivot*exp2(contrast*log2(x/pivot))`. Zero copies its sign; contrast=1 copies x.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

pivot=0.25, contrast=2, x=0.5 gives 1. Zero remains zero.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
