---
spec_schema_version: 1
id: GRD-12A
parent_id: GRD-12
function: monochrome_weights
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.monochrome_weights
---

# GRD-12A: Artistic monochrome feature

Inherits the [family contract](GRD-12_monochrome_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.monochrome_weights` performs the transformation below.

## Interface

`input: T[S,C]`, `weights: Float64[3]`, `bias: Float64[1]` -> `values: T[S,1]`.

Select RGB; signed weights are legal. Static normalize=false by default; normalize=true requires a nonzero weight sum. Suggested bias=0; weights are required.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

v=sum(w_c*x_c)/d+bias, with d=sum(w) when normalize=true and d=1 otherwise. Output is an artistic scalar feature in the declared RGB coordinate system. It contains only this feature; complete-image gray rendering uses GRD-12B/C/D or explicit assembly.

## Behavior

Each feature uses the selected RGB tuple. The output has a new single feature axis and contains no copied alpha/AOV slots.

## Acceptance

Weights (1,0,0), bias=0 select red. Weights (1,1,1) with normalize=true return the channel average.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
