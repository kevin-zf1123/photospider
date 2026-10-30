---
spec_schema_version: 1
id: GRD-11A
parent_id: GRD-11
function: white_balance_gains
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.white_balance_gains
---

# GRD-11A: White balance with RGB gains

Inherits the [family contract](GRD-11_white_balance_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.white_balance_gains` performs the transformation below.

## Interface

`input: T[S,C]`, `gains: Float64[3]` -> `values: T[S,C]`.

Select linear RGB. Gains are positive; suggested [1,1,1].

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

y_c=x_c*g_c. Unit gains copy. The existing RGB basis and reference interpretation remain the caller-selected ones; gain normalization is explicit in the supplied values.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

RGB (0.25,0.5,1) with gains (2,1,0.5) gives (0.5,0.5,0.5).

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
