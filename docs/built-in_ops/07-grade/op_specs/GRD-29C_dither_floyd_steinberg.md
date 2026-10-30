---
spec_schema_version: 1
id: GRD-29C
parent_id: GRD-29
function: dither_floyd_steinberg
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.dither_floyd_steinberg
---

# GRD-29C: Floyd-Steinberg uniform-level dither

Inherits the [family contract](GRD-29_dither_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.dither_floyd_steinberg` performs the transformation below.

## Interface

`input: T[H,W,C]` -> `values: T[H,W,C]`.

Selected normalized channels. levels:Int64 2..65536 default 2; scan=raster|serpentine defaults serpentine; edge=reject|clamp defaults clamp.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Use the Float64 recurrence and scan definition in [GRD-29](GRD-29_dither_contract.md), independently per component. At each visited sample set v=u+error, q=clamp(round_ties_even(v*(levels-1)),0,levels-1), r=Float64(q)/Float64(levels-1). Publish r converted to T and diffuse v-r.

## Behavior

S support from canonical full-image scan predecessors; the complete source image is an admitted support superset. A tile output uses the recurrence from the full-image origin and never resets error at tile/ROI boundaries.

## Acceptance

A raster 1x4 image of 0.5 at levels=2 yields [0,1,0,1]. Test serpentine parity using nonzero global row origins.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
