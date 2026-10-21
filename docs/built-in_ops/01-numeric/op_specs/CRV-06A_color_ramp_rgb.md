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

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registry/platform identity,
floating environment, bounded execution and public acceptance. The new color-array
descriptor, complete-color observation, alpha and precision rules below override
the baseline's generic numeric per-component conventions. Clarification of
this primitive is complete; its shared color-management representation remains
an explicit Proposed contract implemented by the current runtime.

## Confirmed color domain and operation

Use explicit stops[K] and colors[K,C] to map scalar input positions to colors,
with output shape sample_shape(input)+[C]. This RGB implementation is separate from the
CMYK/LCH/HSL/LAB/YCbCr ramps in the [family contract](CRV-06_color_ramp.md).
The same RGB operation supports colors[K,3] for RGB and colors[K,4] for RGBA;
the color description explicitly declares alpha presence. Other color models
remain separate implementations. Output appends the same color-channel arity.

Explicitly specify RGB primaries/gamut, white point and transfer function.
Defaults are sRGB primaries, D65 and the standard piecewise sRGB transfer;
gamma 2.2 is not the selected default. Input colors and output colors use the
same specified encoding and color space. Interpolation is fixed in linear-light
RGB: decode color values, interpolate in linear light, then encode back to that
same space. No encoded-value interpolation mode is provided.

The maintainer requires RGBA association/encoding to be explicitly declared in
the specification and color description. An unspecified four-component color
must not be treated as automatically straight or automatically premultiplied.
Input and output association are independently declared straight or premultiplied;
the output authoring default is straight. Premultiplied storage means encoded
RGB multiplied by alpha, where encoded refers to the description's transfer.
For nonzero alpha, restore encoded straight RGB before decoding, then premultiply
linear-light RGB for interpolation. After interpolation, recover straight linear
RGB, encode it, and multiply the encoded result by alpha only for premultiplied
output. With linear transfer this reduces to ordinary premultiplied linear RGB.
Alpha itself is never transfer-encoded. Zero-alpha cases are specified separately.

Alpha must be finite and in [0,1]. Premultiplied input with zero alpha requires
zero RGB. Straight input may retain finite hidden RGB at zero alpha, but it
contributes no visible premultiplied color. At zero output alpha, emit zero RGB
in either selected output association. Do not infer a hidden-color-preservation
exception at exact transparent stops.

## Confirmed stop ordering

The caller guarantees finite strictly increasing stop values. Repeated positions violate that precondition; this version has no coincident-stop hard-transition rule. Interpolation
is between adjacent distinct stops. Static out_of_domain is clamp
or reject, default clamp. Outside the closed stop domain, clamp selects the
nearest end color; reject requires the query to lie in the closed stop domain. There is no color extrapolation mode.

Require 1<=K<=65536. With one stop, clamp returns that color for every finite
input position, while reject accepts only a position numerically equal to the
stop. Requested input positions remain read, with finiteness guaranteed by the caller, even in the constant case.
Input, stops and colors independently accept Float32/Float64; the output dtype
selects Float32/Float64 and defaults to colors' dtype. Output appends C=3/4, so
input rank must permit the extra axis under the current positive rank-1..8
logical element limit. Total logical input/color/output products retain the 2^40 cap.

## Confirmed color observation granularity

The final channel axis is a complete RGB/RGBA color. Every selected output tile includes all components. A task reads its query region and conservatively declares all stops/colors tiles. Complete selected colors obey finite/alpha/association caller preconditions. Exact hits, clamp and K=1 use one stop color and directly perform dtype/association conversion without transfer decode/encode. Generic unselected colors are numerically unused. Transparent canonicalization and the representable-output association precondition apply on direct paths.

If both selected stop colors have identical complete stored bit patterns, use
the same direct dtype/association conversion to preserve the constant color.
Still read both complete colors and retain their dependencies. Do not perform
a needless transfer round trip even near the standard sRGB segment thresholds.

## Confirmed description binding

A required static `color_description` supplies model, primaries, white, transfer and input alpha association. The output association is a separate parameter. Results and Tensor members carry no attached color description. The authoring helper serializes these parameters and performs no payload conversion while constructing the node. It supplies the declared defaults; direct nodes must provide the required configuration.

## Confirmed numerical versions

Strict treats transfer decode, association conversion, linear-light interpolation,
output encode and output association as a complete mathematical expression,
then correctly rounds each final component directly to the output dtype.
There is no prescribed intermediate Float64 rounding that defines the result.
Input floats and static gamma denote their exact numerical values.

