---
spec_schema_version: 1
id: FMT-09-18-scope-review
kind: specification_scope_review
category: 02-format-color
status: Proposed
implementation_status: implemented
clarification_status: all_active_families_complete
---

# FMT-09 through FMT-18: scope and prerequisite review

This review applies the confirmed [FMT common contract](FMT_common_contract.md)
and FMT-01..08 decisions to the remaining catalog. It records completed
active-family designs and implementation scope. Member contracts retain their
Proposed decision status independently of the recorded runtime evidence.
The subsequent maintainer decision retires FMT-16/17 and assigns their external
sampling/layout work to input/output codecs; the same-size planar boundary
below supersedes the original heterogeneous-plane alternatives.

## Changes that already follow from confirmed contracts

- Native complete images use straight colors and internal alpha. Native color
  transforms do not require a surrounding associate/unassociate pair just to
  process such an image. FMT-04 handles explicit external representation boundaries.
  Each operation must define its zero-alpha/hidden-color behavior; no blanket
  clearing or ignored-alpha optimization follows from the label.
- Interpretations are explicit static configuration; Results carry no attached
  color description. Source parameters assert consistency; FMT-08 edits a
  configuration value and never performs numerical conversion. A complete target must be coherent and carry no inherited false
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
  apply, and unavoidable required upstream complete-input work retains its original scope.

## Per-ID review

| ID | Required reconsideration / allocation | Recommended organization and next clarification |
| --- | --- | --- |
| [FMT-09 transfer](FMT-09_transfer_contract.md) | Clarification complete: A decode / B encode, ten named curves, explicit native units/reference and exact component requests. | See the family, A/B members and scalar math contract for selected domains, signed extensions, BT.2020 coefficient variants, ACES caps and the unmodified HLG endpoint behavior. Six fixed ABI 8 CPU keys are implemented; see the family implementation/runtime documentation. Proposed remains the decision status. |
| [FMT-10 RGB basis and white](FMT-10_rgb_basis_contract.md) | Clarification complete: native A/B/C, composite D; seven presets/custom xy; relative/absolute scale preserved; explicit preserve_xyz/adapt and four full linear methods. | See family/math/member specs for exact geometry, parameter domains, full triple dependencies and native exact-I exceptions. D preserves A/C/B stage rounding and failures. Nine fixed ABI 8 CPU keys and D authoring are implemented; no implicit transfer or view rendering. |
| [FMT-11 color models](FMT-11_model_conversion_contract.md) | Clarification complete: A-P model pairs, Q/R four explicit Gray meanings, S/T binary threshold/reconstruction; normalized CIE l=L*/100. | Nineteen native primitives and S composition; exact per-formula support, explicit singularities, fixed OKLab coefficients and NCL YCbCr. CMYK remains FMT-12; BT.2020 CL is a separate future member. A-R/T provide 57 fixed ABI 8 CPU keys; S composes MASK threshold, and explicit coordinate descriptions are in the ops SDK. |
| [FMT-12 ICC](FMT-12_icc_transform_contract.md) | Clarification complete: A ordinary profiles, B DeviceLink, C ordered chain; Little CMS 2.19.1 CPU. | ICC v2/v4, five endpoint models, profile-defined/explicit analytic bindings; optimized default and reference verification, double unit adaptation, finite-only raw, exact point/group support. All three fixed ABI 8 CPU keys are implemented with private allocation/work integration, actual-path inspection and independent direct-engine fixtures. Construction is synchronous and noncancellable. |
| [FMT-13 OCIO](FMT-13_ocio_transform_contract.md) | Clarification complete: A spaces, B display/view, C Looks, D NamedTransform, E files, F declarative trees; fixed OCIO v2.5.2 CPU. | Frozen config/context/resources and property snapshots; optimized default/reference, explicit bridges, finite F32 engine boundary, independent alpha, exact point/group support. All six fixed ABI 8 CPU keys are implemented with closed resource/context resolution, disabled ambient caches, frozen properties and private allocation/work admission. Construction is synchronous and noncancellable. |
| [FMT-14 gamut mapping](FMT-14_gamut_mapping_contract.md) | Design complete: A component clip, B fixed-step OKLab chroma reduction, C neutral-ray knee compression, D exact RGB-box mask. | Explicit target linear RGB cube; strict native CPU, exact dependencies, bounded search and endpoint/residual policies. All four strict fixed ABI 8 CPU keys are implemented and independently checked. Algorithms are versioned project definitions, not CSS/ACES compatibility claims. |
| [FMT-15 tone/view](FMT-15_tone_view_contract.md) | Design complete: A explicit-exposure Reinhard luminance, B relative/nits display range, C explicit view composition. | Native rational formulas and units; no image-statistics exposure, implicit gamut mapping or duplicated OCIO rendering. A/B strict fixed ABI 8 operations and native/configured C authoring are implemented. C retains each stage rounding/failure. |
| [FMT-16 retired](FMT-16_retired.md) | Confirmed: internal Y/Cb/Cr planes remain same-size, co-sited 1:1:1 (4:4:4), with same-tensor alpha. | External subsampling/reconstruction moves to input/output codecs. No heterogeneous-plane DAG carrier or FMT-16 primitive; RGB/YCbCr mathematics remains FMT-11. |
| [FMT-17 retired](FMT-17_retired.md) | Confirmed: all kernel image operators strictly obey planar storage. | External layout/packing moves to input/output codecs; allocation/addressing remains kernel-owned, logical axis/channel transformations remain NUM/FMT. No image.layout_convert primitive or per-node tile override. |
| [FMT-18 proof/check](FMT-18_softproof_contract.md) | Design complete: A ICC proof display colors, B independent sampled CMM alarm mask. | Fixed LCMS foundation; proof intents/BPC explicit, no alarm-color comparison. Both fixed ABI 8 CPU keys are implemented. B includes the source-dimension correction and budgeted synchronous source-domain table construction; pixel execution remains cancellable. |

