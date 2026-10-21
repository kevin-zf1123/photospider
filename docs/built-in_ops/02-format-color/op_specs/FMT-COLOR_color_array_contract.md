---
spec_schema_version: 1
id: FMT-COLOR
kind: shared_data_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# FMT-COLOR: explicit color descriptions and numeric tuples

## Scope and implementation

NUM/CRV and FMT use the operations-SDK `TensorDescription` and its existing
canonical v4/v5 codecs. Native CIELAB and CIELCh(ab) store `l=L*/100`.
The [relative-coordinate contract](FMT_relative_coordinate_scale.md) defines
this scale, including finite negative and HDR extensions. This contract remains
Proposed; its implementation status is independent of that decision status.

Descriptions are explicit static operation parameters. Result and Tensor carry
structure, samples and storage owners; they do not carry, propagate or certify
color interpretation. Every consuming operation receives its own description.
A description identifies immutable profile/configuration resources but does not
own their bytes or cause a color transform.

The former `ColorArrayDescriptor`, `color-array-v1` codec, its validation helpers
and `photospider/ops/color_array.hpp` have been removed. No forwarding header,
old overload, legacy decoder or inferred L* scale remains. Old workflows and
bake reports must be reauthored and rebuilt. The kernel, its public data model,
workflow format, IR, planner, executor and invocation ABI are unchanged.

## NUM logical tuple and physical ports

The public numeric helpers are in
[`photospider/ops/numeric/color_description.hpp`](../../../../include/photospider/ops/numeric/color_description.hpp).
They are an operations-SDK restriction of `TensorDescription`, not another codec:

- `color_tuple_description` constructs and checks one logical color tuple.
- `resolve_color_tuple` validates it and returns its resolved color group.
- `validate_color_tuple_shape` checks a sampled floating port without reading
  samples.

The logical tuple description has `channel_axis=0`, exactly one complete group,
canonical component roles at consecutive indices starting at zero, and no
unrelated channels. It has no logical axes table, selected component, numeric
encoding or sampling record. An optional alpha is an independent final channel
outside the group's color indices; it requires an explicit channel declaration
with role `alpha` and a native unit. Alpha is straight and never transfer encoded.

This axis zero describes a tuple of shape `[C]`. NUM ports independently define
sample arrays of shape `[...,C]`, rank 2..8, positive extents and at most `2^40`
logical elements, with Float32 or Float64 storage. A single sampled color is
`[1,C]`. A ramp's colors port is `[K,C]`; LUT input, output and tables have their
own declared ranks. A rational-hue ramp instead binds two floating non-hue
columns plus separate Int64 numerator and denominator ports. Its description
still names all three logical color coordinates and uses `rational_pi` for hue.

FMT descriptions name axes of the actual tensor supplied to that FMT call.
For a CRV-produced tensor with shape `[N,3]`, explicitly construct the FMT input
description with `channel_axis=1`; for `[H,W,3]` use axis 2. Carry over the same
color interpretation and native units. This is static authoring, not an implicit
description propagation or numerical conversion. Physical affine strides remain
independent of the logical axis number.

## Interpretation and conflicts

A complete numeric group declares its model and required model fields. Global
and per-channel interpretation assertions must agree with the group wherever
both specify a value. The generic codec checks group/channel role and unit
consistency; the numeric resolver additionally checks reference, association,
white, basis, transfer, convention and model-coordinate conflicts. Empty native
units inherit the model convention; hue always has an explicit unit.

For native models, `convention="relative-v1"` and `reference="scene"` or
`"display"` are required. `association` is absent or `"straight"`; resolution
makes straight explicit. No exposure, adaptation or tone mapping follows from
the reference label. Native tuples reject ICC/configured-space and analytic
binding records. CMYK instead uses `convention="icc-native"`,
`reference="profile"` and an immutable profile identity.

Numeric tuples have no absolute-luminance or code-range encoding. Explicit
absolute-to-relative conversion and FMT-06 numeric decoding precede these
consumers when needed. The shared TensorDescription codec also represents
Gray, HSV, xyY, profile-defined and configured spaces, integer encoding and
arbitrary channel groups; that wider vocabulary does not add NUM/CRV members.

## Models, roles and native units

