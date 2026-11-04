---
spec_schema_version: 1
id: GRD-24F
parent_id: GRD-24
function: auto_levels_xyz_y
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_levels_xyz_y
  - grade.auto_levels_xyz_y_statistics
  - grade.auto_levels_xyz_y_apply
---

# GRD-24F: Automatic levels in XYZ Y

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_levels_xyz_y` performs the transformation below.

## Interface

`input: T[H,W,C]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W,C]`.

Select relative linear RGB. Selection mode, alpha_policy and ROI follow GRD-24. `probability_low:Float64=0.01` and `probability_high:Float64=0.99` satisfy 0<=low<high<=1; these are static scalars, not tensor ports. gamma,out0,out1 as GRD-24C, in the selected feature units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

`grade.auto_levels_xyz_y_statistics(input,selection;P) -> controls:Float64[1,6]`; `grade.auto_levels_xyz_y_apply(input,controls;P) -> values:T` with the original shape. The row is `[low,high,gamma,out0,out1,state]`, or `[0,1,1,0,1,1]` for insufficient statistics; state is Float64{0,1}. The complete entry creates this atomic temporary once per Run for all application tiles; subsequent Runs refresh it. State 1 bit-copies before clamp/reconstruction. Apply accepts the same P and uses controls without collecting statistics. Axis, selector encoding, metering coordinates, explicit alpha binding and `max_work` follow the family contract.

## Mathematics

Extract the `xyz_y` feature in Float64; compute its two quantiles and GRD-24C controls. If state=1, copy original RGB. Otherwise apply bounded levels to the feature with Float64 output and use [GRD-20H](GRD-20H_set_xyz_y.md) to set it. Each named feature/statistic/mapped-feature stage is a Float64 boundary.

## Behavior

W over the metering ROI plus each requested RGB tuple. The statistical selection does not restrict where the effect is applied. Single-image operation.

## Acceptance

A constant or empty metering selection copies original signed/HDR RGB. A nonconstant ramp maps the selected feature quantiles to the chosen output endpoints.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
