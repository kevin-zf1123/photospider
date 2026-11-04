---
spec_schema_version: 1
id: GRD-24G
parent_id: GRD-24
function: auto_levels_cielab_l
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.auto_levels_cielab_l
  - grade.auto_levels_cielab_l_statistics
  - grade.auto_levels_cielab_l_apply
---

# GRD-24G: Automatic levels in CIELAB lightness

Inherits the [family contract](GRD-24_auto_adjust_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.auto_levels_cielab_l` performs the transformation below.

## Interface

`input: T[H,W,C]`, `selection: UInt8[H,W]` or `Float64[H,W]` -> `values: T[H,W,C]`.

Select relative linear RGB. Selection mode, alpha_policy and ROI follow GRD-24. `probability_low:Float64=0.01` and `probability_high:Float64=0.99` satisfy 0<=low<high<=1; these are static scalars, not tensor ports. gamma,out0,out1 as GRD-24C, in the selected feature units.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

`grade.auto_levels_cielab_l_statistics(input,selection;P) -> controls:Float64[1,6]`; `grade.auto_levels_cielab_l_apply(input,controls;P) -> values:T` with the original shape. The row is `[low,high,gamma,out0,out1,state]`, or `[0,1,1,0,1,1]` for insufficient statistics; state is Float64{0,1}. The complete entry creates this atomic temporary once per Run for all application tiles; subsequent Runs refresh it. State 1 bit-copies before clamp/reconstruction. Apply accepts the same P and uses controls without collecting statistics. Axis, selector encoding, metering coordinates, explicit alpha binding and `max_work` follow the family contract.

## Mathematics

Extract the `cielab_l` feature in Float64; compute its two quantiles and GRD-24C controls. If state=1, copy original RGB. Otherwise apply bounded levels to the feature with Float64 output and use [GRD-20I](GRD-20I_set_cielab_lightness.md) to set it. Each named feature/statistic/mapped-feature stage is a Float64 boundary.

The complete-entry native profile also publishes original CIELAB coordinates in a Run-local `Float64[H,W,3]` temporary over requested application coordinates. Positive effective metering samples with requested coordinates store the original Lab tuple and use its unchanged l value for quantiles; metering samples outside those requests use the same Y/lightness evaluation directly. Root evaluations use the deterministic per-sample native profile in GRD-24, including the same CIELAB branch and identical scalar/vector lightness sequence. Other requested coordinates are completed after statistics establishes a valid adjustment. Application changes only l, then uses the original Lab-to-XYZ and inverse-basis reconstruction. An unchanged l or state 1 copies source bits. The temporary is regenerated each Run and does not change the public helper signatures above.

## Behavior

W over the metering ROI plus each requested RGB tuple. The statistical selection does not restrict where the effect is applied. Single-image operation.

## Acceptance

A constant or empty metering selection copies original signed/HDR RGB. A nonconstant ramp maps the selected feature quantiles to the chosen output endpoints.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
