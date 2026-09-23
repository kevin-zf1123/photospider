---
spec_schema_version: 1
id: FMT-09-18-scope-review
kind: specification_scope_review
category: 02-format-color
status: Proposed
implementation_status: not_implemented
clarification_status: all_active_families_complete
inspection_commit: 1b403fb9
---

# FMT-09 through FMT-18: scope and prerequisite review

This review applies the confirmed [FMT common contract](FMT_common_contract.md)
and FMT-01..08 decisions to the remaining catalog. It records the completed active-family designs and the remaining implementation
prerequisites. FMT-14/15/18 decisions were delegated by the maintainer on
2026-09-24. The member contracts remain Proposed, not runtime acceptance.
The subsequent maintainer decision retires FMT-16/17 and assigns their external
sampling/layout work to input/output codecs; the same-size planar boundary
below supersedes the original heterogeneous-plane alternatives.
Existing implementation retirement is recorded in
[the retirement record](FMT_legacy_retirement.md).

## Changes that already follow from confirmed contracts

- Native complete images use straight colors and internal alpha. Native color
  transforms do not require a surrounding associate/unassociate pair just to
  process such an image. FMT-04 handles explicit external representation boundaries.
  Each operation must define its zero-alpha/hidden-color behavior; no blanket
  clearing or ignored-alpha optimization follows from the label.
- Attached descriptions are authoritative unless explicitly reinterpreted. Source
  parameters assert consistency; FMT-08 assigns interpretation, never performs
  conversion. A complete target must be coherent and carry no inherited false
  sample-validity proof. Alpha/AOV/emission are transformed only when explicitly
  part of the declared operation, not because they occupy the same tensor.
- FMT-06A owns dtype and default/explicit per-channel interval conversion. Later
  FMT-06 members own effective-bit/legal-code validation. FMT-07 is retired;
  GRD-29/30 own future dither/halftone. Do not put these back into transfer/model
  conversion as undocumented side effects. Decode code-domain samples explicitly
  before an operation requiring native coordinates or normalized alpha.
- Images are planar with fixed DAG tile geometry and the kernel's virtual-span,
  page-backing, edge-row and lifetime rules. All image channels share spatial
  dimensions and a sample grid, including full-resolution Y/Cb/Cr and alpha.
  [FMT-16/17 are retired](FMT_codec_boundary.md); I/O codecs handle external
  sampling/packing. No operator publishes an interleaved image or a private
  tile size.
- Native mathematics inherits NUM, with each formula stating exact rounding
  boundaries. A blanket Float64-intermediate implementation is not the strict
  reference. ICC/OCIO use separately fixed engine/resource/numerical contracts;
  their names do not imply native strict or four-ULP conformance.
- Exact request scope is formula-dependent. A separable transfer can read one
  selected color component; a requested matrix/model output may require several
  source components. Pass-through alpha/AOV-only requests must not automatically
  invoke a color engine or read all color samples. Static descriptor checks still
  apply, and unavoidable required upstream Whole work retains its original scope.

## Per-ID review

