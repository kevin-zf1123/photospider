---
spec_schema_version: 1
id: FMT-12A
parent_id: FMT-12
function: transform_icc_profiles
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
proposed_operation_keys:
  - color.icc_transform_lcms2_19_1_cpu
---

# FMT-12A: ordinary ICC profile conversion

Inherits the [FMT-12 family](FMT-12_icc_transform_contract.md), including the frozen Little CMS 2.19.1 CPU contract. It is not a native NUM strict/accelerated primitive.

## Interface

One `input` tensor produces same-dtype `values`. One complete color group is selected using the explicit input configuration, or explicit ordered slots in raw. source_profile and destination_profile are immutable resources; both identities are explicit static parameters. An analytic source requires an explicit endpoint binding or interpretation change; a name such as sRGB does not identify arbitrary profile bytes.

ICC v2/v4 input/display/output/color-space profiles with usable directions and Gray/RGB/CMYK/XYZ/Lab endpoint models are admitted. DeviceLink belongs to B, abstract intermediates to C. intent is one of the four standard intents, default relative_colorimetric; requested bpc is off/on, default off. Absolute with on is invalid. The pinned v4 perceptual/saturation BPC behavior and fixed adaptation=1.0 apply. Actual tag/matrix paths and admitted intent fallback are recorded.

An ordinary two-profile transform is constructed with source input and destination output directions. Profile tables, TRCs and engine PCS/media-white operations are part of this operation; a profile's transfer is not predecoded through FMT-09 merely because it is encoded RGB, as that would change the source device coordinates.

execution defaults to optimized; reference is the separately fixed verification configuration. There is no automatic switch on resource failure or profile content. Identical source/destination profile bytes still execute this path.

## Behavior

The family's double planar formatters and explicit unit conversion/rounding apply. Public Float32/Float64 dtype is preserved; it does not specify internal precision. Native normalized Lab lightness and CMYK ink units are kept at public ports. The family finite/range rules apply, including raw's finite-only engine boundary.

Same-count conversion retains ordered source slots. Different counts replace the source group at its smallest index with consecutive target components and remap alpha/AOVs. Callers pass destination profile-defined configuration or its compatible analytic binding explicitly to later consumers. No output carries a description. Images remain planar and straight.

Any requested target color component depends on all source colors at that point; bypass depends only on its own source. Hidden color does not depend on alpha. Empty/bypass-only observations still require valid static parameters/resources. auto/materialize produce the requested output; forced view is invalid.

## Acceptance

- A fixed RGB source and CMYK destination with RGBA data: output has CMYK plus unchanged alpha. Verify group position, exact alpha bits and a direct-CMM oracle under both execution settings. Reversing profiles need not recover RGB.
- Fixed profiles missing a dedicated relative intent table but supporting the admitted fallback: construction succeeds and records the actual path; an unusable reverse direction fails instead of inventing an inverse.
- Requested bpc=off with v4 perceptual follows the engine's effective rule; absolute/on fails statically. Profile header intent does not replace A's explicit/default intent.
- Same-profile Float64 input is still evaluated, including formatter narrowing; it does not bypass to an identity view. Unrequested alpha NaN does not fail a color request; unrequested color NaN does not fail alpha-only observation.
- Lab l=0.5 enters the formatter as L*=50; CMYK 0.5 enters as 50 percent. Independent unit fixtures cover the overflow/underflow boundary.
