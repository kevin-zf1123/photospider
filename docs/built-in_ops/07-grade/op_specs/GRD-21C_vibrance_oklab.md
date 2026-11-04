---
spec_schema_version: 1
id: GRD-21C
parent_id: GRD-21
function: vibrance_oklab
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.vibrance_oklab
---

# GRD-21C: OKLab vibrance

Inherits the [family contract](GRD-21_saturation_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.vibrance_oklab` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[2]` [strength,chroma_reference] -> `values: T[S,C]`.

Select OKLab. strength in [-1,1], suggested 0; chroma_reference>0, suggested 0.4.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

q=min(1,hypot(a,b)/chroma_reference); g=1+strength*(1-q)^2. Copy L; multiply a,b by g. strength=0 or q=1 copies. Skin protection is expressed by a separate effect mask.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Gray remains gray. Chroma at or above chroma_reference is unchanged. For C/reference=0.5 and strength=1, g=1.25.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
