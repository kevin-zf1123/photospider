---
spec_schema_version: 1
id: GRD-19B
parent_id: GRD-19
function: local_base_grade
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.local_base_grade
---

# GRD-19B: Adjust a supplied local tonal base

Inherits the [family contract](GRD-19_shadow_highlight_local_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.local_base_grade` performs the transformation below.

## Interface

`input: T[S,C]`, `base: Float64[S,1]`, `controls: Float64[4]` [pivot_log,contrast,shadow_ev,highlight_ev], `edges: Float64[4]` -> `values: T[S,C]`.

Linear RGB/Gray. contrast>=0; suggested controls [-2,1,0,0]. Edges satisfy GRD-18A and use stops; suggested [-8,-4,-1,2].

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Evaluate shadow/highlight weights from base by GRD-18A. e=(contrast-1)*(base-pivot_log)+shadow_ev*w_shadow+highlight_ev*w_highlight. Apply x*exp2(e). contrast=1 with both EV controls zero copies the input.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

At base=pivot_log with zero zone EV, changing contrast leaves the pixel unchanged. A uniform +1 effective EV doubles the pixel.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
