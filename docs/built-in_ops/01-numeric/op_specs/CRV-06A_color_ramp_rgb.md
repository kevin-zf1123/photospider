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
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06A: color_ramp_rgb

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registry/platform identity,
floating environment, bounded execution and public acceptance. The new color-array
descriptor, complete-color observation, alpha and precision rules below override
the baseline's generic empty-facet/per-component conventions. Clarification of
this primitive is complete; its shared color-management representation remains
an explicit Proposed contract implemented by the current runtime.

## Confirmed color domain and operation

Use explicit stops[K] and colors[K,C] to map scalar input positions to colors,
with output shape input.shape+[C]. This RGB implementation is separate from the
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
the output authoring default is premultiplied. Premultiplied storage means encoded
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

Stop values are finite and strictly increasing. Repeated stop positions are
rejected; this version has no coincident-stop hard-transition rule. Interpolation
is between adjacent distinct stops. Static out_of_domain is clamp
or reject, default clamp. Outside the closed stop domain, clamp selects the
nearest end color and reject fails. There is no color extrapolation mode.

Require 1<=K<=65536. With one stop, clamp returns that color for every finite
input position, while reject accepts only a position numerically equal to the
stop. Requested input positions remain read/validated even in the constant case.
Input, stops and colors independently accept Float32/Float64; the output dtype
selects Float32/Float64 and defaults to colors' dtype. Output appends C=3/4, so
input rank must permit the extra axis under the current positive rank-1..8
Value limit. Total logical input/color/output products retain the 2^40 cap.

## Confirmed color observation granularity

The final channel axis is a complete RGB/RGBA color. Any channel request closes
to a complete color, and a nonempty request evaluates the full Whole output.
All input arrays are collected and typed-validated; stop order and every finite
position are checked before selected-row color mathematics. Alpha and finite
constraints apply to all components of each mathematically selected color.
Exact stop hits, clamp and K=1 use one stop color and directly
perform necessary dtype/association conversion, without transfer decode/encode.
Unselected stop colors are not numerically validated. Endpoint transparency
canonicalization and AssociationUnderflow remain applicable to this direct path.

If both selected stop colors have identical complete stored bit patterns, use
the same direct dtype/association conversion to preserve the constant color.
Still read and validate both colors and retain their dependencies. Do not perform
a needless transfer round trip even near the standard sRGB segment thresholds.

## Confirmed description binding

A required static input color description supplies model, primaries, white,
transfer and input alpha association. Plain numeric colors are interpreted under
that explicit description. If colors already carries a recognized color-array
description, it must match; the ramp does not silently override or relabel it.
The authoring constructor may explicitly populate sRGB/D65/sRGB-transfer defaults,
but a direct node supplies its description. Output carries the resulting complete
description, preserving color space/transfer and selecting the separately declared
output association. Descriptor mismatch fails before numeric sample reads.

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
alpha constraints and complete-color numerical success/failure must agree with
strict. Use the strict path when those guarantees cannot be established; Whole fallback
counters are unavailable. Resource/cancellation failures remain explicit.

If exact output alpha is positive but its correctly rounded destination value
is zero while any correctly rounded RGB component remains nonzero, fail the
complete color with OperationFailed/AssociationUnderflow. Do not silently clear
representable RGB to satisfy zero-alpha output validity. If all RGB components
also round to zero, transparent black is valid. Accelerated classification uses
the strict-reference outcome for this decision and may not hide it within 4 ULP.

Direct selection/constant-color paths preserve the signs of converted RGB zeros.
Actual mixed-color exact zeros are +0; nonzero exact underflow retains its sign.
All zero alpha values and transparent black components are canonical +0. The
transparent-black rule takes precedence over direct RGB zero-sign preservation.

For standard sRGB black and white, the half-way linear-light color encodes to
approximately 0.735 per component, rather than 0.5. This is an illustrative
mathematical value, not a product test or final numerical tolerance.

## Confirmed color representation dependency

Output carries the [generic color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md),
supporting Float32/Float64 and arbitrary legal shapes with a final color axis.
RGB gamut uses explicit three-primary xy and white xy coordinates, with named
presets as authoring conveniences. Transfer is an independent field. No color
profile is guessed from C or encoded data relabeled as the current linear Image.

