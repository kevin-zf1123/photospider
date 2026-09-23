---
spec_schema_version: 1
id: FMT-18
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
decision_authority: maintainer_delegated_2026_09_24
repository_branch: ops-specs
inspection_commit: 1b403fb9
---

# FMT-18: ICC softproof colors and independent gamut alarms

The maintainer delegated the remaining design on 2026-09-24. This family is
Proposed and unimplemented. It reuses the complete
[FMT-12 engine contract](FMT-12_icc_transform_contract.md), including immutable
ICC v2/v4 resources, Little CMS 2.19.1 CPU, endpoint bindings, native units,
double formatters, finite inputs/results, host budget/cancellation hooks and
external-engine numerical limits. It does not imply NUM strict/four-ULP accuracy.

## Member allocation

[A softproof](FMT-18A_softproof_icc.md) returns proof-rendered display coordinates.
[B alarm mask](FMT-18B_check_icc_gamut.md) returns an independent binary engine
warning about the proof device. Neither mutates working-image samples. These
are two nodes, not one coupled multi-output engine call. Asking for only a mask
never executes a full-image softproof, and requesting proof color constructs no
alarm table. A UI warning overlay uses an explicit mask/color composition.

FMT-14D tests a mathematical RGB cube. B is a sampled/quantized CMM heuristic;
its zero result is not a proof of physical device reproducibility. FMT-13 views
and FMT-15 rendering recipes do not automatically simulate an ICC printing
condition. Display calibration and ambient observation conditions are external
facts; this node produces profile coordinates, not a calibrated screen session.

## Common interface and profile admission

One `input` tensor Value, same public Float32/Float64 dtype throughout. Semantic
mode selects one complete source color group; raw gives explicit ordered slots
and source profile/units. Admit source Gray/RGB/CMYK/XYZ/Lab and ordinary classes
under FMT-12's actual device-to-PCS direction checks. Resolve omitted source
profile only from authoritative bound metadata; an analytic input needs an
explicit compatible binding or source override. Decode integers and normalize
absolute luminance explicitly before ICC processing.

`proof_profile` is mandatory: ICC v2/v4 display/output class with Gray/RGB/CMYK
device signature and usable PCS->device AND device->PCS paths. DeviceLink,
abstract, Named Color, spectral/multichannel and iccMAX proof devices are rejected.
A's mandatory `display_profile` is a Gray/RGB display-class profile with a usable
PCS->device direction. No system-monitor default. B requires no display profile;
passing one is an invalid unused parameter rather than changing its classifier.

All selectors, resources, bindings, intent/options, keepdims/output_axis and
layout are static and part of identity. Both modes support respect (default),
call-local override and finite-domain raw as in FMT-12. Static admission and full
required engine construction occur for empty/bypass-only queries. A semantic
CMYK source requires [0,1]; raw permits finite extended values. All modes reject
consumed NaN/Inf and unit-adapter/F32 formatter overflow. Profile parsing alone
does not prove the required directions, numerical validity or engine construction.

A publishes `values`, retaining dtype and using FMT-12's same-count slot retention
or changed-count group replacement at the lowest source slot. Source CMYK to RGB
therefore removes one color slot while retaining/remapping alpha/AOVs. Axis-free
Gray expansion requires output_axis explicitly. Publish display-profile-defined
coordinates or its explicit compatible analytic binding, plus proof provenance;
ICC absolute intent does not turn these values into nits. Raw retains only
applicable unverified descriptions and no automatic target-space guarantee.

B publishes `mask` of the input dtype on the non-channel shape S; keepdims=false
default, true preserves the channel axis at length one. Rank-zero removal is
unsupported and requires true. Axis-free Gray keeps its shape. Output has spatial
coordinates and binary gamut_alarm provenance (source/proof/algorithm/settings),
not Gray, coverage alpha or a complete color group. Values are exactly +0 or 1.

## A: fixed softproof chain