| ID | Required reconsideration / allocation | Recommended organization and next clarification |
| --- | --- | --- |
| [FMT-09 transfer](FMT-09_transfer_contract.md) | Clarification complete: A decode / B encode, ten named curves, explicit native units/reference and exact component requests. | See the family, A/B members and scalar math contract for selected domains, signed extensions, BT.2020 coefficient variants, ACES caps and the unmodified HLG endpoint behavior. Proposed/unimplemented; runtime verification remains required. |
| [FMT-10 RGB basis and white](FMT-10_rgb_basis_contract.md) | Clarification complete: native A/B/C, composite D; seven presets/custom xy; relative/absolute scale preserved; explicit preserve_xyz/adapt and four full linear methods. | See family/math/member specs for exact geometry, parameter domains, full triple dependencies and native exact-I exceptions. D preserves A/C/B stage rounding and failures. Proposed/unimplemented; no implicit transfer or view rendering. |
| [FMT-11 color models](FMT-11_model_conversion_contract.md) | Clarification complete: A-P model pairs, Q/R four explicit Gray meanings, S/T binary threshold/reconstruction; normalized CIE l=L*/100. | Nineteen native primitives and S composition; exact per-formula support, explicit singularities, fixed OKLab coefficients and NCL YCbCr. CMYK remains FMT-12; BT.2020 CL is a separate future member. Proposed/unimplemented, including shared coordinate/schema migration. |
| [FMT-12 ICC](FMT-12_icc_transform_contract.md) | Clarification complete: A ordinary profiles, B DeviceLink, C ordered chain; Little CMS 2.19.1 CPU. | ICC v2/v4, five endpoint models, profile-defined/explicit analytic bindings; optimized default and reference verification, double unit adaptation, finite-only raw, exact point/group support. Proposed/unimplemented; resource/cancellation integration and engine corpus remain implementation gates. |
| [FMT-13 OCIO](FMT-13_ocio_transform_contract.md) | Clarification complete: A spaces, B display/view, C Looks, D NamedTransform, E files, F declarative trees; fixed OCIO v2.5.2 CPU. | Frozen config/context/resources and property snapshots; optimized default/reference, explicit bridges, finite F32 engine boundary, independent alpha, exact point/group support. Proposed/unimplemented; host resource/cache/environment/cancellation integration remains prerequisite. |
| [FMT-14 gamut mapping](FMT-14_gamut_mapping_contract.md) | Design complete: A component clip, B fixed-step OKLab chroma reduction, C neutral-ray knee compression, D exact RGB-box mask. | Explicit target linear RGB cube; strict native CPU, exact dependencies, bounded search and endpoint/residual policies. Algorithms are versioned project definitions, not CSS/ACES compatibility claims. |
| [FMT-15 tone/view](FMT-15_tone_view_contract.md) | Design complete: A explicit-exposure Reinhard luminance, B relative/nits display range, C explicit view composition. | Native rational formulas and units; no image-statistics exposure, implicit gamut mapping or duplicated OCIO rendering. C retains each stage rounding/failure. |
| [FMT-16 retired](FMT-16_retired.md) | Confirmed: internal Y/Cb/Cr planes remain same-size, co-sited 1:1:1 (4:4:4), with same-tensor alpha. | External subsampling/reconstruction moves to input/output codecs. No heterogeneous-plane DAG carrier or FMT-16 primitive; RGB/YCbCr mathematics remains FMT-11. |
| [FMT-17 retired](FMT-17_retired.md) | Confirmed: all kernel image operators strictly obey planar storage. | External layout/packing moves to input/output codecs; allocation/addressing remains kernel-owned, logical axis/channel transformations remain NUM/FMT. No image.layout_convert primitive or per-node tile override. |
| [FMT-18 proof/check](FMT-18_softproof_contract.md) | Design complete: A ICC proof display colors, B independent sampled CMM alarm mask. | Fixed LCMS foundation; proof intents/BPC explicit, no alarm-color comparison. B requires the identified source-dimension correction and budgeted/cancellable source-domain table construction before implementation acceptance. |

## Prerequisites that should be resolved before detailed formulas

1. **Generic color/encoding metadata and access.** FMT-08 requires immutable
   composable descriptions/resources; FMT-06 requires exact code-domain decoders.
   Existing ColorArray v1 is a shared NUM/CRV dependency, not an implementation of
   that target. Reconcile its model/units vocabulary explicitly rather than
   silently promoting the old last-axis/premultiplied codec to the new contract.
2. **Native transform demand support.** Existing planar callback support is a
   bounded subset; implementation must verify per-port/per-component requests
   for RGB-coupled transforms. Do not implement a Whole fallback and claim exact
   component requests. A conforming explicit graph composition is possible only
   if its actual constituent contracts preserve the required dependencies/errors.
3. **HDR reference and transfer versus view.** Set the common vocabulary for
   native-coordinate units/reference before selecting FMT-09/15 members. This
   does not choose an HDR standard, viewing environment or tone operator.
4. **I/O codec details outside the FMT operator catalog.** Same-size planes and
   planar internal storage are settled. External codecs still need filters,
   siting, odd edges, packed formats, regions, resources and exact output
   metadata. They no longer block the internal model-conversion carrier.
5. **Complete model coverage.** The [coverage proposal](FMT_model_conversion_coverage.md)
   identifies descriptions, basis pairs and explicit routes for every named
   model, including Gray/Black-White and ACES encodings. Basis-pair coverage is
   confirmed; finalize member details and loss/reconstruction semantics. A model
   name or ramp helper is not a
   completed conversion spec.
6. **External engines.** Select and pin ICC/OCIO engines/resources only in their
   own clarification. Native precision policies, current profile parsers and
   historical source code are insufficient engine conformance evidence.

## Implementation order and remaining extensions

Keep IDs stable, including retired FMT-07/16/17. All currently enumerated active
FMT-09..18 members now have completed specifications. Implement generic metadata
and exact component/region support first, then the native dependencies and pinned
external adapters. Every member remains Proposed/not_implemented until its own
runtime gate passes. FMT-18B specifically requires its identified source-domain
LCMS correction; stock cross-channel-count alarm construction is not conforming.

Separate future extensions remain explicit: FMT-06 legal-code/effective-bit
members, BT.2020 CL, GRD-29/30 algorithms, I/O codec filters/packing, local tone
mapping and more advanced gamut/appearance algorithms. Their existence does not
leave a hidden mode or unspecified formula in the completed members. Native
geometric checks, sampled ICC alarms and view rendering remain separate products.
