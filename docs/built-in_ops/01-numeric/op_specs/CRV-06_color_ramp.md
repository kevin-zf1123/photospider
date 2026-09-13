---
spec_schema_version: 1
id: CRV-06
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06: color-ramp family

## Confirmed representation and model split

Use explicit dynamic stops[K] and colors[K,C] rather than only a uniform LUT.
Each scalar element of input maps to one complete color, with output shape
input.shape+[C]. Nonuniform stop spacing is supported. This scalar-to-color
mapping differs from CRV-05's independent per-input-channel table application.

The maintainer explicitly requires separate implementations/specifications for
RGB, CMYK, XYZ, CIELAB, CIELCh(ab), OKLab, OKLCh, HSL and YCbCr. Do not implement them as an unspecified
color-model mode of one numeric interpolator. RGB is clarified first; each other
model's exact color definition, channel domains, reference parameters and alpha
handling will be specified separately. CIELAB/CIELCh(ab) and OKLab/OKLCh are
separate required pairs; their scales, reference whites and hue conventions
must not be interchanged.

CIELAB, CIELCh(ab), OKLab and OKLCh initially use three color components without
alpha; coverage is separate. RGB alone currently defines its explicit RGBA
association behavior, while CMYK is four ink channels without alpha.

The model-specific operation clarifications are complete in the linked specs.
Each declares stop constraints, input/output types, interpolation, boundaries,
precision, dependencies and output semantics. The shared ColorArray representation
has its initial validation/encoding contract specified and remains Proposed,
requiring implementation.
No target runtime registration or color compatibility result is claimed.

## Confirmed RGB color description requirement

RGB ramps explicitly specify gamut/primaries, white point and transfer function.
The maintainer selected default sRGB primaries, D65 and the standard piecewise
sRGB transfer function, correcting the earlier gamma-2.2 wording. RGB interpolation
is fixed in linear light, decoding input colors and encoding output colors in
the same specified space. Necessary color-management
dependencies may be clarified in their corresponding specifications, without
silently extending current accepted runtime metadata or conversion behavior.

Current typed Image validation in
[semantic.cpp](../../../../src/lib/data/semantic.cpp) accepts RGB only with
primaries=srgb and transfer=linear. Image samples are Float32 HWC; the generic
input.shape+[C] output and configurable RGB encoding are not already supported
typed Image metadata. The required color-description and transfer extensions
must be specified and implemented explicitly rather than relabeling incompatible
data as the existing profile.

The standard sRGB transfer is independently documented in
[W3C CSS Color 4](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/#valdef-color-srgb).
This is primary reference evidence, not adoption of CSS's complete interpolation
or rendering behavior. The project's [color-management category](../../02-format-color/representation.md)
already separates assignment, transfer, RGB matrix and model conversion.

The maintainer selected a new
[generic color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md)
supporting Float32/Float64 and arbitrary legal shapes with a final color axis.
Color ramp outputs carry this complete color information to downstream consumers;
it is not silently discarded into an untyped numeric array.

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
