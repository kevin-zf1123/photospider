---
spec_schema_version: 1
id: GRD-12B
parent_id: GRD-12
function: monochrome_linear_y
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.monochrome_linear_y
---

# GRD-12B: Linear-Y monochrome

Inherits the [family contract](GRD-12_monochrome_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.monochrome_linear_y` performs the transformation below.

## Interface

`input: T[S,C]` -> `values: T[S,C]`.

Select relative linear RGB with explicit basis/white. Output replaces the same RGB group; other slots, including alpha, bypass.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Current parameters

`source_basis: String` uses the FMT basis codec and is required for raw selection; a described relative linear RGB group may supply it. Other selectors follow the family profile.

## Mathematics

Extract Y in Float64 and return the neutral triple (Y,Y,Y).

### Current native algorithm/profile

Form Y from the second row of the resolved RGB-to-XYZ matrix using Float64 native dot products, then write (Y,Y,Y) to the selected RGB slots. No opponent-model lightness is substituted for Y.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Black remains black. Compare a colored input to explicit model-neutral reconstruction, and check alpha/AOV bit preservation. CIELAB l=0.5 corresponds to relative Y=(66/116)^3.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
