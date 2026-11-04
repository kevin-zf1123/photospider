---
spec_schema_version: 1
id: GRD-25A
parent_id: GRD-25
function: histogram
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.histogram
---

# GRD-25A: Selected histogram

Inherits the [family contract](GRD-25_histogram_equalize_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.histogram` performs the transformation below.

## Interface

`feature: T[H,W]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `counts: Int64[B]` in binary mode or `Float64[B]` in weighted mode, `count: Int64[1]`, `mass: Float64[1]`.

selection_mode=binary|weighted defaults binary. B:Int64 2..65536 default 256; `roi_x0`, `roi_y0`, `roi_x1`, `roi_y1` are Int64 half-open coordinates defaulting to 0,0,W,H; `edge:String=reject|clamp` defaults clamp. `selection_mode` is String and `max_work:Int64` defaults to 9223372036854775807. Selected feature domain is [0,1].

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Set u=x or clamp(x,0,1) according to edge. bin=min(B-1,floor(B*u)). Add the effective selection weight to this bin; binary counts use integer increments. count counts positive weights and mass sums them. Empty histograms are zero.

## Behavior

W over the metering ROI. count/mass use selection only; counts also use positive-weight feature samples. Static mode determines counts dtype.

## Acceptance

With B=2, selected values [0,0.5,1] produce binary counts [1,2]. Weights [0.5,0.25,0.25] produce [0.5,0.5].

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