Supported initial transfers are linear, standard sRGB and pure-power gamma with
a positive finite static exponent. PQ/HLG/log encodings are outside this initial
RGB ramp scope. Finite negative and greater-than-one RGB values are supported.
For sRGB/gamma, apply the positive-domain transfer to absolute magnitude and
restore the original sign; do not clamp RGB to [0,1]. Nonfinite demanded color
data and nonfinite final outputs fail. The maintained implementation and
validation evidence are linked below.

## Complete interface and mathematical evaluation

Ordered required inputs are input[S], stops[K], colors[K,C]. Here S is an
arbitrary rank-1..7 positive shape, C=3 or 4, colors.shape[0]=stops.shape[0],
and output values has shape S+[C]. Enforce the stated K and 2^40 input/color/
output product limits using checked integer arithmetic. Position input and stops
are generic floating Values; colors may be generic or already color-described
under the explicit matching rule. All scalar positions must be finite for a nonempty Whole request.

| Static parameter | Type/domain | Constructor behavior |
| --- | --- | --- |
| color_description | Canonical String emitted by the proposed color-description helper; RGB model and explicit input association | Populate sRGB primaries, D65, sRGB transfer defaults; input alpha format remains explicit |
| output_association | String none for C=3, straight/premultiplied for C=4 | none for RGB; premultiplied for RGBA |
| dtype | String float32/float64 | Match colors dtype |
| out_of_domain | String clamp/reject | clamp |

All are explicit in direct nodes; output_association must agree with C. The
description carries transfer=linear/srgb/gamma and a positive finite gamma only
for gamma mode. It provides the numeric primaries/white, not an unresolved gamut
name. Output copies the model/color-space/transfer information and changes only
association and value dtype as specified. Unknown or mismatched descriptors,
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
qi=ai*D(ui). Validate complete source colors even on the zero-contribution path.

    A = (1-w)*a0 + w*a1
    Q = (1-w)*q0 + w*q1
    encoded = E(Q/A)                         when A>0
    rgb_out = encoded                       for straight/none
              A*encoded                     for premultiplied

At exact A=0, all output components are +0. Final alpha is RN_dtype(A);
final RGB is RN_dtype(rgb_out), followed by the explicitly selected association
underflow check, never a silent clear of nonzero rounded color. Intermediate
unbounded mathematical values do not fail merely because an approximate
Float64 evaluation would overflow; actual source/destination finite requirements
and host resource limits remain enforced.

On direct selection or identical-color paths, skip D/E. With positive alpha,
same-association RGB is converted directly; straight->premultiplied multiplies
encoded RGB by alpha, and premultiplied->straight divides by alpha, with one
destination rounding. All versions correctly round these direct paths exactly;
the four-ULP allowance applies only to actual mixed RGB. Zero-alpha canonicalization
and the shared underflow check still apply. Both selected colors remain dependencies
on the identical-color shortcut.

## Observation mapping, demand and ownership

All formal strict and accelerated keys use Whole execution. Any nonempty
request collects complete input, stops and color arrays (and both rational-hue
integer arrays when present), with complete upstream and typed validation.
The callback validates every stop, then every position, before color arithmetic.
Each position still uses exactly one hit/clamp/singleton row or two enclosing
rows mathematically. Unused generic color rows are not subjected to new numeric
domain checks; invalid typed data or upstream failures anywhere still fail.
Empty requests perform static preflight but read no sample payload.

The output is one immutable dense Value of shape input.shape+[C]. The final
channel axis retains complete-color closure, ColorArray identity and owned ICC
resources where applicable. Public fragments expose the requested complete
colors while retaining the full output owner. Arbitrary immutable input strides,
offsets and unaligned storage are supported. Owners survive context teardown.
Any input edit invalidates the complete recorded output demand. Cache identity
retains descriptors, parameters, typed validation and resource identities.
Numeric errors have Run scope; no successful color subset survives a failed
callback. Upstream, resource and cancellation errors retain their categories.

