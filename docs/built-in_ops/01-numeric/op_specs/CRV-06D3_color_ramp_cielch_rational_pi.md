---
spec_schema_version: 1
id: CRV-06D3
parent_id: CRV-06D
function: color_ramp_cielch_rational_pi
operation_family: curve.color_ramp_cielch_rational_pi
proposed_operation_keys:
  - curve.color_ramp_cielch_rational_pi_strict
  - curve.color_ramp_cielch_rational_pi_accelerated_apple_silicon
  - curve.color_ramp_cielch_rational_pi_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06D3: color_ramp_cielch_rational_pi

The five dynamic ports inherit the family sole-tensor Result rule: each has exactly one tensor member under any structurally valid schema id, version and member key. Use complete `sample_shape()` values including batch axes: `input` is S, `stops` is [K], the model-specific color pair array is [K,2], and `hue_numerator` and `hue_denominator` are [K].

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output table coordinates and validation points. Finite values outside 0..1 remain legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged. Color configuration uses canonical TensorDescription v4/v5 tuple parameters; explicit L* units are rejected without sample-magnitude inference.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

This independent CIELCh(ab) ramp uses exact rational pi-multiple source hue. The ordered dynamic inputs are input, stops, lightness_chroma, hue_numerator, hue_denominator; output is named values. lightness_chroma is Float32/Float64[K,2]; numerator and denominator are Int64[K]; selected rows require denominator>0, and fractions need not be reduced. Unselected q<=0 has no added mathematical-domain check. input and stops independently accept Float32/Float64. stops is [K] with 1<=K<=65536; input has rank 1..7, and values has shape sample_shape(input)+[3]. All logical element limits, matching K and finite-value rules come from the [complete CIELCh contract](CRV-06D_color_ramp_cielch.md).

Static String parameters are color_description, dtype, out_of_domain and output_hue_unit. Constructors default dtype to lightness_chroma dtype, out_of_domain to clamp, output_hue_unit to pi_multiple. Both radian and pi_multiple output units remain available. color_description explicitly identifies the input representation, CIELCh(ab), white (default D50) and no alpha. The output parameter selects the floating hue unit; the Result carries no description.

The public authoring helper is `color_ramp_cielch_rational_pi_node`.

## Mathematics and behavior

All original-hue interpolation, C=0 hue retention, correct final rounding and unit-conversion rules are normative from CRV-06D. Strict is correctly rounded; accelerated finite arithmetic follows the shared FP32-scaled final-result bound. Exact hit/clamp directly converts the original hue; no source hue is discarded because chroma is zero.

Caller preconditions and dependency support follow the [family contract](CRV-06_color_ramp.md#behavior). Selected hue and rational-denominator preconditions remain active at zero chroma or saturation.

## Acceptance

With stops=[0,1], matching color_description and explicit static parameters: lightness_chroma=[[0.25,2],[0.75,4]], hue_numerator=[7,1], hue_denominator=[4,4], input=[0.5] gives [[0.5,3,1]]. The example uses the default output unit; values carries only tensor structure and samples.

Acceptance also covers per-profile/dtype/unit fixtures, multi-turn and large-angle cases, unused remote rows and source/destination precision mixing. Include INT64_MIN and equivalent unreduced fractions.
