---
spec_schema_version: 1
id: CRV-06A
parent_id: CRV-06
function: color_ramp_rgb
operation_family: curve.color_ramp_rgb
proposed_operation_keys:
  - curve.color_ramp_rgb_strict
  - curve.color_ramp_rgb_accelerated_apple_silicon
  - curve.color_ramp_rgb_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-06A: color_ramp_rgb

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registry/platform identity, floating environment and acceptance, and the [family contract](CRV-06_color_ramp.md) for the Result tensor-port rules and caller preconditions. The color-array descriptor, complete-color observation, alpha and precision rules below override the baseline's generic numeric per-component conventions.

## Interface

Ordered required inputs are input[S], stops[K], colors[K,C]. Each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key; shape checks use complete `sample_shape()` values, including batch axes. S is an arbitrary rank-1..7 positive shape, C=3 or 4, colors.shape[0]=stops.shape[0], and output values has shape S+[C]. 1<=K<=65536, and total logical input/color/output products are capped at 2^40 (checked with integer arithmetic). Input, stops and colors independently accept Float32/Float64; the output dtype selects Float32/Float64 and defaults to colors' dtype. Consumed scalar positions must be finite.

Stops and colors map scalar input positions to colors. This RGB operation is separate from the CMYK/LCH/HSL/LAB/YCbCr ramps in the [family contract](CRV-06_color_ramp.md). colors[K,3] is RGB and colors[K,4] is RGBA; the color description explicitly declares alpha presence and must agree with C. Output appends the same color-channel arity.

| Static parameter | Type/domain | Constructor behavior |
| --- | --- | --- |
| color_description | Canonical TensorDescription String; one RGB tuple with optional independent straight alpha | `color_ramp_rgb_description(with_alpha)` supplies sRGB primaries, D65, sRGB transfer and display reference |
| dtype | String float32/float64 | Match colors dtype |
| out_of_domain | String clamp/reject | clamp |

The public constructor takes `ColorRampOptions`. All parameters are explicit in direct nodes. `color_description` is encoded by `tensor_description_parameter` from a TensorDescription tuple; it supplies the RGB model, primaries, white, transfer and optional straight alpha. The logical tuple has `channel_axis=0`; its colors port has physical shape `[K,3]` or `[K,4]`. A four-column tensor alone does not establish an alpha interpretation. Results and Tensor members carry no attached color description. Unknown or mismatched descriptors, parameters and shape relations fail before payload evaluation (InvalidArgument for malformed parameters or descriptions; TypeMismatch for incompatible schemas, shapes or dtypes).

The `values` Result uses `photospider.tensor` v1/member `samples`, descriptor shape `sample_shape(input)+[C]`, and `atomic_trailing_axes=1`. It carries no color description or input batch-axis partition.

Color descriptions follow the [generic color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md). RGB gamut uses explicit three-primary xy and white xy coordinates, with named presets as authoring conveniences; basis presets must agree with explicit coordinates. Transfer is an independent field. No color profile is guessed from C. Supported transfers are linear, standard sRGB and pure-power gamma with a positive finite static exponent; PQ/HLG/log encodings are outside this scope. Alpha presence must agree with C. Finite negative and greater-than-one RGB values are supported: for sRGB/gamma the positive-domain transfer applies to absolute magnitude and restores the original sign; RGB is never clamped to [0,1].

## Mathematics

Explicit RGB primaries/gamut, white point and transfer function apply; defaults are sRGB primaries, D65 and the standard piecewise sRGB transfer (gamma 2.2 is not the default). Input colors and output colors use the same specified encoding and color space. Interpolation is in linear-light RGB: decode color values, interpolate in linear light, then encode back to that same space. No encoded-value interpolation mode is provided.

Input and output RGB coordinates are straight. The description accepts absent or `"straight"` association and rejects `"premultiplied"` as well as an `output_association` parameter. For nonzero alpha, decode straight RGB and weight the linear-light value by alpha for interpolation; divide by the mixed alpha and encode straight output. Alpha itself is never transfer encoded. Alpha must be finite and in [0,1]. Straight input may retain finite hidden RGB at zero alpha, but it contributes zero to the weighted linear color. At zero output alpha emit canonical transparent black, including exact transparent stops. This does not require hidden source RGB to be zero.

Strict treats transfer decode, alpha-weighted linear-light interpolation, alpha division and straight output encoding as a complete mathematical expression, then correctly rounds each final component directly to the output dtype. No intermediate Float64 rounding defines the result. Input floats and static gamma denote their exact numerical values.

Let D be signed transfer decoding and E encoding. linear is identity. For gamma, D(c)=sign(c)*abs(c)^gamma and E(l)=sign(l)*abs(l)^(1/gamma). For sRGB, apply the following exact-real positive-magnitude formulas and restore sign:

```text
D(c) = c/12.92                      if c<=0.04045
       ((c+0.055)/1.055)^(12/5)     otherwise
E(l) = 12.92*l                      if l<=0.0031308
       1.055*l^(5/12)-0.055         otherwise
```

