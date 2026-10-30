---
spec_schema_version: 1
id: GRD-19C
parent_id: GRD-19
function: shadow_highlight_local
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.shadow_highlight_local
---

# GRD-19C: Local shadows, highlights and detail

Inherits the [family contract](GRD-19_shadow_highlight_local_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.shadow_highlight_local` performs the transformation below.

## Interface

`input: T[H,W,C]`, `controls: Float64[4]`, `edges: Float64[4]`, `detail_gain: Float64[1]` -> `values: T[H,W,C]`.

Select relative linear RGB or Gray. Base parameters are as GRD-19A; controls/edges as GRD-19B; detail_gain>=0 with suggested 1.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Extract linear Y in Float64, publish z=log2(max(Y,epsilon)) to Float64 and calculate the Float64 GRD-19A base b. Let e_base be the exposure from GRD-19B and e_detail=(detail_gain-1)*(z-b). Return x*exp2(e_base+e_detail). Identity controls with detail_gain=1 copy the input.

## Behavior

H(radius) through the luminance base plus matching source colors. Alpha/AOVs bypass. Effects are applied over the requested output independently of any later effect mask.

## Acceptance

Constant Y has zero detail. detail_gain=1 preserves the source log-detail contribution; detail_gain=0 removes it while applying the selected base exposure.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
