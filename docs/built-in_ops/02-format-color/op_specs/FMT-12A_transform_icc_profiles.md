---
spec_schema_version: 1
id: FMT-12A
parent_id: FMT-12
function: transform_icc_profiles
kind: external_engine_adapter
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
proposed_operation_keys:
  - color.icc_transform_lcms2_19_1_cpu
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-12A: ordinary ICC profile conversion

Inherit the complete [FMT-12 family](FMT-12_icc_transform_contract.md), including
the frozen Little CMS 2.19.1 CPU contract. This Proposed adapter is unimplemented.
It is not a native NUM strict/accelerated primitive.

## Interface and construction

One `input` tensor produces same-dtype `values`. Select one complete color group
in respect/override, or explicit ordered slots in raw. Bind source_profile and
destination_profile as immutable resources; source may be resolved from attached
profile-defined metadata in semantic mode. Destination is mandatory. An analytic
source requires an explicit endpoint binding or interpretation change; a name
such as sRGB does not identify arbitrary profile bytes.

Admit ICC v2/v4 input/display/output/color-space profiles with usable directions
and Gray/RGB/CMYK/XYZ/Lab endpoint models. DeviceLink belongs to B, abstract
intermediates to C. Set intent to one of the four standard intents, default
relative_colorimetric, and requested bpc=off/on, default off. Absolute with on
is invalid. Retain the pinned v4 perceptual/saturation BPC behavior and fixed
adaptation=1.0. Record actual tag/matrix paths and admitted intent fallback.

Construct an ordinary two-profile transform with source input and destination
output directions. Profile tables, TRCs and engine PCS/media-white operations
are part of this operation; do not predecode a profile's transfer through
FMT-09 merely because it is encoded RGB. This would change the source device
coordinates. Source/destination interpretations and resources must be explicit.

execution defaults to optimized; reference is the separately fixed verification
configuration. No automatic switch on resource failure or profile content is
allowed. Identical source/destination profile bytes still execute this path.

## Samples, output and failures

Use the family's double planar formatters and explicit unit conversion/rounding.
Preserve public Float32/Float64 dtype; it does not specify internal precision.
Keep native normalized Lab lightness and CMYK ink units at public ports. Apply
the family finite/range rules, including raw's finite-only engine boundary.

Same-count conversion retains ordered source slots. Different counts replace
the source group at its smallest index with consecutive target components and
remap alpha/AOVs. Publish destination profile-defined interpretation or its
explicit compatible analytic binding in semantic modes. Raw retains only
applicable unverified descriptions. Images remain planar and straight.

Any requested target color component depends on all source colors at that point;
bypass depends only on its own source. Hidden color does not depend on alpha.
Empty/bypass-only observations still require valid static parameters/resources.
auto/materialize use requested output regions; forced view is invalid.
Resource/cancellation and failure scope inherit the family without Whole pixel
scans or fallback copies.

## Acceptance and conceptual public workflow

- Bind a fixed RGB source and CMYK destination with RGBA data. Output has CMYK
  plus unchanged alpha. Verify group position, exact alpha bits and a direct-CMM
  oracle under both execution settings. Reversing profiles need not recover RGB.
- Use fixed profiles missing a dedicated relative intent table but supporting
  the admitted fallback. Construction succeeds and records the actual path;
  an unusable reverse direction fails instead of inventing an inverse.
- Test requested bpc=off with v4 perceptual and verify the engine's effective
  rule; absolute/on fails statically. Profile header intent does not replace A's
  explicit/default intent.
- Same-profile Float64 input is still evaluated, including formatter narrowing;
  it must not bypass to an identity view. Unrequested alpha NaN does not fail
  a color request; unrequested color NaN does not fail alpha-only observation.
- Lab l=0.5 enters the formatter as L*=50; CMYK 0.5 enters as 50 percent.
  Test independent unit fixtures and the family overflow/underflow boundary.

The future public workflow explicitly imports frozen bytes, binds an input
interpretation, selects A and requests a nonzero ROI. No current executable key
or successful runtime evidence is claimed by this conceptual example.
