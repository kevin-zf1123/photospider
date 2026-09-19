---
spec_schema_version: 1
id: FMT-COLOR
kind: shared_data_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# FMT-COLOR: generic color-array description

## Confirmed need and scope

The maintainer selected a generic color-array description so color ramp outputs
carry their color interpretation to downstream consumers. Support Float32 and
Float64, arbitrary legal array shapes with the last axis holding color channels.
This is a Proposed shared data contract required by CRV-06, not an operation key
or an implicit change to current Image/Layer semantics.

The description must express color model, gamut/primaries, reference white and
transfer explicitly where relevant, along with the model-specific channel/alpha
information below. RGB ramp defaults are sRGB primaries, D65 and the standard
piecewise sRGB transfer; this authoring default does not change existing linear
Image/Layer defaults. This initial representation clarification is complete;
the maintained encoding/helpers, resource ownership and operator integration are recorded
in the implementation section below; this contract remains Proposed.

ColorArray rank is 2..8, all extents positive and logical elements <=2^40. The
last axis contains the model channels; one color is [1,C]. One immutable static
description applies to the whole array. Per-element color-space switching is
not supported. The full-color observation domain excludes the channel axis and
therefore has rank 1..7. Arbitrary strides do not change that logical layout.

## Current implementation boundary

[SemanticDescriptor](../../../../include/photospider/data/semantic.hpp) currently
describes typed Image as Float32 HWC, while
[semantic validation](../../../../src/lib/data/semantic.cpp) restricts Image RGB
to primaries=srgb and transfer=linear. Generic Value shapes and arbitrary facet
storage do not by themselves establish a validated generic color-array contract.
The current implementation supplies explicit metadata validation, ownership, propagation,
cache identity and operator-consumer rules described in the maintained section below.

This specification does not silently label encoded RGB as linear RGB, expand a
legacy semantic enum at runtime, or claim an existing assign/convert operation
accepts this new representation.

## Maintained implementation and validation

