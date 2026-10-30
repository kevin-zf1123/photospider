---
spec_schema_version: 1
id: GRD-24B
parent_id: GRD-24
function: auto_exposure_parameters
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_exposure_parameters
---

# GRD-24B: Derive automatic-exposure parameters

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_exposure_parameters` performs the transformation below.

## Interface

`quantile: Float64[1]`, `count: Int64[1]` -> `ev: Float64[1]`, `state: UInt8[1]`.

Static target:Float64>0 default 0.18; ev_min,ev_max:Float64 default -16,16 with ev_min<=ev_max.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

If count>0 and quantile>0, ev=clamp(log2(target/quantile),ev_min,ev_max) and state=0. Otherwise return ev=0,state=1. state=1 denotes insufficient metering data and instructs the image consumer to copy.

## Behavior

The outputs depend on the supplied statistic. count>=0 and finite quantile are unchecked input premises. The fallback concerns empty/nonpositive metering results.

## Acceptance

quantile=0.09 and target=0.18 produce +1 EV. Empty or zero-valued metering yields state=1.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
