---
spec_schema_version: 1
id: CRV-06F
parent_id: CRV-06
function: color_ramp_oklch
operation_family: curve.color_ramp_oklch
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06F: color_ramp_oklch

Dynamic inputs follow the [family Result tensor-port contract](CRV-06_color_ramp.md#interface): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

This family uses the polar form of OKLab with fixed D65, not CIELCh(ab). L uses the OKLab scale (nominal 0..1, all finite extensions allowed), C is finite and nonnegative, and h has three independent input representations. No alpha, custom white substitution, RGB conversion or gamut clipping is included. The [model author](https://bottosson.github.io/posts/oklab/) defines the polar coordinates by C=sqrt(a*a+b*b), h=atan2(b,a). These relationships identify the model; the ramp consumes supplied polar coordinates and does not perform this conversion during interpolation.

## Interface and behavior

Every port, parameter, default, domain, limit, mathematical, precision, zero and dependency rule is that of [CIELCh CRV-06D](CRV-06D_color_ramp_cielch.md), replacing the model with OKLCh, normalized CIELAB l/C* with OKLab L/C and configurable white with fixed D65. The lightness-scale rule does not rescale OKLab coordinates. Specifically:

- Floating radian/pi inputs use colors[K,3]; rational input uses lightness_chroma[K,2] and Int64 numerator/denominator arrays. Selected rows require q>0; unused generic q<=0 rows add no mathematical-domain check.
- Output dtype defaults to colors or lightness_chroma dtype. Output hue unit defaults to radian for radian input and pi_multiple for the other two inputs.
- Hue is the original unnormalized value. Its raw difference is interpolated, retaining multiple turns. There is no hue_path or normalization parameter.
- C=0 never discards hue. Unit conversion and interpolation are performed as one mathematical formula with final correct rounding; all profiles match bits.
- Each output color depends on its query value and the selected stop rows; all stops/colors/p/q arrays are the dependency support. Model, finite and selected-denominator conditions are caller preconditions, including at C=0.

Static descriptions must identify the input model and hue representation. The Result carries no color description. `tensor_description_parameter` encodes a canonical logical tuple at axis zero. Rational L/C ports alone are not complete colors; the logical tuple's hue is supplied by the separate numerator/denominator ports.

## Acceptance

Use stops=[0,1], pi colors=[[0.25,0.125,1.75],[0.75,0.375,0.25]], input=[0.5]. The result is [[0.5,0.25,1]]. Rational hues 7/4 and 1/4 agree. Repeat with C=0: hue remains 1. With hues 0 and 4, midpoint hue is 2. CIELCh and custom-white metadata is rejected. The CRV-06D acceptance cases apply with the OKLCh description.

## References

- [Radian entrypoint](CRV-06F1_color_ramp_oklch.md).
- [Floating pi entrypoint](CRV-06F2_color_ramp_oklch_pi.md).
- [Rational pi entrypoint](CRV-06F3_color_ramp_oklch_rational_pi.md).
- [OKLab counterpart](CRV-06E_color_ramp_oklab.md).