Static fields: rendering_intent is one of the four standard intents, default
relative_colorimetric; bpc=off/on defaults off, absolute rendering with on fails.
proofing_intent is relative_colorimetric or absolute_colorimetric, default
absolute_colorimetric. The latter requests media-white simulation; relative
adapts the proof white to the destination white under the engine recipe.

Construct the pinned cmsCreateProofingTransformTHR-equivalent chain:

| Stage | Profile | Direction | Requested intent | Requested BPC |
| --- | --- | --- | --- | --- |
| 0 | source | device->PCS | rendering_intent | bpc |
| 1 | proof | PCS->device | rendering_intent | bpc |
| 2 | proof | device->PCS | relative_colorimetric | off |
| 3 | display | PCS->device | proofing_intent | off |

All adaptation states are exactly 1.0. Enable SOFTPROOFING, disable GAMUTCHECK.
Verify actual directions and record inserted PCS conversions and table fallbacks.
The BPC array records API requests, not proof that every stage applies BPC.
Keep FMT-12's effective rules, including non-applicable device-to-PCS entries and
v4 perceptual/saturation overrides. Proof black behavior follows this exact chain
and profiles; bpc controls the rendering leg, not an independent display black
simulation switch. No fictitious Boolean promises to remove all black processing.
Both rendering and proofing intents are exposed so media-white and mapping
choices remain explicit. Physical display black/white or surround is not guessed.

execution=optimized defaults to stock optional optimization plus NOCACHE;
reference adds NOOPTIMIZE. Both add SOFTPROOFING and the chosen BPC setup.
Other flags/plugins follow FMT-12 exclusions. Profile equality does not elide the
proof round trip. The engine's medium rendering and clipping are retained;
there is no extra adapter clamp or FMT-10 adaptation. Inputs use the exact
FMT-12 double unit adapter; requested target colors must be finite. Display
coordinates can be finite negative/HDR if the engine produces them.

## B: lcms2.19.1_gamut_source_dims_v1

This member deliberately identifies a corrected source-domain gamut algorithm,
not an unmodified stock transform. Pin upstream source at
`21c582a594fe5279f90c0b93437c398f93bf62b0` (lcms2.19.1). The stock gamut builder
uses the proof channel count for its source-domain CLUT, while its input sampler
consumes source channels. This is invalid for RGB/CMYK and Gray/RGB mismatches.
The required numerical integration correction consists of:

1. Choose HIGHRESPRECALC grid density from InputColorSpace, after resolving it.
2. Allocate the gamut pipeline with nInputChannels inputs and one output.
3. Allocate the 16-bit gamut CLUT with nInputChannels inputs and one output.

Keep proof nChannels for proof-device formatters. Preserve the remaining sampler,
thresholds, internal flags, PCS round trips, rounding and interpolation. This
correction must be implemented and verified before B is registered. Same-channel
cases retain the stock grid/numerical path; changed-channel cases intentionally
have a different, explicitly identified contract. No stock fallback is allowed.
A and ordinary FMT-12 transforms do not enable gamut checking and are unaffected.

B has `source_intent` with the four standard choices, default relative_colorimetric.
It has no proofing_intent, bpc, user threshold, alarm color or execution toggle.
Use a private cmsCreateLab4ProfileTHR(context,NULL) endpoint, fixed to this build's
native D50 convention, to retain an owning transform with profiles
[source,proof,proof,Lab4], intents [source_intent,relative,relative,relative],
requested BPC all off and adaptation all 1.0. Construct with GAMUTCHECK|NOCACHE,
gamut profile=proof and nGamutPCSposition=1. Keep ordinary pinned effective intent
rules and validate every required direction. This private Lab4 endpoint is a
build-identified resource, not a dynamically serialized profile whose timestamp
may enter semantic identity.