For N=product(input.shape), lookup work is O(K+N log K), plus actual exact or
certified arithmetic. Full output payload is N*C*sizeof(dtype), even for a small
requested region. Account complete collected inputs, fixed admitted arithmetic
workspace, a ResourceVector stop index with 8K element bytes plus allocator and
metadata overhead, and retained descriptors/resources. No per-output dependency
records or point-state array is retained. Work/capacity limits and cancellation
apply during scans, lookup, arithmetic and before publication; incomplete
certification fails ResourceExhausted. No reduced-precision fallback is added.

## Reference algorithm, budgets and failures

Validate stops with an optional bounded Float64 index; search each required input,
fetch/validate selected complete colors, take exact shortcuts or evaluate the
whole transfer/association expression using exact rational and directed-precision
arithmetic. Accelerated libraries require final RGB error/classification guarantees
for their declared input domains; otherwise use strict fallback. Whole does not expose fallback counters.
Finite residual or individual pow accuracy alone does not certify the whole ramp.

Whole storage and O(K+N log K) lookup follow the preceding execution contract.
The RGB fixed state additionally owns the transfer/association arithmetic arena.
Final-expression guarantees remain required for any accelerated arithmetic.

| Failure | Phase and Status |
| --- | --- |
| Missing/invalid parameters or color description | Compile/preflight; InvalidArgument / InvalidDomain |
| Wrong port shape/type/C/size or conflicting attached color description | Compile/preflight; TypeMismatch |
| Nonfinite position/color, invalid stops or reject-domain position | Evaluation; OperationFailed / InvalidDomain |
| Alpha outside [0,1] or nonzero premultiplied RGB at zero source alpha | Evaluation; OperationFailed / InvalidAssociation |
| Final RGB/alpha conversion overflow | Evaluation; OperationFailed / ArithmeticOverflow |
| Rounded alpha zero with nonzero strict-reference rounded RGB | Evaluation; OperationFailed / AssociationUnderflow |
| Backend, resource, upstream/typed, cancellation or stale failure | Preserve existing host code/reason/source/scope |

Attribute numerical failures to Run scope and include the offending port/row
when available. No partial output is published.

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
[0,0,1,1] at w=0.5 gives premultiplied encoded [0,0,0.5,0.5] by default,
or straight [0,0,1,0.5]. At the transparent stop return [0,0,0,0]. Cover
same-format and cross-format direct paths, repeated identical bit-pattern stops,
zero signs, negative/HDR components, transfer join neighbors, tiny-alpha
AssociationUnderflow, valid all-zero underflow, invalid premultiplied sources,
K=1, irregular stops and description mismatch.

Test strict bits and accelerated final ULP separately from exact selections, complete
color classification. Whole fallback counters are unavailable. Public implementation fixtures
must bind all inputs, construct the explicit descriptions/parameters, execute
through Compiler/ExecutionContext and inspect the color facet and numerical
results using actual supplied build/run commands. Validate full-color expansion,
mathematically unused generic colors and full typed validation, exact stop/input dirty effects, typed
closures, strides, low budgets, cancellation, cache-off and post-context lifetime.
The maintained ColorArray/runtime path and current validation boundary are recorded
below; the document remains Proposed.

- [Color management category](../../02-format-color/representation.md).
- [Operator specification template](../../00-foundation/spec-template.md).

## Maintained implementation and validation

Public [`color_ramp_rgb_node`](../../../../include/photospider/numeric/color_ramps.hpp)
constructs this primitive; [`color_ramps.cpp`](../../../../plugins/ops/01-numeric/color_ramps.cpp)
implements its Whole complete-output execution.

Direct, linear and gamma=2 paths use exact rational/root arithmetic.
Other gamma and sRGB paths use certified whole-expression enclosures.
Current profiles return strict bits; the accelerated RGB contract permits
up to four ULP while retaining exact alpha and failure classification.

See the [family implementation](CRV-06_color_ramp.md#maintained-implementation-and-validation),
[mathematical machinery](../math-implementation.md#crv-06-colorarray-and-color-ramps)
and [public workflow commands](../../../../examples/numeric_workflow/README.md#color-ramps)
for resource limits and actual validation.
