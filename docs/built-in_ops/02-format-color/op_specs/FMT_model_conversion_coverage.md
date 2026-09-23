---
spec_schema_version: 1
id: FMT-model-conversion-coverage
kind: specification_coverage_proposal
category: 02-format-color
status: Proposed
implementation_status: not_implemented
clarification_status: current_active_member_coverage_complete
---

# Color-model descriptions and conversion coverage

The maintainer confirmed the basis-pair and explicit-composition coverage on
2026-09-23. This proposal fills the model-level gaps in the [format/color catalog](../representation.md).
Its coverage direction is confirmed; it is not a completed member specification,
new registry API or declaration of runtime support. FMT-09..15 and FMT-18 now have member specifications, including external engines
and explicit rendering families. They remain unimplemented. The
maintainer's same-size planar image and I/O codec boundary is recorded in
[FMT codec boundary](FMT_codec_boundary.md).

## Current coverage gap

The catalog names RGB, gray, binary black/white, XYZ/xyY, CIELAB/CIELCh,
OKLab/OKLCh, HSL/HSV, YCbCr, CMYK and ACES. Naming a model does not specify its
conversion graph. Existing ColorArray v1 supplies nine model descriptions and
some NUM/CRV helpers, with last-axis and restricted alpha rules. It has no Gray,
HSV or xyY enum. Its ramp/interpolation/validation helpers are not arbitrary
source-to-target conversion operators. The old format/color runtime is removed;
FMT target specifications, including clarified FMT-09/10/11, do not implement
model transforms by themselves.

Complete coverage needs all three layers:

1. A coherent description: components, units, reference, white, primaries,
   transfer, encoding and applicable profile/resource identity.
2. Explicit forward and reverse primitives where mathematically meaningful;
   parameterized reconstruction where information has been lost.
3. Documented compositions connecting the models, including every transfer,
   white-adaptation, range and profile stage. No automatic routing or implicit
   rendering is added by this proposal.

The word "handling" does not require every filter, blend or adjustment to accept
all models directly. Such an operator must declare its supported working model
and interpretation; callers explicitly convert to and from that model. Raw
component arithmetic remains possible under the shared metadata policy.

## Confirmed coverage and remaining family boundaries

| Representation | Proposed basis/pairs and allocation | Required description and remaining choices |
| --- | --- | --- |
| RGB and RGB+alpha | FMT-09 linear/encoded transfer pairs; FMT-10 linear RGB <-> XYZ and explicitly selected RGB basis/white transforms. FMT-05 edits alpha separately. | Primaries, white, transfer, scene/display reference, units and numeric encoding. Named RGB presets need a fixed list/version; RGB alone is incomplete. Every admitted preset needs an explicit route to/from the hub. |
| Gray | FMT-11 linear RGB or XYZ -> relative luminance Gray; Gray -> neutral XYZ/RGB under an explicit white. Q/R admit linear Y, encoded luma, normalized CIELAB l and OKLab L; paths through RGB remain explicit. FMT-09 owns applicable scalar transfer. | Distinguish linear Y, encoded Y', normalized l=L*/100, OKLab L and arbitrary scalar channels; metadata retains white, scale and reference. Neutral reconstruction cannot recover discarded chroma. Component extraction/relabeling in FMT-01/08 is not colorimetric gray conversion. |
| Black/White | FMT-11S composes explicit MASK-03 hard threshold; T expands exact 0/1 to declared Gray levels. RGB reconstruction composes R and inverse model/basis nodes. GRD-29/30 supply separate dither/halftone. | Binary Gray-origin semantics are separate from a coverage mask. No implicit 1-bit storage; codec owns bit packing. Binarization is lossy; expansion does not reconstruct the original tone. FMT-06 owns numeric codes. |
| XYZ | FMT-10 linear RGB <-> XYZ, with explicit white adaptation; FMT-11 XYZ <-> Lab, OKLab and xyY as separately admitted members. | Relative/absolute units, reference white and normalization. An arbitrary XYZ white cannot be fed to a fixed-white model without an explicit adaptation stage. |
| CIELAB / CIELCh(ab) | FMT-11 XYZ <-> CIELAB and CIELAB <-> CIELCh(ab). | Explicit source white; normalized l=L*/100 with unchanged a*/b*/C* scale; signed principal atan2 hue, exact-zero hue=0 and finite extensions. "LCh" must name its underlying model. |
| OKLab / OKLCh | FMT-11 reference XYZ <-> OKLab and OKLab <-> OKLCh; RGB routes through the explicit FMT-10 basis path. | Fixed W3C 2026-09-13 forward matrices with exact inverses; relative D65, real signed cube roots, principal polar hue and finite extensions. Opponent scales remain distinct from CIELAB. |
| HSL | FMT-11 described RGB <-> HSL. | Explicit linear/encoded relative RGB interpretation; radian/pi_multiple hue, exact achromatic H/S=0, algebraic finite extension and requested-saturation singularity checks. HSL lightness is not generic luminance Gray. |
| HSV / HSB | FMT-11 described RGB <-> HSV, with HSB confirmed as authoring terminology canonicalized to HSV. | Canonical serialized model is HSV; HSB is an authoring alias. V=max(R,G,B), six-sector hue and finite algebraic extension with explicit singularity behavior; no duplicate HSB mathematics. |
| YCbCr | FMT-11 described RGB' <-> same-size YCbCr coordinate conversion. FMT-09 handles transfer and FMT-06 handles storage/range encoding. | All three planes have the same shape and sample grid inside the DAG. M/N fix explicit BT.601/709/2020 NCL or custom coefficients and zero-centered chroma; constant luminance is a separate future member. Codec owns external 4:2:2/4:2:0 reconstruction/subsampling and siting. |
| CMYK | FMT-12 profile-directed CMYK <-> admitted RGB/Gray/PCS (XYZ/Lab) paths, according to the selected engine and profiles. FMT-18 handles proofing. | Profile/printing condition, ink units and direction-specific intent/BPC. Four channels alone do not determine color. No unprofiled universal CMYK <-> RGB formula is claimed. A future algebraic "generic CMYK" would need a separately named, explicitly non-device-color definition. Profile transforms need not be invertible or recover original ink separation. |
| ACES2065-1 and ACEScg | FMT-10 AP0/AP1 linear RGB bases and explicit conversion routes; applicable white/reference behavior remains explicit. | These are named RGB encodings, not another interchangeable model enum. Pin definitions and distinguish ACES white from D65. |
| ACEScc and ACEScct | Named FMT-09 encoding/decoding members on the prescribed RGB basis, composed with FMT-10 as needed. | Pin formulas/version, piecewise domains and noninvertible branches. Supporting an AP1 primary preset alone does not support these encodings. |
| ACES input/output rendering and transport | Configured FMT-13 processors or separately specified FMT-15 rendering workflows; FMT-06 plus I/O codec for explicitly admitted transport code/packing recipes such as ACESproxy. | A complete ACES workflow requires its version, input/output transforms and target conditions. An I/O file decoder is not automatically an ACES color input transform. Named encodings do not imply adoption of every ACES transform or transport format. |

