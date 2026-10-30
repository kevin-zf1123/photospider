---
spec_schema_version: 1
id: GRD-14C
parent_id: GRD-14
function: threshold_local_gaussian
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.threshold_local_gaussian
---

# GRD-14C: Gaussian-weighted local threshold

Inherits the [family contract](GRD-14_threshold_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.threshold_local_gaussian` performs the transformation below.

## Interface

`feature: T[H,W]`, `offset: Float64[1]` -> `mask: UInt8[H,W]`.

radius:Int64 0..4096 default 3; sigma:Float64>0 default 2 pixels. Suggested offset=0 in feature units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

For the cropped square radius neighborhood, w_q=exp(-distance(p,q)^2/(2*sigma^2)); m=sum(w_q*x_q)/sum(w_q). Return 1 iff x_p>=m-offset. Use Float64 sums.

## Behavior

The mathematical support is H(radius), cropped against the full image extent. ROI and tile edges retain the same full-image neighborhood. Output mask values are UInt8 0 or 1.

## Acceptance

A constant feature with zero offset gives all ones. Radius zero agrees with GRD-14B.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