Standard decimal constants are exact rationals for this mathematical definition; static gamma is the exact supplied Float64 value. Correct rounding covers branch selection and all transcendental work. No intermediate gamma reciprocal or transfer output is rounded as part of the strict definition. Signed zeros follow the explicit direct/mixed/transparent policies rather than a sign(0) convention.

For two selected stops, define exact w=(input-stops[j])/(stops[j+1]-stops[j]). For RGB, treat a0=a1=1. For RGBA, ai is each stored alpha. Let ui be the stored straight encoded RGB. For ai=0 use premultiplied-linear qi=0 without decoding hidden color. Otherwise qi=ai*D(ui). Complete selected source colors obey caller validity preconditions even on the zero-contribution path.

```text
A = (1-w)*a0 + w*a1
Q = (1-w)*q0 + w*q1
encoded = E(Q/A)        when A>0
rgb_out = encoded       straight RGB output
```

At exact A=0, all output components are +0. Final alpha is RN_dtype(A); final RGB is RN_dtype(rgb_out), subject to the output tuple representability precondition. Intermediate unbounded mathematical values do not fail merely because an approximate Float64 evaluation would overflow; source/destination finiteness is a caller precondition and host resource limits remain operational checks.

On direct selection or identical-color paths, skip D/E. With positive alpha, convert each stored straight RGB component directly with one destination rounding. All versions correctly round these direct paths exactly; the four-ULP allowance applies only to actual mixed RGB. Zero-alpha canonicalization and the shared output-tuple precondition still apply. Both selected colors remain dependencies on the identical-color shortcut.

### Numeric profiles

The two accelerated CPU profiles permit a final error of at most four FP32-scaled representable steps per final RGB or alpha component. Exact selected endpoints and zero-alpha cases retain exact semantics; ordinary alpha arithmetic also preserves its [0,1] constraint. Required special values, alpha constraints and output representability use the strict reference.

If exact output alpha is positive but its correctly rounded destination alpha is zero, every correctly rounded RGB component must also be zero. This output-tuple representability condition is an unchecked caller precondition. A valid all-zero result becomes canonical transparent black. The accelerated allowance cannot change this classification.

Direct selection/constant-color paths preserve the signs of converted RGB zeros. Actual mixed-color exact zeros are +0; nonzero exact underflow retains its sign. All zero alpha values and transparent black components are canonical +0. The transparent-black rule takes precedence over direct RGB zero-sign preservation.

For standard sRGB black and white, the half-way linear-light color encodes to approximately 0.735 per component, rather than 0.5. This is an illustrative mathematical value, not a tolerance.

## Behavior

The final channel axis is a complete RGB/RGBA color; every selected output tile includes all components. An output color depends on its query value and the selected one or two stop rows (all stops and colors are the dependency support). Region and tile results equal the corresponding slice of the whole result.

The caller guarantees finite strictly increasing stop values; repeated positions violate that precondition and this operation has no coincident-stop hard-transition rule. Interpolation is between adjacent distinct stops. Static out_of_domain is clamp or reject, default clamp. Outside the closed stop domain, clamp selects the nearest end color; reject requires the query to lie in the closed stop domain. There is no color extrapolation mode. With K=1, clamp returns that color for every finite input position, while reject accepts only a position numerically equal to the stop. The input positions are still dependencies in the constant case.

Exact hits, clamp and K=1 use one stop color and directly convert dtype without transfer decode/encode. If both selected stop colors have identical complete stored bit patterns, the same direct conversion preserves the constant color without a transfer round trip, even near the standard sRGB segment thresholds; both complete colors remain dependencies. Generic unselected colors are numerically unused. Transparent canonicalization and the representable-output association precondition apply on direct paths.

Finite ordered stops, finite queries, selected complete-color model/association validity (finite colors, alpha in [0,1]), reject-domain membership and representable final output are unchecked caller preconditions. Numerical precondition violations are not runtime-error cases.

## Acceptance

RGB fixture: stops=[0,1], colors=[[0,0,0],[1,1,1]], input=[0,0.5,1], sRGB/D65/sRGB transfer gives black, RN_dtype(E(1/2)) in all middle components, and white. The strict middle bits are Float32 0x3f3c405b and Float64 0x3fe7880b5e230e4f. An independent rational midpoint comparison verifies these by testing ((encoded+0.055)/1.055)^12 against 1/32. Under gamma=2, the middle encoded component is RN_dtype(sqrt(1/2)). Under linear transfer it is 0.5. Expected values use exact rational weights and directed-precision transfer enclosures.

RGBA fixture: transparent red [1,0,0,0] to opaque blue [0,0,1,1] at w=0.5 produces straight [0,0,1,0.5]. At the transparent stop return [0,0,0,0]. Cover mixed-dtype direct paths, repeated identical bit-pattern stops, zero signs, negative/HDR components, transfer join neighbors, tiny-alpha representability, valid all-zero underflow, K=1, irregular stops and description conflicts. Premultiplied descriptions and an `output_association` parameter are rejected.

## References

- [Color management category](../../02-format-color/representation.md).
- [Operator specification template](../../00-foundation/spec-template.md).