Obtain cmsGetTransformGamutCheckPipeline. Require a non-null pipeline, exactly
source-channel-count inputs and one output; successful outer transform creation
alone is insufficient. Retain its owning transform, never independently free or
mutate the borrowed pipeline. Evaluate only this pipeline for requested mask
pixels, not the display/proof color transform.

Public source coordinates pass through FMT-12's exact double-unit adapter and
the pinned input formatter to normalized Float32 engine coordinates. Evaluate
cmsPipelineEvalFloat on that normalized vector. Require a finite score and emit
1 iff score>0, otherwise +0. The 16-bit CLUT stage first saturating-quantizes its
normalized input to u16, interpolates in the pinned integer path, and returns
q/65535 in Float32. Thus classification is q>=1 after interpolation/quantization;
it is not a floating Delta-E comparison and has no added epsilon.

The pinned classifier uses two Lab->proof-device->Lab round trips, with its
matrix-shaper threshold 1 or other-profile threshold 5 and its existing error
ratio/rounding branches. The full versioned source algorithm is normative, not
the shorthand statement 'Delta-E exceeds threshold'. Its proof round-trip helpers
use relative intent; the source-to-Lab helper retains source_intent. All three
use NOCACHE with stock optional optimization; outer
NOOPTIMIZE would not disable these internals. This is why B has one fixed
classifier configuration instead of a misleading reference/optimized toggle.

The corrected grid uses 49 knots per axis for Gray/RGB/XYZ/Lab sources and 23
for CMYK: RGB->CMYK proof has 49^3 knots, CMYK->RGB proof has 23^4. Source/profile
resource identity and source_intent determine the sampled PCS input path.
Finite extended coordinates follow the engine's saturating normalized sampling
boundary; B does not certify arbitrary negative/HDR coordinates against an
unbounded device gamut. A source outside that sampling cube may share its alarm
with a boundary source. Native geometric out-of-range checks use FMT-14D.

Alarm RGB values cannot identify this result. The pinned floating transform
writes context AlarmCodes/65535 for flagged colors, which can equal legitimate
output colors. B bypasses that collision-prone output and never relies on an
old '-1 sentinel' comment. Changing alarm display colors cannot change B.

## Request, publication and resource contract

Any A requested color or B mask sample consumes the entire source color group
at exactly the same spatial position. A alpha/AOV-only requests consume and
bit-copy only the corresponding bypass samples; B has no bypass output. Dirty
mapping follows these dependencies, with no spatial halo. Hidden colors at zero
alpha remain processed; neither member reads alpha to classify/convert colors.
Unavoidable upstream Whole operations retain their scope. Requested outputs only
are published/validated; internal construction errors still fail the node.

auto/materialize default to auto and create requested canonical planar output
pages in a new full virtual reservation; forced view fails. Private uniform-stride
planar scratch is bounded and owned. No interleaved public image or per-node tile
size. Unproduced pixels/padding do not become valid output.

Inherit ALL FMT-12 resource integration prerequisites: context bootstrap, owned
profile bytes, parser/transform/CLUT allocations, numerical scratch, callbacks,
latched failures, cache-off behavior, page windows, ancestry and final teardown.
B additionally accounts its entire static source-domain alarm grid and all three
construction helper transforms, even for one requested pixel. This is static
resource preprocessing, not a Whole image scan. Complexity is O(grid_knots) for
alarm construction plus O(P) bounded-dimensional interpolation; A construction
is profile-dependent and pixel work O(P) for fixed profiles.

Check cancellation inside table sampling and engine construction as well as
pixel batches. The stock helper's ignored sample-return/failure paths must not
publish a partially initialized table: latch host failure, verify complete table
construction and discard on any error. Invalid/nonfinite generated sampler
intermediates must fail construction before unsafe integer conversion; do not
silently treat malformed profiles as an all-zero mask. Required host safety
instrumentation preserves successful finite numerical behavior and enters the
integration identity. No detached work, unbudgeted engine cache or alternate
algorithm is permitted. Owner-retained pipeline/window access survives context
teardown until final release.

