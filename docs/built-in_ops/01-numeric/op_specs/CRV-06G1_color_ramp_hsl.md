---
spec_schema_version: 1
id: CRV-06G1
parent_id: CRV-06G
function: color_ramp_hsl
operation_family: curve.color_ramp_hsl
proposed_operation_keys:
  - curve.color_ramp_hsl_strict
  - curve.color_ramp_hsl_accelerated_apple_silicon
  - curve.color_ramp_hsl_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06G1: color_ramp_hsl

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#interface): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

This independent HSL ramp uses floating radian source hue. The ordered dynamic inputs are input, stops, colors; output is named values. colors is Float32/Float64[K,3], ordered H, S, L. input and stops independently accept Float32/Float64. stops is [K] with 1<=K<=65536; input has rank 1..7, and values has shape sample_shape(input)+[3]. All logical element limits, matching K and finite-value rules come from the [complete HSL contract](CRV-06G_color_ramp_hsl.md).

Static String parameters are color_description, dtype, out_of_domain and output_hue_unit. Constructors default dtype to colors dtype, out_of_domain to clamp, output_hue_unit to radian. Both radian and pi_multiple output units remain available. color_description explicitly identifies the input representation, HSL, underlying RGB primaries/white/transfer (default sRGB/D65/sRGB) and no alpha. The output parameter selects the floating hue unit; the Result carries no description.

The public authoring helper is `color_ramp_hsl_node`.

## Mathematics and behavior

All original-hue interpolation, S=0 hue retention, correct final rounding and unit-conversion rules are normative from CRV-06G. Strict is correctly rounded; accelerated finite arithmetic follows the shared FP32-scaled final-result bound. Exact hit/clamp directly converts the original hue; no source hue is discarded because saturation is zero.

Caller preconditions and dependency support follow the [family contract](CRV-06_color_ramp.md#behavior). Selected hue and rational-denominator preconditions remain active at zero chroma or saturation.

## Acceptance

With stops=[0,1], matching color_description and explicit static parameters: colors=[[0,0.25,0.125],[0,0.75,0.875]], input=[0.5] gives [[+0,0.5,0.5]]. The example uses the default output unit; values carries only tensor structure and samples.

Acceptance also covers per-profile/dtype/unit fixtures, multi-turn and large-angle cases, unused remote rows and source/destination precision mixing. Include source/destination precision mixing and finite huge hues.
