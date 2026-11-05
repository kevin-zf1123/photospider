---
spec_schema_version: 1
id: GRD-30C
parent_id: GRD-30
function: halftone_cmyk
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.halftone_cmyk
---

# GRD-30C: CMYK separation screens

Inherits the [family contract](GRD-30_halftone_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.halftone_cmyk` performs the transformation below.

## Interface

`input: T[H,W,C]` -> `values: T[H,W,C]`.

Select four explicitly ordered CMYK ink slots with an explicit CMYK interpretation. Required per-ink screen:Array[String,4] entries dot|line, bases:Float64[4,2,2], origins:Float64[4,2]. samples_per_axis and edge as GRD-30A.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Evaluate GRD-30A or B independently for C,M,Y,K coverage using that ink screen geometry. Return the four sampled ink coverages in the corresponding source slots and copy other slots.

## Behavior

E per ink. The input is already CMYK; RGB separation uses a separately selected ICC transform. This output represents ink separations.

## Acceptance

K-only input leaves C/M/Y at zero. Each ink preserves zero/full endpoints. Alpha is an independent bypass.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
