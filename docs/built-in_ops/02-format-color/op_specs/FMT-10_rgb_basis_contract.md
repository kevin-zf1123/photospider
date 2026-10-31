---
spec_schema_version: 1
id: FMT-10
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
---

# FMT-10: linear RGB bases, XYZ and white adaptation

Inherits [FMT-common](FMT_common_contract.md), the NUM numerical baseline, [FMT-09 transfer separation](FMT-09_transfer_contract.md) and [model coverage](FMT_model_conversion_coverage.md). The family contains [A RGB->XYZ](FMT-10A_rgb_to_xyz.md), [B XYZ->RGB](FMT-10B_xyz_to_rgb.md) and [C adapt XYZ white](FMT-10C_adapt_xyz_white.md) operations, plus the compile-time [D RGB->RGB](FMT-10D_convert_linear_rgb.md) helper. Each native member has strict, accelerated Apple Silicon and accelerated x86-64 CPU keys. [Exact matrix definitions](FMT-10_basis_math.md) are normative.

## Purpose and stage boundaries

A/B transform coordinates between a described linear RGB basis and XYZ. C performs an explicitly selected full linear chromatic adaptation. D expands A -> optional C -> B, preserving constituent rounding and failure. No transfer, exposure, gamut mapping, tone/view rendering, dtype/range scaling, alpha association or codec operation is implied. Gray/other model conversions belong to FMT-11; arbitrary caller matrices belong to NUM-14.

Changing RGB basis and adapting white are distinct operations. D can preserve XYZ across unequal whites or map source white to target white using C. Only the latter attempts the selected chromatic-adaptation correspondence. Neither implies complete CIECAM appearance rendering or a scene/display change. All selected XYZ/RGB descriptions share the same observer convention; this family does not convert observers or spectral data.

The authoring header `photospider/ops/format/rgb_basis.hpp` exposes the static parameter codecs and transactional helpers. D resolves real upstream structure and caller-supplied color configuration, then stages A, optional C and B before publishing the expansion.

## Interface

Each native member accepts one input Result containing one Float32 or Float64 tensor, then publishes one `values` output Result containing one tensor. The output preserves Result schema identity, tensor key, logical shape and batch axes, carries no description, and sets `atomic_trailing_axes` to zero. The dtype and complete sample shape are unchanged. Full sample rank, including batch and cell axes, is at most 8; sample count is at most 2^40. Raw calls also produce no description. D accepts a graph edge and returns the final B output edge; it creates no native D registry key.

Semantic calls select one complete ordered three-component color group: RGB for A/D, XYZ for B/C. A writes X/Y/Z to the selected R/G/B slots; B writes R/G/B to X/Y/Z slots. C/D keep XYZ/RGB roles. The semantic role order defines matrix rows/columns even if physical slots are permuted. `axis` parameters and TensorDescription channel axes index cell axes and exclude the Result batch prefix. Slots are reordered separately through FMT-03. Gray is not expanded or reduced.

The selected group/axis resolves statically and uniquely. All unrelated groups, internal alpha and AOVs copy bit-for-bit. No alpha read, multiplication, hidden color removal or opacity validation is introduced. Semantic calls reject a premultiplied boundary representation; the caller unassociates through FMT-04B first, or supplies a deliberate explicit interpretation or uses raw numerical parameters.

Raw selects exactly three distinct ordered component indices on an explicit or resolved structural axis. A rank-one [3] tensor can use axis=0. RGB is not inferred from three channels and a partial raw triple is not accepted. Raw preserves shape/slots and applies the selected geometry directly to stored floating values. Integer dtype is unsupported in every mode. Non-native floating numeric encodings are explicitly decoded before semantic use, but raw may compute on their stored values.

## Geometry and units

Seven primary/white presets are admitted: sRGB/Rec.709 primaries, Display P3, Rec.2020, Adobe RGB (1998), ProPhoto RGB, ACES AP0 and AP1. Complete custom primary xy and white xy are allowed. Presets expand to the fixed RN64 coordinates in the math contract; they never choose transfer. Contradictory preset/custom descriptions fail. Resolved numerical equality is exact, including metadata zero canonicalization, not an epsilon comparison.

Primary coordinates may be virtual, negative or have y=0. White xy is finite with x>0,y>0,x+y<1. Both the homogeneous primary basis and the white-normalized RGB matrix are exactly nonsingular. Negative normalization scales are not inherently invalid. Near-singular exact invertibility is not rejected by a condition-number threshold. Geometry validity is static in all modes.