The `ops-impl` branch now provides the independent public
[`ColorArrayDescriptor` codec](../../../../include/photospider/data/color_array.hpp),
exact white/basis checks, regional sample validation and generic NUM full-color
Validation closure. These facilities use the canonical bytes defined below and
keep Image/SemanticKind separate. Static rational split descriptions remain
parameter-only. The manual [ColorArray example](../../../../examples/numeric_workflow/README.md#colorarray-facilities)
checks public workflow composition, source support/dirty behavior, snapshot and
fragment boundaries, and per-observation dependency proofs. It is excluded from
CTest and integration registration.

The explicit `IccProfile`/`ResourceBindings` path now resolves profile identities
at compilation and publication, retains accepted bytes in Values/fragments and
snapshots, and carries owners through ordinary, dependency and structured
execution queries. Empty output retains the required profile. The codec's
`ColorProfileIdentity` remains metadata only; a CMYK Value requires the matching
owning binding. See the [public ICC manual](../../../../examples/numeric_workflow/README.md#immutable-icc-resources)
for editable workflows, independent structure/identity fixtures and resource
failure checks. Resource-bearing results bypass optional sample-only caches.
Color ramp arithmetic is implemented; the broader family acceptance matrix is
tracked separately. This document does not claim the entire category is complete.

## Confirmed RGB primaries and white representation

Represent the RGB gamut by explicit xy chromaticities for red, green and blue,
plus an explicit white xy pair. Named presets fill these numbers; custom
coordinates are supported. Transfer is separate from the primaries/white data.
Neither an RGB name nor equal channel count substitutes for those values.
The selected built-in primary presets are sRGB, Display P3, Rec.2020,
Adobe RGB (1998), ProPhoto RGB, ACES AP0 and ACES AP1. Each preset supplies its
defined default white; transfer remains separately explicit and is not switched
implicitly by a primary preset.

White xy must be finite with x>0,y>0 and exact x+y<1, yielding positive XYZ when
normalized to white Y=1. Primary xy may be virtual/imaginary, but the resulting
RGB-to-XYZ matrix must be invertible. Check exact coordinate relationships rather
than approximate determinant thresholds. Use homogeneous primary columns
(x,y,1-x-y), so a primary y=0 is not inherently invalid. Solve for column scales
against white (xw/yw,1,(1-xw-yw)/yw); both the primary basis and scaled matrix
must be nonsingular. Negative primary y or negative normalization scale is not by
itself invalid, as needed for AP0. A near-singular but exact invertible matrix is
not silently rejected by an invented condition-number cutoff; downstream numeric
budgets/overflow remain explicit. A malformed white/basis is a descriptor error.

Preset validation must allow the defined imaginary primaries: for example,
[ACES AP0](https://docs.acescentral.com/encodings/aces2065-1/) has blue xy
(0.00010,-0.077), and AP0/AP1 use white (0.32168,0.33767), as also documented
by [ACEScg](https://docs.acescentral.com/encodings/acescg/). Do not reject them
by requiring every primary coordinate to lie in [0,1]. White selection is not
implicitly forced to D65 for every preset.

## Confirmed initial transfer scope

RGB and related three-component colors explicitly carry reference=scene_relative
or display_relative. It is required metadata; the default sRGB authoring helper
fills display_relative. Ramp/LUT execution never infers exposure, tone mapping
or luminance adaptation from this label. CMYK's device/reference conditions
remain defined by its bound ICC profile rather than a fabricated RGB reference.

Support linear, the standard piecewise sRGB transfer and parameterized pure-power
gamma. Gamma is a static positive finite numeric parameter. PQ, HLG and log
encodings are outside this initial dependency scope and require separately defined
scene/luminance parameters and conversion specifications. Alpha is not a color
transfer channel; RGBA association/encoding must be explicit. The precise
supported RGBA association values are straight and premultiplied. Premultiplied
means alpha times encoded RGB under the specified transfer, not transfer encoding
of an already premultiplied linear value. A consumer must restore straight encoded
RGB before decoding when alpha is nonzero. RGB ramps declare input/output formats
independently and default output to premultiplied. Existing linear-only Image/Layer
contracts are unchanged by this proposed general encoded representation.

For RGBA, alpha is finite in [0,1]. At zero alpha, premultiplied storage requires
zero RGB; straight storage may contain finite hidden RGB. A consumer's transform
may impose stronger output rules: the selected RGB ramp emits zero RGB whenever
its output alpha is zero, in either output association. This does not redefine
every straight source array as having zero hidden color.

## Confirmed CIE and OK coordinates

XYZ records relative X,Y,Z coordinates with reference-white Y=1 and explicit
white xy, default D65, optionally D50/custom. It allows finite signed/HDR values.
Absolute luminance and white-Y=100 data require explicit conversion. XYZ ramp,
same-model 3D LUT application and baking carry this description; no RGB transfer
or primaries are implied by an XYZ value. Initial color tuples have no alpha.

CIELAB uses L*,a*,b* and an explicit reference white, default D50. CIELCh(ab)
uses the same L* and white with nonnegative C* and hue. OKLab and OKLCh use
their own coordinate scales and fixed D65; arbitrary white replacement is not
accepted. These four initial ramp models have three channels and no alpha.
All lightness and Cartesian opponent components may be finite extended values;
chroma is finite and nonnegative. Nominal lightness ranges are not clipping rules.

Polar color descriptions record hue unit explicitly. Floating storage uses
radian or pi_multiple; the rational ramp source interface uses separate Int64
numerator/positive-denominator arrays coordinated with its two L/C columns.
That two-column array alone is not a complete ColorArray. Outputs always contain
three floating channels and record the selected floating hue unit. The maintainer requires original hue values without normalization for CIELCh,
OKLCh and HSL. No canonical one-turn storage range is imposed. These ramps interpolate the original hue difference and retain winding count;
no circular-path parameter is provided. C=0 does
not erase or invalidate stored hue. See the complete
[CIELCh contract](../../01-numeric/op_specs/CRV-06D_color_ramp_cielch.md) and
[OKLCh contract](../../01-numeric/op_specs/CRV-06F_color_ramp_oklch.md).

## Confirmed HSL and YCbCr descriptions

HSL records its underlying RGB primaries, white and transfer, defaulting to
sRGB/D65/standard sRGB transfer in authoring helpers. Its channel order is H,S,L,
with explicit radian/pi-multiple hue. Rational source hue is represented by
coordinated integer numerator/denominator ports. All S/L finite extensions are
allowed in the ramp; hue remains original, unnormalized and active at S=0.
The initial form has no alpha. Future HSL conversion operators must explicitly
define their extension for out-of-reference-range S/L; the ramp does no conversion.

YCbCr records underlying RGB primaries/white/transfer and an NCL matrix, using
Kr,Kb with positive coefficients and sum<1. BT.601, BT.709 and BT.2020 NCL are
matrix-only presets; custom coefficients are supported. Coordinates are unbiased
floating Y',Cb,Cr, nominally [0,1],[-0.5,0.5],[-0.5,0.5], with all finite
extensions allowed. Initial ramp output has three full-resolution components,
no alpha and no integer full/limited-range code interpretation. See
[YCbCr ramp](../../01-numeric/op_specs/CRV-06H_color_ramp_ycbcr.md).

## Confirmed CMYK profile resource

CMYK ramp descriptions reference a specific immutable printing ICC profile
resource, frozen at compilation and identified by content. Profile changes
require recompilation, even when the path string is unchanged. Repeated workflow
execution may change stop/color values while the profile identity stays fixed.
Output descriptions retain that identity and the backing resource ownership
needed by downstream color-management consumers. An unowned path or process-local
pointer is not a complete color-array profile description.

The initial CMYK ramp accepts only ICC v2/v4 output-device profiles (prtr) with
CMYK data space. Reject RGB, DeviceLink and ICCmax resources for this operation.
Freeze and validate profile format/content before numeric evaluation, and retain
an owning resource reference in compiled/output state. ICC resource bytes and
validation work are host-accounted, not an unbudgeted path-based cache. This
resource binding and persistence of ownership beyond context lifetime require
explicit implementation support in the new color-array representation.

## Complete model fields and validity

Every description identifies model, ordered roles/units, reference and alpha
association as applicable. The following model-specific fields are mandatory;
irrelevant fields are absent, not silently interpreted as defaults.

| Model | Channels | Required model fields / numeric domain |
| --- | --- | --- |
| rgb | R,G,B or R,G,B,A | Primaries xy, white xy, transfer; finite signed/HDR RGB; alpha rules above |
| xyz | X,Y,Z | White xy, relative white-Y=1; finite signed/HDR |
| cielab | L*,a*,b* | White xy; finite extensions, nominal L*=0..100 |
| cielch | L*,C*,h | White xy, floating hue unit; finite L*, C*>=0, finite original hue |
| oklab | L,a,b | Fixed D65; finite extensions, nominal L=0..1 |
| oklch | L,C,h | Fixed D65, floating hue unit; finite L, C>=0, finite original hue |
| hsl | H,S,L | Underlying RGB primaries/white/transfer, floating hue unit; all finite extensions |
| ycbcr | Y',Cb,Cr | Underlying RGB description, NCL Kr/Kb and unbiased coordinate scale; finite extensions |
| cmyk | C,M,Y,K | Frozen ICC v2/v4 CMYK prtr resource; each ink in [0,1]; no alpha |

Only RGB initially supports alpha. Three-channel RGB uses association=none;
RGBA uses straight/premultiplied with the encoded-association definition above.
All other initial models have association=none. Three-component reference is
scene_relative/display_relative; CMYK uses profile-defined device reference.
Floating hue is radian/pi_multiple with no modulo canonicalization of sample
values. Negative zero samples remain governed by the producing operation, not
by descriptor serialization. All complete color samples are validated together
when their operation observes any of their channels.

Required transfer identity is linear, srgb or gamma, with gamma's positive finite
Float64 exponent only for gamma. sRGB uses the exact piecewise constants/exponents
and odd signed extension in CRV-06A. Gamma uses sign(c)*abs(c)^gamma to decode
and its mathematical inverse to encode. No implicit transfer applies to XYZ/Lab.
No absolute luminance parameter or PQ/HLG/log transfer is inferred by this version.

## Named primary/white presets

Authoring helpers fill explicit Float64 xy values by nearest/ties-even conversion
of these decimals. Preset spelling is not semantic identity: custom coordinates
with the same canonical values have the same interpretation. Transfer remains
separate. This lists primaries/white only, not complete named RGB encodings.

| Preset | Red xy | Green xy | Blue xy | White xy |
| --- | --- | --- | --- | --- |
| srgb | .64,.33 | .30,.60 | .15,.06 | .3127,.3290 |
| display_p3 | .68,.32 | .265,.69 | .15,.06 | .3127,.3290 |
| rec2020 | .708,.292 | .170,.797 | .131,.046 | .3127,.3290 |
| adobe_rgb_1998 | .64,.33 | .21,.71 | .15,.06 | .3127,.3290 |
| prophoto_rgb | .734699,.265301 | .159597,.840403 | .036598,.000105 | .3457,.3585 |
| aces_ap0 | .73470,.26530 | 0,1 | .00010,-.077 | .32168,.33767 |
| aces_ap1 | .713,.293 | .165,.830 | .128,.044 | .32168,.33767 |

Named D65 and D50 helpers use (.3127,.3290) and (.3457,.3585), respectively;
ACES white is its stated pair, not an implicit substitution by D65. XYZ white
coordinates are derived with Y=1 from the stored xy as exact ratios. No bitwise
equivalence to older image descriptors with separately rounded XYZ white triples
is claimed. Basis validation includes AP0's imaginary primary and all custom
zero-y primary cases permitted by the homogeneous representation.

Source coordinates: [W3C CSS Color 4 predefined spaces](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/#predefined),
[ACES2065-1](https://docs.acescentral.com/encodings/aces2065-1/),
[ACEScg](https://docs.acescentral.com/encodings/acescg/).
These identify preset data, not adoption of CSS interpolation or an ACES transform.

## Canonical encoding and authoring helpers

Proposed ValueFacet key is photospider.color-array, version 1, with canonical
bounded payload <=4096 bytes. Provide owned encode/decode/parameter helpers
analogous to existing semantic helpers; no caller hand-assembles bytes or manual
hex. The static color_description String is the canonical lowercase hexadecimal
payload, <=8192 bytes, emitted by the helper. This is a new facet contract and
helper implementation; do not pass it to existing semantic-v1/image-v2 decoders.

Use an explicit model-tagged fixed-field binary record, with little-endian fixed
width integers and IEEE binary64 bit encodings for numeric metadata. Model tags
are rgb=1,xyz=2,cielab=3,cielch=4,oklab=5,oklch=6,hsl=7,ycbcr=8,cmyk=9;
reference tags scene_relative=1,display_relative=2,profile_relative=3;
association none=0,straight=1,premultiplied=2. The prefix is four UInt8 values:
model, reference, association, source_layout (0 interleaved,1 rational_hue_split).
Channel roles/units are determined by model, association and hue-unit tags;
no redundant unconstrained role strings are encoded.

Following the prefix, all non-CMYK models contain white xy (two Float64).
RGB/HSL/YCbCr additionally contain primary xy in R,G,B order (six Float64),
then transfer UInt8 (linear=0,srgb=1,gamma=2) and a Float64 exponent only for
gamma. CIELCh/OKLCh/HSL then contain hue UInt8 (radian=0,pi_multiple=1,
rational_pi=2). YCbCr then contains Kr,Kb as two Float64; its fixed unbiased
range is implicit in version/model. CMYK instead contains UInt64 profile byte
length and a 32-byte content identity as described below. All inapplicable fields
are omitted, and no trailing bytes or unknown tags are accepted. Only CMYK uses profile_relative;
all other models require scene_relative or display_relative. Only RGB accepts
a nonzero association tag. Contradictory color-interpretation facets are rejected
unless an explicit adapter has established a single consistent description. Metadata signed
zeros are canonicalized to +0, never sample payloads. Reject noncanonical encodings
on decode rather than silently repairing stored payloads.

Runtime ColorArray requires source_layout=0 and floating hue codes 0/1 only.
The rational ramp authoring wrapper uses source_layout=1 and hue=2, only for
CIELCh/OKLCh/HSL. It describes coordinated split source ports and is legal only
as the static source parameter; it cannot be attached to the two-column L/C or
S/L Value as a complete ColorArray. Output encoders always use interleaved layout
and the selected floating hue unit. This distinction resolves static rational
input descriptions without inventing a rational sample dtype.

For YCbCr, matrix presets populate Kr/Kb by correctly rounding the documented
decimal coefficients to Float64; the actual stored numeric values define the
custom/preset descriptor alike. No preset name silently replaces them with a
different rational matrix. Ordinary ramp arithmetic does not execute the matrix.
Exact metadata comparisons cover all canonical fields, including reference,
white, hue unit, association and profile identity; dtype/shape are checked separately.

Canonical byte fixture: XYZ, display_relative, no alpha, interleaved layout,
D65 xy=(RN64(0.3127),RN64(0.3290)) has the 20-byte payload
`0202000088635ddc4603d43f75931804560ed53f`. The first four bytes are the
model/reference/association/layout tags, followed by the two little-endian
binary64 coordinates. Its facet key is photospider.color-array and version is 1;
the helper's static String is exactly the lowercase hex above. No primary,
transfer, hue or profile fields may be appended to this XYZ record. Future codec
acceptance compares exact bytes and rejects trailing fields independently of
sample dtype/shape validation.

## ICC resource ownership and admission

Profile content identity is SHA-256 plus exact byte length, computed after loading
the explicitly bound immutable resource. Matching identity is a lookup aid; a
resolver must retain the actual accepted bytes and reject conflicting content for
an existing identity, rather than allowing a pathname substitution. The resource
must have checked declared length, acsp signature, v2/v4 version, prtr class and
CMYK data space, valid XYZ/Lab PCS and a structurally valid bounded tag directory.
Check all offsets/sizes/count arithmetic against the frozen bytes, required
profile-class structures and permitted shared tag data under the selected ICC
version. Do not execute transform tables merely to interpolate CMYK components.

The metadata digest is not ownership. Compilation/output bindings retain an
immutable budget-accounted profile resource, accessible after execution-context
teardown while a result/read view still refers to it. Missing/unresolved resources
fail admission; bytes cannot be loaded later from a mutable path based only on
metadata. The current ResourceBindings resolver and owner integration implement that resource
binding; metadata alone still does not grant ownership. Profiles,
tag-validation scratch and content-identity computation are bounded by host budgets.

## Integration, errors and acceptance

Compilation checks facet schema, model/shape/dtype, matching operation declarations,
required immutable resources and static numeric metadata. Evaluation validates
the complete colors actually required by the operation's observation/support
contract; never scan all sample payload just to decode a descriptor. Broader
typed/upstream closure remains explicit. Metadata cannot silently reclassify a
NaN/Inf numeric array as valid finite color data.

Color-preserving operations forward the full owned descriptor. Model/space-changing
operators construct an explicit destination description; generic NUM operations
continue their already specified facet behavior and do not automatically perform
color conversion. Copying metadata is not permission to skip numerical legality.
Current Image/Layer representations and their existing helpers remain unchanged;
adapters require explicit shape, transfer, reference and association agreement.

Malformed/oversized/noncanonical payload, invalid white/basis/transfer/matrix/profile
or unresolved resource is InvalidArgument/InvalidDomain at preflight/admission.
Incompatible dtype/shape/declared-versus-attached description uses TypeMismatch.
Demanded invalid color samples use the operation's OperationFailed/InvalidDomain
scope, with RGB alpha association failures retaining the established
InvalidAssociation/AssociationUnderflow categories. Host resource, cancellation,
stale and upstream failures are preserved. Charge metadata copies, resolver maps,
read views, reference ancestry and temporary growth; no unowned process-global
profile cache or pointer forms semantic identity.

Acceptance covers canonical round trips and rejection of old/unknown/trailing
formats, each model/role count, arbitrary strides, all presets including AP0,
singular and near-singular custom primaries, invalid white and gamma, scene/display
mismatches, hue original multi-turn values, rational-source versus runtime layout,
XYZ scale, CMYK profile classes/tag ranges/resources and context-lifetime ownership.
Test partial-channel requests validating the whole required color, unrelated
colors remaining unread, cache identity changes, metadata/resource budgets and
cancellation. Provide actual public producer/consumer workflow evidence when
implemented; these specifications do not constitute current API support.

- [CRV-06 color ramps](../../01-numeric/op_specs/CRV-06_color_ramp.md).
- [Color management category](../representation.md).
- [Operator specification template](../../00-foundation/spec-template.md).
- [Current public foundation contract](../../00-foundation/contracts.md).
