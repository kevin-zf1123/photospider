---
spec_schema_version: 1
id: GRD-24D
parent_id: GRD-24
function: auto_exposure
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_exposure
---

# GRD-24D: Automatic exposure of a single image

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_exposure` performs the transformation below.

## Interface

`input: T[H,W,C]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W,C]`.

Relative linear RGB or Gray. Selection and alpha_policy follow GRD-24. Static percentile:Float64 in [0,1] default 0.5; target/EV limits as GRD-24B. ROI defaults full image.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Derive effective weights, extract linear Y in Float64, compute its selected quantile, and derive EV as GRD-24B. Apply exposure to the original image; state=1 copies it.

## Behavior

Every requested adjusted pixel depends on the metering ROI and the local source tuple. Statistics and output ROI are independent. This operation has no temporal state.

## Acceptance

Uniform Y=0.09 with default target is doubled. An empty selection copies all requested original values, including values outside [0,1].

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
