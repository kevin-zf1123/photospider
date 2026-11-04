---
spec_schema_version: 1
id: GRD-25G
parent_id: GRD-25
function: clahe
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.clahe
---

# GRD-25G: Complete CLAHE

Inherits the [family contract](GRD-25_histogram_equalize_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.clahe` performs the transformation below.

## Interface

`feature: T[H,W]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W]`.

The selection mode, cell geometry, B, clip_factor and edge parameters are those of GRD-25D.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Generate local maps and apply them with GRD-25D/E equations as `clahe_maps -> clahe_apply`. Maps and identity flags are explicit temporary results reused across requested application tiles in the same Run. Both stages use the same declared logical cell geometry; map generation has no metering-ROI parameters.

## Behavior

A requested pixel depends on the source rectangles of its bracketing cells and its own feature. Selection affects mapping statistics, independently of output effect masking.

## Acceptance

The complete operator agrees with separately generated maps followed by GRD-25E within floating tolerance. Constant input remains constant.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
