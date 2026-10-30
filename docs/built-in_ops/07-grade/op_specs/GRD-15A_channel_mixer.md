---
spec_schema_version: 1
id: GRD-15A
parent_id: GRD-15
function: channel_mixer
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.channel_mixer
---

# GRD-15A: Matrix channel mixer

Inherits the [family contract](GRD-15_channel_mixer_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.channel_mixer` performs the transformation below.

## Interface

`input: T[S,C]`, `matrix: Float64[D,Csel]`, `bias: Float64[D]` -> `values: T[S,D]`.

1<=Csel,D<=64. Explicit ordered source slots and destination component roles. Matrix and bias are required; identity and zero are suggested when Csel=D.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

y_j=sum_c M[j,c]*x_c+bias[j]. Output contains exactly the D calculated channels in row order. An exact one-hot row with zero bias selects the corresponding source value.

## Behavior

Each output row depends on its selected source tuple and coefficient row. Alpha or AOV inclusion requires explicit rows or separate assembly.

## Acceptance

The R/B permutation matrix swaps channels. A row [0.5,0.5,0] averages R and G.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