The two independently named CPU accelerated versions permit a final error of
at most four FP32-scaled representable steps per final RGB or alpha component.
Exact selected endpoints and zero-alpha cases retain exact semantics; ordinary
alpha arithmetic must also preserve its [0,1] constraint. Required special values,
alpha constraints and output representability use the strict reference. The selected implementation must establish its declared bound. Resource/cancellation failures remain explicit.

If exact output alpha is positive but its correctly rounded destination alpha is zero, every correctly rounded RGB component must also be zero. This output-association representability condition is an unchecked caller precondition. A valid all-zero result becomes canonical transparent black. The accelerated allowance cannot change this reference classification.

Direct selection/constant-color paths preserve the signs of converted RGB zeros.
Actual mixed-color exact zeros are +0; nonzero exact underflow retains its sign.
All zero alpha values and transparent black components are canonical +0. The
transparent-black rule takes precedence over direct RGB zero-sign preservation.

For standard sRGB black and white, the half-way linear-light color encodes to
approximately 0.735 per component, rather than 0.5. This is an illustrative
mathematical value, not a product test or final numerical tolerance.

## Confirmed color representation dependency

Static parameters use the [generic color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md),
supporting Float32/Float64 and arbitrary legal shapes with a final color axis.
RGB gamut uses explicit three-primary xy and white xy coordinates, with named
presets as authoring conveniences. Transfer is an independent field. No color
profile is guessed from C or encoded data relabeled as the current linear Image.

Supported initial transfers are linear, standard sRGB and pure-power gamma with
a positive finite static exponent. PQ/HLG/log encodings are outside this initial
RGB ramp scope. Finite negative and greater-than-one RGB values are supported.
For sRGB/gamma, apply the positive-domain transfer to absolute magnitude and
restore the original sign; do not clamp RGB to [0,1]. Consumed color data and final outputs are finite by caller precondition. The family links the implementation and independent oracles.

## Complete interface and mathematical evaluation

Ordered required inputs are input[S], stops[K], colors[K,C]. Here S is an
arbitrary rank-1..7 positive shape, C=3 or 4, colors.shape[0]=stops.shape[0],
and output values has shape S+[C]. Enforce the stated K and 2^40 input/color/
output product limits using checked integer arithmetic. Position input and stops
are generic floating Result tensors interpreted by the explicit parameters. Consumed scalar positions must be finite.

| Static parameter | Type/domain | Constructor behavior |
| --- | --- | --- |
| color_description | Canonical String emitted by the proposed color-description helper; RGB model and explicit input association | Populate sRGB primaries, D65, sRGB transfer defaults; input alpha format remains explicit |
| output_association | String none for C=3, straight/premultiplied for C=4 | none for RGB; straight for RGBA |
| dtype | String float32/float64 | Match colors dtype |
| out_of_domain | String clamp/reject | clamp |

All are explicit in direct nodes; output_association must agree with C. The
description carries transfer=linear/srgb/gamma and a positive finite gamma only
for gamma mode. It provides the numeric primaries/white, not an unresolved gamut
name. Output coordinates use the specified model/color-space/transfer and association without attached color metadata. Unknown or mismatched descriptors,
parameters and shape relations fail before payload evaluation.

Let D be signed transfer decoding and E encoding. linear is identity. For gamma,
D(c)=sign(c)*abs(c)^gamma and E(l)=sign(l)*abs(l)^(1/gamma). For sRGB, apply
the following exact-real positive-magnitude formulas and restore sign:

D(c) = c/12.92                         if c<=0.04045
           ((c+0.055)/1.055)^(12/5)       otherwise
    E(l) = 12.92*l                         if l<=0.0031308
           1.055*l^(5/12)-0.055           otherwise

Standard decimal constants are exact rationals for this mathematical definition;
static gamma is the exact supplied Float64 value. Correct rounding covers branch
selection and all transcendental work. No intermediate gamma reciprocal or
transfer output is rounded as part of the strict definition. Signed zeros follow
the explicit direct/mixed/transparent policies rather than a sign(0) convention.

For two selected stops, define exact w=(input-stops[j])/(stops[j+1]-stops[j]).
For RGB, treat a0=a1=1. For RGBA, ai is each stored alpha. Let ui be straight
encoded RGB: stored RGB for straight, stored RGB/ai for premultiplied with ai>0.
For ai=0 use premultiplied-linear qi=0 without decoding hidden color. Otherwise
qi=ai*D(ui). Complete selected source colors obey caller validity preconditions even on the zero-contribution path.