| Model | Canonical roles | Required interpretation and coordinate domain |
| --- | --- | --- |
| rgb | red, green, blue | RGB basis, white and linear/sRGB/power-gamma transfer; finite signed/HDR RGB |
| xyz | x, y, z | Explicit white and `coordinates.scale="relative"`; white Y=1; finite signed/HDR values |
| cielab | l, a, b | Explicit white; `l=L*/100`, unchanged a*,b*; finite extensions |
| cielch | l, c, h | Same normalized l and white; C*>=0; explicit original hue |
| oklab | l, a, b | Fixed D65; existing OKLab units and finite extensions |
| oklch | l, c, h | Fixed D65; existing L/C units, C>=0 and explicit original hue |
| hsl | hue, saturation, lightness | Underlying RGB basis, white and transfer; finite S/L extensions and original hue |
| ycbcr | y, cb, cr | Underlying encoded RGB basis/white/transfer and explicit NCL Kr,Kb; unbiased native Y',Cb,Cr |
| cmyk | cyan, magenta, yellow, black | Immutable ICC v2/v4 CMYK printing profile; each ink in [0,1] |

A native non-hue component accepts an empty unit, `"1"` or `"relative"`.
CIELAB a/b additionally accept `"a*"`/`"b*"`, and CIELCh chroma accepts `"C*"`.
These unit spellings retain the model's native coordinate, not a uniform scale
for all channels. `"L*"`, percentage, absolute-unit and range-encoding declarations
are rejected by the numeric tuple resolver. Hue is `"radian"`, `"pi_multiple"`
or, only for coordinated ramp source ports, `"rational_pi"`.

The generic numeric tuple helper can express independent alpha for these models.
Individual members impose narrower arity: CRV-06 supports alpha only for RGB;
CMYK has four ink components without alpha; other ramps have three colors.
CRV-07 and CRV-09 require same-model input/output descriptions with exactly three
color components, no alpha and floating hue. No consumer infers alpha from a
four-column tensor.

## RGB geometry, transfer and alpha

RGB geometry uses explicit primary/white xy coordinates or a supported named
basis with consistent explicit assertions. Transfer is independent. Numeric
consumers reuse the exact FMT basis/white admission implementation:
[rgb_geometry_codec.hpp](../../../../plugins/ops/common/domain/rgb_geometry_codec.hpp).
White must be finite with x>0, y>0 and exact x+y<1. Homogeneous primary columns
`(x,y,1-x-y)` and the white-normalized basis must be nonsingular. Imaginary
primaries, zero primary y and negative primary normalization scales are not
rejected merely for leaving [0,1]. No approximate determinant threshold is added.

Supported named bases are `srgb_rec709`, `display_p3`, `rec2020`,
`adobe_rgb_1998`, `prophoto_rgb`, `aces_ap0` and `aces_ap1`. Named D65 and D50
white coordinates are (.3127,.3290) and (.3457,.3585); ACES uses its stated white.
[The basis specification](FMT-10_basis_math.md) records exact preset values.

Numeric RGB/HSL accept linear, standard sRGB and positive finite power gamma.
YCbCr requires encoded underlying RGB and rejects linear transfer; Kr and Kb
must be positive with exact Kr+Kb<1. PQ, HLG, log and engine-defined transfers
require separately specified conversion and are not added to numeric ramps.
The default RGB ramp description selects sRGB/Rec.709 primaries, D65, standard
sRGB transfer and display reference.

RGB ramp inputs and outputs are straight. For RGBA, alpha is finite in [0,1].
The ramp decodes straight RGB, weights linear color by alpha, interpolates,
divides by the mixed alpha and re-encodes straight RGB. Internal alpha weighting
does not define a premultiplied storage format. Finite hidden color at zero
input alpha contributes zero; zero output alpha produces canonical transparent
black. The complete expression and representability rules are in
[CRV-06A](../../01-numeric/op_specs/CRV-06A_color_ramp_rgb.md).
There is no `output_association` parameter or `RgbRampOptions` compatibility type.

## CIE, OK, hue and NCL coordinates

CIELAB/CIELCh l is normalized, while a*, b* and C* retain their existing scales.
A nominal l interval of 0..1 is not clipping. XYZ uses one common tristimulus
scale with reference-white Y=1; X/Z are not divided by white X/Z.
OKLab/OKLCh retain their own scales and exact D65 white declaration.

