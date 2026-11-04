---
spec_schema_version: 1
id: GRD-24A
parent_id: GRD-24
function: selected_quantiles
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.selected_quantiles
---

# GRD-24A: Quantiles of a selected feature

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.selected_quantiles` performs the transformation below.

## Interface

`feature: T[H,W]`, `selection: UInt8[H,W]` in binary mode or `Float64[H,W]` in weighted mode, `probabilities: Float64[K]` -> `quantiles: Float64[K]`, `count: Int64[1]`, `mass: Float64[1]`.

1<=K<=64; probabilities in [0,1]. Static `selection_mode:String=binary|weighted` defaults binary; `roi_x0`, `roi_y0`, `roi_x1`, `roi_y1` are Int64 half-open coordinates defaulting to 0,0,W,H. `max_work:Int64` defaults to 9223372036854775807. Selected weights follow the family contract.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Use the binary linear quantile or weighted inverse-CDF definition in [GRD-24](GRD-24_auto_adjust_contract.md). count is the number of positive-weight samples; mass is their total weight. Empty selection returns zero count/mass and zero placeholders for quantiles.

## Behavior

W over the explicit statistical ROI. count/mass requests depend only on selection; quantiles additionally depend on positive-weight feature samples and probabilities. Each output has its declared shape independent of sample values.

## Acceptance

Binary samples [0,10] at p=0.5 give 5. Weighted samples [0,10] with weights [0.75,0.25] at p=0.5 give 0. Empty selection has count=mass=0.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
