---
spec_schema_version: 1
id: GRD-29B
parent_id: GRD-29
function: dither_noise
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.dither_noise
---

# GRD-29B: Dither with explicit noise

Inherits the [family contract](GRD-29_dither_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.dither_noise` performs the transformation below.

## Interface

`input: T[H,W,C]`, `noise: Float32/64[H,W,Csel]` -> `values: T[H,W,C]`.

Noise in [0,1), matched to selected component order. levels:Int64 2..65536 default 2; edge=reject|clamp defaults clamp.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Use each noise sample as the threshold in the family uniform rule. Uniformly distributed noise makes the rounding probability equal to the fractional quantization coordinate.

## Behavior

E on the source and corresponding noise. Random generation and seed semantics belong to the explicit noise producer.

## Acceptance

At levels=2 and x=0.25, noise=0.1 selects 1 while noise=0.5 selects 0. Source endpoints are exact.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