xyY was already identified as a catalog gap and remains in this proposal even
though it is not repeated in the maintainer's latest model list. Its zero-Y,
zero-denominator and inverse policies are fixed in FMT-11O/P.

## Connecting the model graph

A proposed explicit core is:

- encoded RGB <-> linear RGB <-> XYZ <-> CIELAB <-> CIELCh(ab);
- reference XYZ <-> OKLab <-> OKLCh;
- described RGB <-> HSL / HSV;
- described RGB' <-> full-resolution YCbCr;
- linear RGB/XYZ -> defined Gray -> Black/White, with explicit neutral/two-level
  reconstruction in the reverse direction;
- CMYK <-> profile-directed PCS/RGB paths;
- ACES named RGB bases and transfers reuse the corresponding RGB edges.

For example, HSL -> CIELCh is a declared HSL-to-RGB stage, the RGB transfer
inverse, an RGB-to-XYZ transform, any explicitly selected white adaptation,
XYZ-to-Lab and Lab-to-LCh. CMYK -> OKLCh first requires a profile transform to an
explicitly described intermediate. No all-pairs family, implicit choice of
intermediate space, hidden gamut compression or unified automatic converter is
required to provide coverage.

Every edge must say whether it is a numerical inverse, a paired conversion with
rounding loss, a many-to-one mapping, or a reconstruction requiring extra
parameters. Unavailable inverse information must not be fabricated. Request
support follows the actual component dependence; alpha remains straight and
same-tensor, and unrelated alpha/AOV-only requests bypass color arithmetic.

## Runtime acceptance and separately scoped extensions

- Native member allocation and Gray/HSB vocabulary are confirmed in
  [FMT-11](FMT-11_model_conversion_contract.md). [FMT-12](FMT-12_icc_transform_contract.md) now fixes ICC v2/v4 admission,
  Little CMS 2.19.1 CPU, ordinary/DeviceLink/chain members and optimized/reference
  execution; concrete user rendering/transport resources remain explicit inputs, not guessed defaults. The
  [relative-coordinate scale](FMT_relative_coordinate_scale.md) revision also
  requires explicit NUM/CRV metadata migration.
- [FMT-09 transfer](FMT-09_transfer_contract.md) is now clarified with A/B member
  specs and ten named curves; [FMT-10 basis/adaptation](FMT-10_rgb_basis_contract.md)
  is also clarified with native A/B/C and composite D. Both remain unimplemented.
  [FMT-11](FMT-11_model_conversion_contract.md) now specifies A-T and FMT-12
  specifies the ICC engine contract. [FMT-13](FMT-13_ocio_transform_contract.md)
  specifies OCIO v2.5.2 CPU A-F, with fixed resources/properties and an F32 engine
  boundary. [FMT-14](FMT-14_gamut_mapping_contract.md) specifies native gamut mapping,
  [FMT-15](FMT-15_tone_view_contract.md) tone/units and explicit view composition,
  and [FMT-18](FMT-18_softproof_contract.md) ICC proofing/alarms. BT.2020 CL remains
  a future member. All new conversion members
  remain unimplemented.
- For every model, record forward/reverse prerequisites, units/ranges, singular
  cases, exact rounding or external-engine tolerance, metadata updates, exact
  demands/dirty support, cancellation/resources and failure behavior.
- Cover each admitted model with independently checked reference coordinates,
  explicit composition workflows and applicable round-trip tests. Lossy Gray,
  binary, gamut/tone and profile paths require loss/reconstruction assertions,
  not bitwise round-trip promises. Swizzling or changing metadata is not a
  conversion oracle.

## Primary references and limits

[W3C CSS Color 4 conversion material](https://www.w3.org/TR/css-color-4/#color-conversion-code)
provides reference material for RGB, XYZ, Lab/LCh and OKLab/OKLCh conversions.
Its CSS syntax, clipping, default white and rendering choices do not become
Photospider defaults. [ACES encoding overview](https://docs.acescentral.com/encodings/overview/)
distinguishes linear AP0/AP1 encodings, ACEScc/ACEScct transfer characteristics
and transport encoding. These references identify definitions to pin during
member clarification; the individual member contracts fix their adopted definitions and limits.