Errors and observation scope inherit FMT-12: static invalid profile/parameters
and unavailable required directions fail construction; type mismatch is explicit;
consumed domain/narrowing/requested-result failures carry sample/component detail.
ResourceExhausted and cancellation are never converted into profile fallback,
empty pipelines, identity proof or all-zero alarms. No independent transform may
reuse a failed/partially built state. All static resources are checked even for
empty observations, without reading image samples.

## Numerical acceptance and conceptual workflow

A uses the fixed FMT-12 build/configuration contract and double-adapter rounding.
B additionally pins the source-dimension correction and exact binary classification.
Each build must verify repeat/ROI/batch/thread invariance; no cross-build equality,
NUM four-ULP or universal physical-gamut accuracy is promised.

- Compare A with an independent direct-CMM four-profile harness on frozen RGB,
  Gray and CMYK source/proof fixtures and two display profiles. Exercise rendering
  intent, relative/absolute proofing and requested/effective BPC, including v4.
  Paper/black patches must respond according to the actual selected profiles;
  same-profile proofing is not assumed identity. Alpha bits remain unchanged.
- Verify Lab l=.5 enters as L*=50 and CMYK .5 as 50 percent. Test A channel-count
  changes and independent bypass-only reads with invalid unrequested colors.
- Independently rebuild B's source-grid sampler plus pinned integer interpolation;
  check 49^3 RGB->CMYK and 23^4 CMYK->RGB dimensions. Use a synthetic source whose
  K alone moves PCS across a proof boundary so K cannot be silently fixed to zero.
  Same-channel cases must match the unmodified stock table and classifier bits.
- Exercise q=0/q=1 classification and quantization collisions. Vary context alarm
  codes and include valid colors equal to alarm RGB; B results must be unchanged.
  Do not demand all-zero masks merely from identical profiles; the heuristic and
  profile reversibility determine actual values.
- Reject null/wrong-shape gamut pipelines, unsupported reverse directions and
  nonfinite generated samples. Inject cancel/budget failure during table sampling
  and ensure no partially built or zero-filled success escapes.
- Split/offset/cross-tile/component observations match whole observations;
  sparse requests never scan unrelated pixels. Verify multi-consumer/window
  lifetime, simultaneous static-table/scratch budgets and final resource release.

Conceptual public workflow branches the same working input into A for proof
colors and B for warnings; the original remains available. An explicit overlay
combines A with B if desired. Runtime acceptance still requires runnable public
commands and actual engine/profile corpus results. No CMM execution occurred in
this documentation step.

## Primary source evidence and remaining implementation work

- [Pinned proof transform and borrowed gamut accessor](https://github.com/mm2/Little-CMS/blob/21c582a594fe5279f90c0b93437c398f93bf62b0/src/cmsxform.c):
  actual four-stage arrays, softproof flags, alarm-color collision and ownership.
- [Pinned gamut builder/sampler](https://github.com/mm2/Little-CMS/blob/21c582a594fe5279f90c0b93437c398f93bf62b0/src/cmsgmt.c):
  source/proof dimension distinction, private helper flags, heuristic and table construction.
- [Pinned grid policy](https://github.com/mm2/Little-CMS/blob/21c582a594fe5279f90c0b93437c398f93bf62b0/src/cmspcs.c)
  and [CLUT evaluation](https://github.com/mm2/Little-CMS/blob/21c582a594fe5279f90c0b93437c398f93bf62b0/src/cmslut.c):
  fixed grid dimensions and normalized Float32/u16 conversion/interpolation.

These fixed source paths were inspected; they are not runtime test evidence.
The source-dimension fix, resource/cancellation hooks, metadata migration and
actual profile corpus remain implementation gates. Spectral appearance models,
ink-separation editing, physical viewing adaptation, arbitrary threshold tuning
and monitor calibration are outside these two specified members.
