---
spec_schema_version: 1
id: GRD-24E
parent_id: GRD-24
function: auto_levels
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_levels
---

# GRD-24E: Automatic levels in Proportional RGB luminance

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_levels` performs the transformation below.

## Interface

`input: T[H,W,C]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W,C]`.

Select relative linear RGB. Selection mode, alpha_policy and ROI follow GRD-24. probabilities:Float64[2] defaults [0.01,0.99], with 0<=p0<p1<=1. gamma,out0,out1 as GRD-24C, in the selected feature units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Extract the `ratio` feature in Float64; compute its two quantiles and GRD-24C controls. If state=1, copy original RGB. Otherwise apply bounded levels to the feature with Float64 output and use [GRD-20F](GRD-20F_reconstruct_luminance.md) to set it. Each named feature/statistic/mapped-feature stage is a Float64 boundary.

## Behavior

W over the metering ROI plus each requested RGB tuple. The statistical selection does not restrict where the effect is applied. Single-image operation.

## Acceptance

A constant or empty metering selection copies original signed/HDR RGB. A nonconstant ramp maps the selected feature quantiles to the chosen output endpoints.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
