---
spec_schema_version: 1
id: GRD-29A
parent_id: GRD-29
function: dither_bayer
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.dither_bayer
---

# GRD-29A: Bayer ordered dither

Inherits the [family contract](GRD-29_dither_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.dither_bayer` performs the transformation below.

## Interface

`input: T[H,W,C]` -> `values: T[H,W,C]`.

Selected normalized channels. levels:Int64 2..65536 default 2; order:Int64 1..8 default 2, N=2^order; origin:Int64[2]=[0,0]; channel_phase:Int64[Csel,2] defaults zero; edge=reject|clamp defaults clamp.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

B_1=[0]; B_2n=[[4B_n,4B_n+2],[4B_n+3,4B_n+1]]. Use threshold=(B_N[(y-origin_y+phase_y) mod N,(x-origin_x+phase_x) mod N]+0.5)/N^2. Quantize with the family uniform rule.

## Behavior

E support with absolute integer image coordinates and Euclidean modulo, including negative origins. ROI/tile requests preserve phase.

## Acceptance

With N=2, Bayer ranks are [[0,2],[3,1]]. At levels=2, input 0.5 chooses 1 at ranks 0,1 and 0 at ranks 2,3.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
