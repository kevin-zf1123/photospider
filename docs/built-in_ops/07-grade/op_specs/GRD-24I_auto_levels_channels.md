---
spec_schema_version: 1
id: GRD-24I
parent_id: GRD-24
function: auto_levels_channels
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_levels_channels
  - grade.auto_levels_channels_statistics
  - grade.auto_levels_channels_apply
---

# GRD-24I: Automatic per-channel levels

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_levels_channels` performs the transformation below.

## Interface

`input: T[H,W,C]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W,C]`.

Explicit selected numeric/RGB components. Raw numeric selection is any nonempty ordered unique component subset, without a four-component limit or RGB-basis requirement; semantic descriptions retain the existing FMT group constraints. Selection, alpha binding and metering coordinates follow GRD-24. `probability_low:Float64=0.01`, `probability_high:Float64=0.99`, `gamma:Float64=1`, `out0:Float64=0`, `out1:Float64=1`, with the domains of GRD-24E.

`grade.auto_levels_channels_statistics(input,selection;P) -> controls:Float64[rows,6]`, rows equal to selected component count in selection order; `grade.auto_levels_channels_apply(input,controls;P) -> values:T` with the original shape. Each row is `[low,high,gamma,out0,out1,state]`, or `[0,1,1,0,1,1]` for insufficient statistics; state is Float64{0,1}. The complete entry creates this atomic temporary once per Run for all application tiles; subsequent Runs refresh it. State 1 bit-copies the corresponding source component before clamping. Apply accepts the same P and uses controls without collecting statistics. Axis, selector encoding and `max_work` follow the family contract.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

Compute quantiles and GRD-24C controls independently for each selected component. Apply GRD-04A to each component with state=0; copy it when state=1. All statistics use the original image.

## Behavior

W over the selected components in the metering ROI plus the requested source samples. Bypass components and alpha remain independent.

## Acceptance

A constant red channel copies while a varying green channel is adjusted. This mode can change RGB ratios independently of brightness-only paths.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