Relative RGB [1,1,1] corresponds to XYZ white with Y=1. Absolute display-linear RGB uses the FMT-09 cd/m^2 component scale, and XYZ Y is luminance in cd/m^2. A/B preserve the native numerical scale and scene/display reference. Absolute XYZ Y=100 is not a 0..100 relative encoding merely because of its value; a relative Y=100 encoding requires a separate explicit range conversion.

C normalizes both chromaticity whites to Y=1 to build its matrix and preserves the declared unit/reference-luminance scale. It does not apply a ratio of display peak/white luminances or change exposure. Not every non-neutral pixel's Y is unchanged by adaptation. Target reference/units cannot disagree with the source.

| Static logical field | Rules |
| --- | --- |
| input_description | Optional explicit source interpretation. Presence selects the described path; absence requires raw selectors and geometry. |
| group | Required unique RGB/XYZ semantic group according to member. |
| components / axis | Raw ordered triple and structural axis; not combined with semantic group. |
| source_basis | A/D resolve from explicit RGB description in semantic mode; explicit assertions must agree. Required explicit primary/white description in raw. |
| target_basis | B/D require explicit target primary/white description or one named preset. Target is linear RGB on the preserved unit/reference scale. |
| source_white | C resolves from explicit XYZ description in semantic mode; raw C requires it explicitly. Source assertions must agree. B's semantic source white belongs to its XYZ description and is not used to build the target inverse matrix. |
| target_white | C requires explicit xy/white shorthand. For B/D it is part of target_basis; contradictory duplicate specification fails. |
| method | Required for C and D adapt: xyz_scaling, bradford, cat02 or cat16; no default. Forbidden for A/B or D without adapt. |
| white_handling | B: require_match default or preserve_xyz. D: require_match default, preserve_xyz or adapt. Only adapt inserts C. Raw B has no semantic white-match obligation and accepts no separate white_handling parameter. Raw D still needs its geometric composition policy. |
| layout | auto default, view or materialize; D applies this selection to its constituent nodes. |

Presets, method and layout are closed authoring choices; custom coordinates are fixed arrays of Float64; selectors use static bounded integer indices/metadata identifiers. Fields not applicable to a member fail. All matrix data are compile-time fixed. Typed group/unit/geometry descriptions use canonical TensorDescription v4/v5; the removed ColorArray v1 payload is rejected.

## White handling and adaptation methods

B applies only the target inverse RGB matrix. Its semantic default requires the source XYZ white and target basis white to match. Explicit preserve_xyz allows a mismatch while retaining XYZ coordinates before that inverse. B never performs adaptation; to adapt, C is connected explicitly first.

D's omitted policy means require_match: unequal source/target whites fail, while equal whites expand A->B. preserve_xyz expands A->B with explicit B preserve_xyz. adapt requires a method and expands A->C->B, with matching C output white and B target white. Explicit adapt checks its selected method even when the whites are equal. White-response positivity is an unchecked numerical premise; a same-white shortcut does not change the permitted domain. Raw D freezes the same matrices/policy but does not rely on color metadata in its intermediate tensors; raw B receives no semantic match assertion.

C supports XYZ Scaling, linear Bradford, CAT02 and CAT16 at complete adaptation. There is no partial degree, scene/environment adaptation parameter, nonlinear Bradford variant or full CIECAM02/CAM16 model. All three source/target white responses under the selected matrix are strictly positive (unchecked numerical premises in every mode). No epsilon or method fallback exists. The constraint applies to white parameters, not to color sample responses: finite negative/HDR color samples remain legal, and tiny positive white responses are not rejected through a conditioning threshold.

## Numerical evaluation and composition

A/B/C strict rows use the exact coefficients derived by the math contract, three exact products plus implicit +0 bias, and one final rounding to the unchanged dtype. The derived matrix is never rounded to Float64 or the sample dtype before defining strict results. Accelerated numerical bounds inherit NUM.

For semantic nonidentity evaluation, the caller guarantees that every consumed source triple and every rounded output row is finite. Negative XYZ, RGB outside [0,1], HDR and negative color responses remain valid. Violations carry no promised error. Raw retains the NUM-14 nonfinite and signed-zero behavior, including zero matrix coefficients and implicit +0 bias.

D preserves every A/C/B rounding and each node's sample-domain preconditions. Equal source/target bases retain A->B. Mathematical equality of a fused matrix does not establish equal intermediate rounding, raw IEEE results or dependencies.

With `input_description`, A/D select a complete linear RGB group and B/C select XYZ. The supplied group is required; raw axis/components are not accepted on this path. Extra source geometry is a consistency assertion. Without a description, callers provide the ordered triple, axis and required geometry. Neither path reads labels from a Result or writes labels to an output. A computes XYZ on the source scale; B computes target-basis linear RGB; C applies the selected white adaptation. Callers supply the resulting interpretation to later operations. No observer, transfer, exposure or rendering conversion is implied.

