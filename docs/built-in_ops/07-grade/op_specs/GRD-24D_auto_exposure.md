---
spec_schema_version: 1
id: GRD-24D
parent_id: GRD-24
function: auto_exposure
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_exposure
  - grade.auto_exposure_statistics
  - grade.auto_exposure_apply
---

# GRD-24D: Automatic exposure of a single image

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_exposure` performs the transformation below.

## Interface

`input: T[H,W,C]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W,C]`.

Relative linear RGB or linear-Y Gray, with relative reference, units and scale. Selection, metering coordinates and explicit alpha binding follow GRD-24. Static `percentile:Float64=0.5` in [0,1], `target:Float64=0.18`, `ev_min:Float64=-16`, `ev_max:Float64=16`; target>0 and ev_min<=ev_max. Color selectors and `max_work` use the family parameter encoding.

`grade.auto_exposure_statistics(input,selection;P) -> controls:Float64[1,6]`; `grade.auto_exposure_apply(input,controls;P) -> values:T` with the original shape. The row is `[ev,0,0,0,0,state]`, or `[0,0,0,0,0,1]` for insufficient statistics; state is Float64{0,1}. The complete entry creates this atomic temporary once per Run for all application tiles; subsequent Runs refresh it. State 1 bit-copies before any adjustment. Apply accepts P but consumes the numerical EV from controls without collecting statistics.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Derive effective weights, extract linear Y in Float64, compute its selected quantile, and derive EV as GRD-24B. Apply exposure to the original image; state=1 copies it.

## Behavior

Every requested adjusted pixel depends on the metering ROI and the local source tuple. Statistics and output ROI are independent. This operation has no temporal state.

## Acceptance

Uniform Y=0.09 with default target is doubled. An empty selection copies all requested original values, including values outside [0,1].

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
