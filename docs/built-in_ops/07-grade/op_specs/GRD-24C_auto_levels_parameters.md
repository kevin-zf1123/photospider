---
spec_schema_version: 1
id: GRD-24C
parent_id: GRD-24
function: auto_levels_parameters
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_levels_parameters
---

# GRD-24C: Derive automatic-levels parameters

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_levels_parameters` performs the transformation below.

## Interface

`quantiles: Float64[2]` [low,high], `count: Int64[1]` -> `controls: Float64[1,5]`, `state: UInt8[1]`.

Static gamma:Float64>0 default 1; out0,out1:Float64 default 0,1. Quantiles are ordered low<=high.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

If count>0 and low<high, return [low,high,gamma,out0,out1],state=0. Empty or equal-bound statistics return placeholder [0,1,1,0,1],state=1. The consuming image operation copies original samples when state=1.

## Behavior

Both outputs depend on the supplied statistics. The placeholder is not applied to the source during fallback, so extended-range source values remain intact.

## Acceptance

Quantiles [0.2,0.8] yield these input bounds. Equal bounds select state=1 and the image path preserves original HDR values.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
