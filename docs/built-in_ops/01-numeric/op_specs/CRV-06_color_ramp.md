---
spec_schema_version: 1
id: CRV-06
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06: color-ramp family

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Native lightness and explicit description

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md) uses native CIELAB/CIELCh `l=L*/100` for ramp colors, LUT input axes, output table coordinates and validation points. Finite values outside 0..1 remain legal. Opponent/chroma scales, hue winding and arithmetic formulas are unchanged. Color configuration uses canonical TensorDescription v4/v5 tuple parameters; explicit L* units are rejected without sample-magnitude inference.

## Representation and model split

The ramp uses explicit dynamic stops[K] and colors[K,C] rather than only a uniform LUT. Each scalar element of input maps to one complete color, with output shape sample_shape(input)+[C]. Nonuniform stop spacing is supported. This scalar-to-color mapping differs from CRV-05's independent per-input-channel table application.

The family has separate operations and specifications for RGB, CMYK, XYZ, CIELAB, CIELCh(ab), OKLab, OKLCh, HSL and YCbCr, not an unspecified color-model mode of one numeric interpolator. Each linked specification records its exact color definition, channel domains, reference parameters and alpha handling, stop constraints, input/output types, interpolation, boundaries, precision and output semantics. CIELAB/CIELCh(ab) and OKLab/OKLCh are separate pairs; their scales, reference whites and hue conventions must not be interchanged.

CIELAB, CIELCh(ab), OKLab and OKLCh use three color components without alpha. RGB alone defines explicit RGBA association behavior, while CMYK is four ink channels without alpha. The 15 independent entrypoints are registered as 45 profile keys (strict, apple_silicon, x86_64) with public constructors.

## Interface

Each dynamic port is a Result containing exactly one tensor member. Any structurally valid schema id/version/member key is accepted. Shapes are validated against each member's complete `sample_shape()`, including batch axes. The ports are `input` (rank 1..7, Float32/64), `stops` (rank 1, K=1..65536, Float32/64), `colors` (rank 2, [K,C], Float32/64), and for RationalPi variants `hue_numerator` and `hue_denominator` (rank 1, [K], Int64). RationalPi colors use [K,2] model-specific component arrays; all other colors use [K,C]. The first three floating inputs independently select their dtype. All input, color and output logical element products are limited to 2^40. Only a mathematically selected RationalPi denominator q must be positive; an unselected generic q<=0 value adds no numerical precondition.

`values` uses a fresh `photospider.tensor` v1 schema, member `samples`, descriptor shape `sample_shape(input)+[C]` and `atomic_trailing_axes=1`. It carries no color description and does not preserve the input's batch-axis partition.

Color ramp configuration is supplied by the required String `color_description`, encoded with `tensor_description_parameter`. It describes one logical tuple with `channel_axis=0`; the operation separately validates its physical port shape and member model. Dtype and out_of_domain are explicit parameters; polar members also declare output_hue_unit. RGB input/output is straight; an `output_association` parameter is rejected. No Result carries a color description. The caller provides the required color configuration to every downstream consumer; the output itself carries only structure and numerical samples. Generic color arrays follow the [color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).

Malformed static parameters/descriptions fail with InvalidArgument; incompatible shapes, dtypes and schemas use TypeMismatch.

## RGB color description

RGB ramps explicitly specify gamut/primaries, white point and transfer function, with default sRGB primaries, D65 and the standard piecewise sRGB transfer function. RGB interpolation is performed in linear light, decoding input colors and encoding output colors in the same specified space. The standard sRGB transfer is documented in [W3C CSS Color 4](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/#valdef-color-srgb); this is a reference for the transfer function, not adoption of CSS's complete interpolation or rendering behavior. The [color-management category](../../02-format-color/representation.md) separates assignment, transfer, RGB matrix and model conversion.

## Mathematics

The selected row or adjacent pair is located by search in the source stops. Direct selection and complete stored-row identity retain the model's direct conversion rules. For rational hue, identity compares raw p/q as well as both floating components; equivalent unreduced fractions alone do not imply row identity. RGB interpolation evaluates the full transfer/association expression. Other models interpolate their coordinates, with correctly rounded pi multiplication/division for cross-unit hues. There is no modulo reduction, shortest-arc selection or achromatic hue substitution.

## Behavior

Each output color depends on its query value and on the stop row or adjacent stop rows containing it. Stops, colors and optional p/q arrays are selected by query value, so the whole arrays are the dependency support of any output element. K=1 still reads the requested query positions and the stop/color row. Region and tile results equal the corresponding slice of the whole result.

The caller guarantees all stops are finite and strictly increasing, consumed queries are finite, and reject-policy queries lie in the closed stop domain. Selected complete colors must be finite and obey their model and association rules; selected rational denominators must be positive, including at zero chroma/saturation. Unselected generic color rows and p/q pairs carry no numerical precondition. Final components must be finite in the output dtype. For RGBA, a positive exact alpha that rounds to zero requires all rounded RGB components to be zero; this valid transparent tuple canonicalizes to +0. Numerical violations have no promised runtime domain or overflow diagnostic.

## CMYK profile resource

CMYK declares the profile's content identity as an operation resource. The selected frozen bytes must be a valid ICC v2/v4 `prtr`/`CMYK` profile with valid header/tag structure and, for v4, a valid Profile ID when present. Changing profile identity requires recompilation. Interpolation does not execute ICC transforms or infer total-ink limits.

## Acceptance

Expected values come from independent exact coordinate and RGB references. Cover valid numerical cases, explicit raw negative zeros, all model/unit/dtype combinations, straight RGB alpha, sparse tiles and mixed source grids. Static malformed ICC cases are rejection tests; invalid numerical preconditions do not define runtime failure tests.

## References

- [Curve category](../curves.md).
- [RGB ramp specification](CRV-06A_color_ramp_rgb.md).
- [CMYK ramp specification](CRV-06B_color_ramp_cmyk.md).
- [CIELAB ramp specification](CRV-06C_color_ramp_cielab.md).
- [CIELCh entrypoints and hue contract](CRV-06D_color_ramp_cielch.md).
- [OKLab ramp specification](CRV-06E_color_ramp_oklab.md).
- [OKLCh entrypoints and hue contract](CRV-06F_color_ramp_oklch.md).
- [HSL entrypoints and contract](CRV-06G_color_ramp_hsl.md).
- [YCbCr ramp specification](CRV-06H_color_ramp_ycbcr.md).
- [XYZ ramp specification](CRV-06I_color_ramp_xyz.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Foundation color and execution contracts](../../00-foundation/contracts.md).