Polar ramps interpolate original hue differences, including multiple turns.
They do not normalize hue, choose a shortest arc or erase hue at zero chroma
or saturation. Rational source hue uses Int64 p/q with a positive selected
denominator. Outputs contain three floating coordinates in the selected
radian or pi-multiple unit; callers explicitly author that output description
for a later consumer.

YCbCr uses full-resolution unbiased floating Y',Cb,Cr, nominally
[0,1],[-0.5,0.5],[-0.5,0.5], with finite extensions. It has no implicit integer
full/limited range, chroma subsampling or matrix execution in a ramp.

## CMYK resource ownership and admission

The CMYK ramp names an immutable ICC profile by SHA-256 content identity and
exact byte length. `color_ramp_cmyk_description` returns
`Result<TensorDescription>` and rejects an empty identity. It does not load a
path or retain profile bytes. `ResourceSet` supplies the selected immutable
resource to compilation; changing the identity requires recompilation.

CRV-06B accepts ICC v2/v4 output-device (`prtr`) profiles with CMYK data space
and valid XYZ/Lab PCS. It rejects RGB, DeviceLink and ICCmax profiles. Admission
checks the declared length, signature, version, tag directory, offsets and sizes,
required profile-class structure, permitted shared data and optional v4 Profile
ID within frozen bytes. It runs once per PreparedOperation, including Empty
calls and artifact import, with the declared 64 MiB static work limit.

Resource bytes and admission work are host-accounted. Preparation, Plan and
active execution retain the selected owner. Result retains its actual output
storage and does not inherit a profile owner. Interpolation neither executes an
ICC transform nor infers total-ink constraints. FMT-12/13/18 separately implement
their explicit ICC/configured transforms and immutable engine resources.

## Encoding, cache identity and validation

Use `tensor_description_parameter` / `tensor_description_from_parameter` for
static strings and `encode_tensor_description` / `decode_tensor_description` for
bytes. The existing TensorDescription v4/v5 discriminator and canonical rules
are unchanged by NUM/CRV migration. Unknown, noncanonical and old ColorArray
payloads fail admission; no numeric magnitude can select a legacy interpretation.

CRV-06 takes `color_description`; CRV-07 and CRV-09 take independent
`input_color_description` and `output_color_description`. The descriptions may
have different references, white/basis or transfer where the member allows
them; same-model LUT evaluation applies its authored table and performs no
implicit conversion. Output samples have no attached description.

CRV-09 hashes both canonical TensorDescription payloads under
`photospider.bake-configuration.v2`. Its owned table and report are schema v4.
Old v3 tables/reports are rejected, and old workflows must be recompiled.
The operator fixed-contract revisions invalidate old numerical preparations;
no kernel ABI or generic artifact-container revision is introduced.

Static preparation checks descriptions, shape/dtype/arity and required resources,
even when requested output is Empty. Numeric sample validity in NUM/CRV remains
an unchecked caller precondition; migrating metadata adds no eager sample scan.
Invalid metadata is `InvalidArgument`; incompatible schema/shape/dtype is
`TypeMismatch`. Resource, upstream, capacity and cancellation statuses retain
their operational meanings. FMT consumers follow their own explicit consumed
sample validation contracts.

Acceptance includes canonical round trips, contradictory assertions, old payload
and old report rejection, all models/hue/dtype variants, exact basis admission,
straight RGB alpha, normalized Lab/LCh, rational split ports, LUT axes/tables,
bake configuration identity and explicit CRV-to-FMT composition. Executed
evidence is recorded in the
[NUM/CRV migration report](../../01-numeric/NUM-CRV_color_migration.md).

- [TensorDescription runtime contract](../../../kernel-architecture/Tensor-Semantic-Metadata.md).
- [CRV-06](../../01-numeric/op_specs/CRV-06_color_ramp.md),
  [CRV-07](../../01-numeric/op_specs/CRV-07_apply_lut3d.md),
  [CRV-09](../../01-numeric/op_specs/CRV-09_bake_lut3d.md).
- [Current public foundation contract](../../00-foundation/contracts.md).