## Cross-family obligations and extension boundaries

1. **Generic color/encoding metadata and access.** The ops SDK supplies explicit
   TensorDescription/configuration resources; FMT-06 supplies exact numerical
   interval conversion. External-engine bindings perform their own complete admission.
   NUM/CRV now use canonical TensorDescription logical tuples and normalized
   Lab/LCh lightness. The old ColorArray codec and external premultiplied ramp
   options are removed. FMT and numeric consumers retain their explicit axis
   and member arity requirements.
2. **Native transform demand support.** Fixed ABI 8 native operations declare
   per-port/per-component source tile unions during lowering. External
   adapters also declare exact component requests and complete construction scope. Do not implement a complete-input fallback and claim exact
   component requests. A conforming explicit graph composition is possible only
   if its actual constituent contracts preserve the required dependencies/errors.
3. **HDR reference and transfer versus view.** Set the common vocabulary for
   native-coordinate units/reference before selecting FMT-09/15 members. This
   does not choose an HDR standard, viewing environment or tone operator.
4. **I/O codec details outside the FMT operator catalog.** Same-size planes and
   planar internal storage are settled. External codecs still need filters,
   siting, odd edges, packed formats, regions, resources and exact output
   metadata.
5. **Complete model coverage.** The [coverage proposal](FMT_model_conversion_coverage.md)
   identifies descriptions, basis pairs and explicit routes for every named
   model, including Gray/Black-White and ACES encodings. Basis-pair coverage is
   confirmed; finalize member details and loss/reconstruction semantics. A model
   name or ramp helper is not a
   completed conversion spec.
6. **External engines.** ICC/OCIO engines and resources are pinned in their
   own contracts. Their [implementation record](../FMT-12-13-18_implementation.md)
   contains direct-engine numerical and runtime evidence; native precision
   policies or profile-header parsing alone do not establish that evidence.

## Implementation order and remaining extensions

Keep IDs stable, including retired FMT-07/16/17. All currently enumerated active
FMT-09..18 members now have completed specifications. Implement generic metadata
and exact component/region support first, then the native dependencies and pinned
external adapters. Specification decision status remains Proposed independently of runtime status.
FMT-09/10/11/14 and FMT-15A/B have native implementations; FMT-15C supports
native/configured stages. FMT-12/13/18 provide eleven external-engine CPU keys,
including the FMT-18B source-domain LCMS correction. See the
[native](../FMT-14-15_implementation.md) and
[external-engine](../FMT-12-13-18_implementation.md) implementation records.
Construction no longer requires cancellation; memory/work limits, complete
validation and pixel-execution cancellation remain in force.

Separate future extensions remain explicit: FMT-06 legal-code/effective-bit
members, BT.2020 CL, GRD-29/30 algorithms, I/O codec filters/packing, local tone
mapping and more advanced gamut/appearance algorithms. Their existence does not
leave a hidden mode or unspecified formula in the completed members. Native
geometric checks, sampled ICC alarms and view rendering remain separate products.
