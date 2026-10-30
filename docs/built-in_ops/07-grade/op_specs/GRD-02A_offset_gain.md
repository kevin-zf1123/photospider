---
spec_schema_version: 1
id: GRD-02A
parent_id: GRD-02
function: offset_gain
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.offset_gain
---

# GRD-02A: Per-component affine adjustment

Inherits the [family contract](GRD-02_offset_gain_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.offset_gain` performs the transformation below.

## Interface

`input: T[S,C]`, `controls: Float64[Csel,2]` with rows [gain,offset] -> `values: T[S,C]`.

Csel is the number of selected slots. Suggested rows [1,0]. Gains and offsets may be signed.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

`y=x*gain+offset`. A row [1,0] copies its source. Signed/HDR values remain unclipped.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

x=0.25, gain=2, offset=-0.1 gives 0.4 within floating tolerance. Negative gain reverses ordering.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