A = (1-w)*a0 + w*a1
    Q = (1-w)*q0 + w*q1
    encoded = E(Q/A)                         when A>0
    rgb_out = encoded                       for straight/none
              A*encoded                     for premultiplied

At exact A=0, all output components are +0. Final alpha is RN_dtype(A);
final RGB is RN_dtype(rgb_out), subject to the output association representability precondition. Intermediate
unbounded mathematical values do not fail merely because an approximate
Float64 evaluation would overflow; source/destination finiteness is a caller precondition and host resource limits remain operational checks.

On direct selection or identical-color paths, skip D/E. With positive alpha,
same-association RGB is converted directly; straight->premultiplied multiplies
encoded RGB by alpha, and premultiplied->straight divides by alpha, with one
destination rounding. All versions correctly round these direct paths exactly;
the four-ULP allowance applies only to actual mixed RGB. Zero-alpha canonicalization
and the shared output-association precondition still apply. Both selected colors remain dependencies
on the identical-color shortcut.

## Observation mapping, demand and ownership

All profiles use the [fixed execution contract](CRV-06_color_ramp.md#fixed-execution-and-caller-preconditions). Each selected complete-color output tile has one CPU task. Its reads cover the corresponding query region and all stops/colors/p/q tiles, each closed on its own source grid. Runtime selection uses one exact-hit/clamp/singleton row or two enclosing rows. Generic unselected colors and denominators are numerically unused; upstream failures still propagate through declared tile dependencies.

The `values` Result uses `photospider.tensor` v1/member `samples`, descriptor shape `sample_shape(input)+[C]`, and `atomic_trailing_axes=1`. It carries no color description or input batch-axis partition. The kernel writes only selected dense output tiles, borrowing affine source views and owning fixed arithmetic scratch. Empty emits no task or sample read. Successful call publication follows required task completion in retirement order, and output storage survives context teardown.

Finite ordered stops, finite queries, selected complete-color model/association validity, selected rational q>0, reject-domain membership and representable final output are unchecked caller preconditions. Lookup costs O(P log K) for P selected query points, plus exact/certified arithmetic; the kernel searches source stops directly without an O(K) copy. Static schemas, parameters, resources and kernel signatures remain checked. Cancellation and capacity failures stop execution without approximate substitution.

## Reference algorithm, budgets and failures

Malformed static parameters or descriptions fail preparation with InvalidArgument; incompatible port schemas, shapes or dtypes use TypeMismatch. Numerical conditions are caller preconditions, including finite destination results. Operational resource, backend, cancellation, stale-plan and upstream failures preserve their status; a failed required task prevents successful call publication.

Search the finite ordered source stops directly, fetch selected complete rows and evaluate exact rational or certified whole-expression arithmetic. Scratch is fixed per invocation; only selected output tiles are allocated. A finite residual or individual pow accuracy does not certify the complete ramp.

## Acceptance and implementation status

RGB fixture: stops=[0,1], colors=[[0,0,0],[1,1,1]], input=[0,0.5,1],
sRGB/D65/sRGB transfer gives black, RN_dtype(E(1/2)) in all middle components,
and white. The strict middle bits are Float32 0x3f3c405b and Float64
0x3fe7880b5e230e4f. An independent rational midpoint comparison verifies these
by testing ((encoded+0.055)/1.055)^12 against 1/32, without the production pow.
Under gamma=2, the middle encoded component is RN_dtype(sqrt(1/2)).
Under linear transfer it is 0.5. Use independent directed-precision transfer
enclosures and exact rational weights, not the production helper, as oracle.

RGBA fixture with straight inputs: transparent red [1,0,0,0] to opaque blue
[0,0,1,1] at w=0.5 gives straight [0,0,1,0.5] by default.
Explicit premultiplied output gives [0,0,0.5,0.5]. At the transparent stop return [0,0,0,0]. Cover
same-format and cross-format direct paths, repeated identical bit-pattern stops,
zero signs, negative/HDR components, transfer join neighbors, tiny-alpha
output-association boundaries, valid all-zero underflow, valid premultiplied sources,
K=1, irregular stops and description mismatch.

Use independent exact-rational or certified-pi/transfer references on valid inputs. Check complete-color tile coverage, mixed source grids and dtypes, unused generic rows, arbitrary source strides, concurrent Runs, cancellation, bounded scratch, allocation-free invocation and retained output lifetime. Numerical precondition violations are not runtime-error acceptance cases. Public workflows bind explicit descriptions and inspect numerical Results; the specification remains Proposed.

- [Color management category](../../02-format-color/representation.md).
- [Operator specification template](../../00-foundation/spec-template.md).
