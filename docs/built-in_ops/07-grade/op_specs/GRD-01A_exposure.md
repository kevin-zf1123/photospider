---
spec_schema_version: 1
id: GRD-01A
parent_id: GRD-01
function: exposure
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.exposure
---

# GRD-01A: Uniform linear exposure

Inherits the [family contract](GRD-01_exposure_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.exposure` performs the transformation below.

## Interface

`input: T[S,C]`, `ev: Float64[1]` -> `values: T[S,C]`.

Select linear RGB or linear-Y Gray components. The EV unit is stops; the suggested control value is 0.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

`y = x * exp2(ev)` for each selected component. EV zero copies the input. Zero x preserves its sign for every finite EV, including when exp2(ev) would overflow.

## Behavior

The mathematical support is E: matching coordinates and the controls named in the formula. Unchanged components, alpha and AOVs follow the independent bypass rule in the common contract.

## Acceptance

Input 0.25 at +1 stop gives 0.5; at -1 stop gives 0.125. EV zero preserves signed/HDR samples and bypass bits.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