D computes intermediate descriptions from its explicit source configuration and the chosen transforms, then serializes them into the generated A/C/B node parameters. Structural inference follows the real graph edges.

## Behavior

Each nonidentity participating component requires all three selected source coordinates at the same point, including zero-coefficient terms. Bypass alpha/AOV/other-group coordinates require only the matching input coordinate. A retained D row depending on B can require three intermediate XYZ components and therefore retain A/C rows that would disappear under a fused symbolic matrix.

Static descriptor structure, group, geometry-field representation and method checks apply to every compilation, including Empty. Empty output requests produce no payload.

Only a complete native matrix exactly equal to I selects bit-copy behavior before dot/nonfinite classification. Raw preserves every bit, including signaling NaNs and signed zeros. For semantic identity, the caller guarantees finite selected samples actually copied; bypass samples have no additional finite requirement. A single identity-looking row in a nonidentity matrix does not select this exception.

`auto` and `materialize` write independent dense output tiles for both identity and arithmetic. `view` rejects a statically nonidentity transform during preparation. For exact-I, a nonempty forced view returns `InvalidArgument/ViewUnavailable`. D applies these rules to every generated node. Equal-white C checks its selected method before selecting identity; white-response numerical premises remain unchecked.

| Condition | Phase | Outcome |
| --- | --- | --- |
| Missing/malformed/irrelevant field, ambiguous selector, malformed geometry encoding, source assertion conflict | Static preparation | InvalidArgument / InvalidDomain. |
| Wrong model, dtype, transfer, units/reference, axis/shape, default white mismatch | Static preparation | TypeMismatch. |
| Invalid numerical geometry, singular basis, nonpositive white response, semantic consumed nonfinite input or nonfinite rounded output | Unchecked caller precondition | No sample diagnostic guaranteed. |
| Raw nonfinite or sample overflow | Evaluation | NUM-defined IEEE result. |
| Forced view of a statically nonidentity matrix | Static preparation | InvalidArgument / InvalidDomain, ViewUnavailable message. |
| Nonempty exact-I forced view | Invocation | InvalidArgument / ViewUnavailable. |

## Native fast numerical profile

The separately registered `color.rgb_to_xyz_fast`, `color.xyz_to_rgb_fast` and `color.adapt_xyz_white_fast` keys use ordinary native floating arithmetic. The authoring `profile="fast"` selects those keys; the default remains `strict`. This profile definition takes precedence over NUM exact-final-rounding and accelerated error guarantees for these keys only.

Static geometry is resolved with the same exact primary/white definitions, then its matrix coefficients are converted to binary64 for computation. Float32 permits binary32 coefficients and fused multiply-add or widened binary64 evaluation before the final cast; Float64 uses binary64. Each row computes `m0*x0 + m1*x1 + m2*x2` at at least the sample dtype's precision. Reassociation and FMA are permitted; layout/ISA variants need not be bit-identical. Intermediate IEEE overflow, underflow and invalid results are ordinary native results, without exact-rational refinement.

The complete exact-identity matrix is detected before approximation and retains the specified bit copy. Unselected samples retain their bits. Static selectors and the stage boundaries of D remain unchanged: `profile="fast"` still expands A -> optional C -> B, with a materialized sample-dtype boundary after each node. No fused RGB-to-RGB registry key is introduced.

## Acceptance

Exact-rational Gaussian elimination/determinants and dyadic sample rounding, plus fixed special-value bit oracles, verify coefficients and outputs. Check all presets including virtual AP0, valid y=0 primaries, near-singular cancellation, positive white responses and all four methods. White mapping before rounding is compared with each dtype's actual rounded input cases; a nonrepresentable rational XYZ white is not treated as a floating input.

Cover complete, partial and disjoint output requests, bypass-only and Empty requests, raw NaN order, zero-coefficient peers, whole-I bit copies, negative/zero/unaligned input strides and static rejection. Verify D against independently rounded A/C/B stages, including a raw intermediate infinity consumed by a zero coefficient.

## Native coordinate spelling

`relative`/`1` and `cd/m2`/`cd/m^2` are accepted as the native relative/absolute unit spellings. Canonical `scene`/`display` references and the legacy `scene_relative`, `display_relative`, `display_absolute` and `absolute_display_cd_m2` spellings remain subject to the existing reference rules. A supplied `coordinates.scale` agrees with units. These aliases do not rescale samples or insert a display-range conversion.
