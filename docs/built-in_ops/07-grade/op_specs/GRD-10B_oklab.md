---
spec_schema_version: 1
id: GRD-10B
parent_id: GRD-10
function: oklab
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.oklab
---

# GRD-10B: OKLab affine adjustment

Inherits the [family contract](GRD-10_lab_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.oklab` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[3,2]` [gain,offset] for the native first,a,b coordinates -> `values: T[S,C]`.

Select OKLab. Suggested rows [1,0]; coefficients may be signed. CIELAB uses l=L*/100 and native a*,b*; OKLab uses native L,a,b.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Apply y=x*gain+offset independently to the three native coordinates. Identity rows copy. Coordinates remain unclipped.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

An offset on a changes a alone. Identity controls preserve all source bits.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
