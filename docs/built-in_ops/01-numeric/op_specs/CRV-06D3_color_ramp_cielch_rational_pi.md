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
implementation_status: implemented_subset
clarification_status: complete
---

# CRV-06D3: color_ramp_cielch_rational_pi

The five dynamic ports inherit the family sole-tensor Result rule: each has
exactly one tensor member under any structurally valid schema id,
version and member key. Use complete `sample_shape()` values including batch axes:
`input` is S, `stops` is [K], the model-specific color pair array is [K,2], and
`hue_numerator` and `hue_denominator` are [K].

## Revised lightness coordinate and implementation boundary

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
requires native CIELAB/CIELCh l=L*/100, including ramp stops' color values,
LUT input axes and output table coordinates. Finite values outside 0..1 remain
legal. Opponent/chroma scales and arithmetic formulas are unchanged. The current runtime ColorArray v1 encodes L* in its implicit 0..100 unit;
this target scale applies only after public metadata, fixtures and consumers
adopt it explicitly. No implicit unit alias or inference from sample magnitude
is permitted.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and purpose

This independent CIELCh(ab) ramp uses exact rational pi-multiple source hue.
The ordered dynamic inputs are input, stops, lightness_chroma, hue_numerator, hue_denominator; output is named values.
lightness_chroma is Float32/Float64[K,2]; numerator and denominator are Int64[K]; selected rows require denominator>0, and fractions need not be reduced. Unselected q<=0 has no added mathematical-domain check.
input and stops independently accept Float32/Float64. stops is [K] with
1<=K<=65536; input has rank 1..7, and values has shape sample_shape(input)+[3].
All logical element limits, matching K and finite-value rules come from the
[complete CIELCh contract](CRV-06D_color_ramp_cielch.md).

Static String parameters are color_description, dtype, out_of_domain
and output_hue_unit. Constructors default dtype to lightness_chroma dtype,
out_of_domain to clamp, output_hue_unit to pi_multiple.
Both radian and pi_multiple output units remain available. color_description
explicitly identifies the input representation, CIELCh(ab), white (default D50)
and no alpha. The output parameter selects the floating hue unit; the Result carries no description.

## Inherited executable contract

All original-hue interpolation, C=0 hue retention, correct final
rounding and unit-conversion rules are normative from CRV-06D.
Strict is correctly rounded; accelerated finite arithmetic follows the shared
FP32-scaled final-result bound. No GPU variant is specified.
Exact hit/clamp directly converts the original hue; no source hue is discarded
because chroma is zero. A mismatched platform is not silently redirected.

Inherit the family's [fixed execution and caller preconditions](CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions), including complete-color output tiles, conservative table dependencies, fixed scratch, cancellation and retained output lifetime. Selected hue and rational-denominator preconditions remain active at zero chroma or saturation. Unfinished certified arithmetic returns ResourceExhausted.

## Workflow and acceptance

Bind the ordered ports above, stops=[0,1], matching color_description, and
explicit static parameters. lightness_chroma=[[0.25,2],[0.75,4]], hue_numerator=[7,1], hue_denominator=[4,4], input=[0.5] gives [[0.5,3,1]].
The example uses the default output unit; values carries only tensor structure and samples.

Use the shared independent arithmetic oracle, per-profile/dtype/unit fixtures,
multi-turn and large-angle cases, full versus sparse complete-color requests, unused remote rows, strides, low-budget failures, cancellation and
owner-lifetime acceptance. Include INT64_MIN and equivalent unreduced fractions.
The implementation section links the constructor and shared arithmetic contract.

## Maintained implementation and validation

The public helper `color_ramp_cielch_rational_pi_node` is declared in
[`color_ramps.hpp`](../../../../plugins/ops/include/photospider/ops/numeric/color_ramps.hpp).
`color_ramps.cpp` prepares immutable model/unit/profile state and executes the
fixed CPU tile kernel. Original hue values use exact rational interpolation;
Cross-unit conversion in either direction multiplies or divides by certified pi,
with a 4096-bit precision ceiling. Same-unit RationalPi expressions cancel pi
symbolically. Validate the Result path and numerical profile independently for each change. See the [family contract](CRV-06_color_ramp.md#maintained-implementation-and-validation)
for the shared behavior and acceptance requirements.
