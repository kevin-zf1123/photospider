---
spec_schema_version: 1
id: CRV-06D1
parent_id: CRV-06D
function: color_ramp_cielch
operation_family: curve.color_ramp_cielch
proposed_operation_keys:
  - curve.color_ramp_cielch_strict
  - curve.color_ramp_cielch_accelerated_apple_silicon
  - curve.color_ramp_cielch_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06D1: color_ramp_cielch

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#interface): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output table coordinates and validation points. Finite values outside 0..1 remain legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged. Color configuration uses canonical TensorDescription v4/v5 tuple parameters; explicit L* units are rejected without sample-magnitude inference.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

This independent CIELCh(ab) ramp uses floating radian source hue. The ordered dynamic inputs are input, stops, colors; output is named values. colors is Float32/Float64[K,3], ordered l=L*/100, C*, h. input and stops independently accept Float32/Float64. stops is [K] with 1<=K<=65536; input has rank 1..7, and values has shape sample_shape(input)+[3]. All logical element limits, matching K and finite-value rules come from the [complete CIELCh contract](CRV-06D_color_ramp_cielch.md).

Static String parameters are color_description, dtype, out_of_domain and output_hue_unit. Constructors default dtype to colors dtype, out_of_domain to clamp, output_hue_unit to radian. Both radian and pi_multiple output units remain available. color_description explicitly identifies the input representation, CIELCh(ab), white (default D50) and no alpha. The output parameter selects the floating hue unit; the Result carries no description.

The public authoring helper is `color_ramp_cielch_node`.

## Mathematics and behavior

All original-hue interpolation, C=0 hue retention, correct final rounding and unit-conversion rules are normative from CRV-06D. Strict is correctly rounded; accelerated finite arithmetic follows the shared FP32-scaled final-result bound. Exact hit/clamp directly converts the original hue; no source hue is discarded because chroma is zero.

Caller preconditions and dependency support follow the [family contract](CRV-06_color_ramp.md#behavior). Selected hue and rational-denominator preconditions remain active at zero chroma or saturation.

## Acceptance

With stops=[0,1], matching color_description and explicit static parameters: colors=[[0.25,2,0],[0.75,4,0]], input=[0.5] gives [[0.5,3,+0]]. The example uses the default output unit; values carries only tensor structure and samples.

Acceptance also covers per-profile/dtype/unit fixtures, multi-turn and large-angle cases, unused remote rows and source/destination precision mixing. Include source/destination precision mixing and finite huge hues.
