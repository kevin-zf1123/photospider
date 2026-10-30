---
spec_schema_version: 1
id: FMT-common
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: common_scope_complete
---

# FMT: scope and shared specification decisions

This document precedes the individual FMT operator specifications. It fixes the shared representation and interpretation policy. Formulas, ports and parameters are defined by the individual member specifications. It registers no operation.

## Inherited baseline

The [relative-coordinate scale decision](FMT_relative_coordinate_scale.md) sets native CIELAB/CIELCh lightness storage to l=L*/100 across NUM/CRV/FMT. It retains finite extensions, other coordinate scales and explicit absolute units.

The numerical semantics of [NUM-common](../../01-numeric/op_specs/NUM_common_contract.md) and [NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md) are the default: ordinary rounding, strict/accelerated precision, special values and checked shape arithmetic. The final FP32-scaled four-ULP rule also applies to Float64 accelerated arithmetic as specified there; it is not four Float64 ULP. Exact copies, discrete choices, metadata and quantized integer results retain exact requirements.

Every color, alpha, transfer, sampling and LUT interpretation is an explicit static parameter of the operation that consumes it (C13 below). Data carries no description, so nothing is propagated, cleared or matched between operations. A description is not a certificate of sample validity. Color-domain requirements, including finite samples and alpha constraints, are unchecked caller premises even where raw numeric computation separately defines IEEE non-finite values. Finite/range/order/geometry conditions on static numeric parameters are also premises. An individual FMT formula defines its strict reference and rounding boundaries. External engines use the conformance boundary under "External transforms"; no CMM/OCIO precision guarantee follows from inheriting the numerical baseline.

[FMT-COLOR](FMT-COLOR_color_array_contract.md) defines primary/white coordinates, transfer separation, relative reference, native model units, signed/HDR domains, hue units and winding preservation, RGB association, complete-color domains and immutable CMYK profile ownership. An RGB ramp's zero-alpha output policy does not define every other color conversion's policy.

## Terminology

| Term | Meaning in the specifications |
| --- | --- |
| Channel selection | Select a channel index or declared role; distinct from channel count and spatial layer selection. |
| Color model | RGB, XYZ, CIELAB, CIELCh(ab), OKLab, OKLCh, HSL, HSV, YCbCr, CMYK or an explicitly defined gray model. |
| Color-space description | Model plus applicable primaries, white, transfer, reference, coordinate units and/or device profile. |
| RGB primaries / gamut | Primary coordinates and white define the RGB basis; nominal representable gamut also depends on channel limits. |
| Transfer function | Explicit linear/encoded relationship. A gamma exponent alone does not describe every transfer. |
| Alpha association | Independent of color model and dtype; RGBA is RGB with an alpha channel. |
| Numeric encoding | Storage dtype, represented interval, offset/scale and quantization. Pure cast preserves numeric values; FMT-06 defaults to combined dtype/interval conversion with an explicit pure-cast option. |
| Sampling and layout | Chroma sampling/siting and physical packing/strides are distinct from color coordinates. |
| File format | PNG/JPEG/TIFF/EXR and similar codec concerns; not the meaning of model conversion. |

## Category structure

Each FMT ID may identify an operator family. Independently specified members use letter suffixes (FMT-01A, FMT-01B, and so on). The family contract defines shared behavior; member specifications define distinct ports, formulas and acceptance. An ID does not impose one registry key.

| Area | Home | Scope |
| --- | --- | --- |
| Channel structure | FMT-01/02/03 | Extract, merge/append, reorder/replace and constant channels. |
| Alpha | FMT-04/05 | Association, add/set/extract/remove. |
| Numeric encoding | FMT-06; FMT-07 retired | Combined dtype/interval conversion. GRD-29/30 own dither/halftone; FMT-05B owns opaque-code generation. |
| Edit interpretation | FMT-08 | Pure editing of an explicitly supplied description; the result is passed to the next consuming operation as a static parameter. No workflow node relabels data. |
| Transfer | FMT-09 | Decode/encode separately from primary transforms. |
| Primary/white transform | FMT-10 | RGB/XYZ basis conversion and explicit chromatic adaptation. |
| Models | FMT-11 | Color-model conversions including gray and xyY. |
| Profiles and configuration | FMT-12/13 | ICC and OCIO transforms; neither is a generic pair of gamut names. |
| Output rendering | FMT-14/15/18 | Gamut mapping, tone/view transforms, proofing and gamut masks are separate observable functions. |
| Chroma sampling | Input/output codec | Internal Y/Cb/Cr planes are same-size and co-sited; external subsampling/reconstruction belongs to the codec boundary. RGB/YCbCr mathematics remains FMT-11. |
| External layout / packing | Input/output codec | External packing/layout is handled at I/O or explicit layout/COPY boundaries. |

Model-defined gray conversion belongs to FMT-11; creative channel-weight monochrome remains [GRD-12](../../07-grade/adjustments.md). Binary black/white workflows compose gray conversion with [threshold/mask operations](../../04-mask-morphology/masks.md) and explicit encoding. One-bit file packing belongs to the codec boundary. Layer flatten/over owns background composition. [FMT-07](FMT-07_retired.md) assigns dither to GRD-29 and halftone to GRD-30; neither is ordinary rounding.

Forward and inverse conversions are explicit where mathematically defined; unavailable inverses and lossy paths are identified in the member specifications. PQ/HLG/log and scene/display conversions require explicit luminance/reference parameters; they are not an implicit extension of the relative linear/sRGB/gamma descriptor.

## Storage types

The element types are UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64. Float16, Int32, UInt32 and UInt64 are not supported; each member states its supported subset. An interval such as 0..65535 is not itself a native UInt16 storage type.

NUM/CRV numeric color tuples restrict a description to one canonical logical tuple: RGB, XYZ, CIELAB, CIELCh, OKLab, OKLCh, HSL, YCbCr or CMYK. The tuple uses axis zero independently of its sampled port's leading dimensions. Native coordinates are straight; individual members define alpha arity.

The general `TensorDescription` (operations SDK, canonical v4/v5 encoding) is the description passed in the `input_description` parameters of FMT-01, FMT-04/05 and FMT-09..11 and in the per-input `input_descriptions` parameter of FMT-02 (reused by FMT-03). FMT-06 takes explicit numeric ranges instead of a description. Older encodings are rejected.

## Shared decisions

| ID | Shared decision | Selected contract |
| --- | --- | --- |
| C01 | Category scope and operator granularity | Cover the functional catalog with explicit primitives/wrappers; FMT-07, FMT-16 and FMT-17 are retired with responsibilities allocated; other IDs retain their numbers. |
| C02 | Primary representation | Generic tensors/tensor collections; no special Image/Layer semantic types. Interpretation is an explicit operation parameter (C13), not data metadata. |
| C03 | Source interpretation authority | The consuming operation's explicit static description is the only authority. There is no attached description to respect or override. |
| C04 | Conversion stage boundaries | Explicit transfer, basis/white, model, rendering and quantization stages; no implicit clipping or appearance rendering and no unified automatic converter. |
| C05 | Alpha across models | Complete color images use straight coordinates; any alpha is an independent plane in the same tensor, including normalized l,a*,b*,Alpha. Separate planes/scalars may be explicit operator inputs, but no persistent external alpha association exists. |
| C06 | Numeric storage scope | UInt8/UInt16/Int8/Int16/Float32/Float64 plus Int64; storage widths are separate from floating color computation. |
| C07 | External-engine conformance | Versioned ICC/OCIO adapter contracts are defined separately from native mathematical primitives. |
| C08 | Output interpretation | Outputs carry no description. A caller that needs the destination interpretation constructs it explicitly, for example with `format::edit_description`, and passes it to the next consumer. |
| C09 | Per-call interpretation | Every interpretation is local to one invocation and enters that invocation's identity; the source and other consumers are unaffected. |
| C10 | Static structure and unchecked domains | Static schema, descriptors, parameter syntax/types and structural relations are checked. Runtime structure and all numerical/geometric premises are trusted. |
| C13 | Explicit static interpretation | Result/Tensor carry structure, data and storage owners only. Color space, white, transfer, alpha association, sampling and LUT interpretation are explicit static parameters of each consuming operation. No attached description, automatic propagation, `metadata_mode`, or connection-time label matching. Same-structure data with different intended meanings connects without rejection; domain legality is an unchecked caller premise. |

### Representation and source authority

Image channel/spatial axes and member-specific layout requirements are explicit structural configuration. An operation requiring a particular layout receives it through an explicit conversion or COPY. An interpretation parameter does not change address maps or allocate storage.

The carrier is a generic tensor or an explicitly related tensor collection. Storage dtype, shape, strides, bounds and `atomic_trailing_axes` are structural facts. Color model, channel roles, transfer, primaries/white, alpha, coverage, emission and coordinate/sampling interpretation are not carried by data. Each operation that needs them receives them as explicit static parameters: a `TensorDescription` in `input_description`, RGB geometry and white parameters, a transfer definition, model options, or an explicit NUM/CRV TensorDescription tuple parameter. A tensor collection retains explicit correspondence between related components. Immutable profile and configuration bytes are resources supplied to the consuming operation; a description string alone does not create them.

Two Results with the same structure connect to the same port even if one holds linear RGB and the other sRGB-encoded RGB; the consumer's explicit description decides how it reads the samples. Workflows written against attached descriptions or `metadata_mode` are rejected.

An operation that offers semantic selection accepts an optional `input_description`. When present, it is validated against the input descriptor, and conflicts with other explicit selectors (for example an `axis` that disagrees with the description's channel axis) are rejected before execution. When absent, the operation runs its documented raw form, which requires explicit selectors such as `axis` and `components`; a member that has no raw form (for example FMT-01B named selection) is rejected. No path guesses missing required fields. A profile, alpha role, white or transfer is never inferred from dtype, channel count or a filename.

An operation output carries no description. The pure function [`format::edit_description`](FMT-08_metadata_assignment_contract.md) patches or replaces a description without touching samples.

### Interpretation consumption, raw computation and validity

Each operation declares the interpretation parameters it consumes and its required sample domain. Plain numerical arithmetic consumes dtype, shape and numerical operands and has no color parameter. A color/coverage/compositing consumer checks the static structure of its explicit semantic fields and trusts sample premises. Sample dependencies come from the selected computation: structural completeness of a color group does not itself demand all color samples or alpha, and alpha samples enter the dependency only when the formula uses them.

The choice between semantic and raw forms is made by the parameters themselves: a member that offers both runs semantic selection when `input_description` (and any required `group`) is present and its raw form when it is absent. There is no mode parameter. Member-specific raw domains remain authoritative: FMT-09 retains NUM non-finite formula outcomes, whereas FMT-14/15 require finite consumed samples and requested arithmetic.

The raw form ignores color interpretation and uses the declared dtype/shape. A semantic formula needing primaries, axes or alpha roles cannot invent those inputs; they are specified explicitly or the invocation is incomplete.

Arithmetic output carries no sample-validity guarantee across changed values. For example, alpha 0.8 multiplied by 2 yields 1.6; passing that result to a coverage consumer whose contract requires alpha in [0,1] violates a caller premise. Interpretation parameters are static parameters of the invocation, so two consumers reading the same source with different descriptions have distinct outcomes.

### Stage composition and alpha

Each mathematical primitive has its own declared domain. An encoded Display P3 to encoded sRGB workflow explicitly decodes transfer, transforms the linear RGB basis and encodes the destination transfer. Different whites require a selected adaptation stage. Gamut mapping, tone/view rendering and quantization are selected operations with observable effects. A reference label alone never requests exposure or scene-to-display rendering.

A complete color image uses straight (unassociated) color coordinates. RGB, Gray and non-RGB models do not offer a per-image straight/premultiplied choice. Any alpha is an independent plane in the same tensor, identified by the color group in the consuming operation's `input_description` (or by an explicit raw channel selector); it need not be the last channel. Images without alpha are legal. For example, [l,a*,b*,Alpha], with l=L*/100, has a Lab group and a separate alpha plane, with no multiplication of Lab coordinates by alpha.

A standalone alpha tensor or explicit scalar can be an operator input, be extracted as data, or be processed independently. It is assembled into the result image to establish image alpha. There is no persistent alpha owner or cross-tensor sample binding. Group-to-alpha references in a description are indices into the same logical tensor. Multiple groups may share an alpha plane subject to each operation's editing rules.

A partial result retains the full logical channel structure and exact produced coverage. An unproduced alpha region is missing, never implicitly opaque or zero.

Semantic image operators publish straight image outputs. They may use premultiplied working values internally where their formulas require them, but restore the straight representation before publication. Temporary premultiplication, rounding stages and zero-alpha handling are part of each operator's numerical contract. Alpha association does not select a transfer function, working color space or blending color space.

Straight samples may retain finite hidden colors at alpha=0. Setting or removing alpha does not erase those colors or reconstruct colors already lost. A transform, composite or filter specifies its own zero-alpha output rule. Alpha is not transformed as a color coordinate or transfer-encoded implicitly. Independent emission/AOVs are not alpha-weighted without an explicit operator contract.

For RGBA -> Lab+alpha -> RGBA, the declared straight color group is converted and the alpha plane is carried through; no associate/unassociate stage is needed to enter or leave Lab. Premultiplied samples at an import/export or external-engine boundary use the explicit FMT-04 adapters, which preserve shape with alpha inside both input and output tensors. Explicit premultiplied payloads are ordinary numeric tensors; the adapter's parameters state the association and the tensor carries no label. Describing P as C in a later call is a numerical reinterpretation, not unassociation.

### Computation and storage encoding

Native floating color coordinates retain model units. Encoded storage has an explicit decoder and does not become native merely because its dtype is floating.

The [FMT-06 specification](FMT-06_numeric_conversion_contract.md) combines dtype and interval conversion by default, using full integer ranges and floating [0,1], with explicit shared/per-channel overrides. `rescale=false` performs a pure cast without changing numeric values. The operation takes explicit `source_range`/`target_range` parameters and reads no description; it does not change color-model units. UInt8 covers 0..255 and UInt16 covers 0..65535; Int8 and Int16 represent signed integer values.

NUM/CRV native tuple consumers accept Float32/64 and reject numeric encodings. An integer output that a later FMT consumer must interpret needs a `TensorDescription` with an explicit `TensorEncoding`, constructed by the caller; FMT-06 does not emit one. FMT-05B specifies opaque generation through explicit or dtype-default alpha encoding, requiring a legal exactly representable code for coverage one. Quantizing alpha, limited-range YCbCr codes, packed 10/12-bit storage and dithering need explicit individual contracts; none follows from selecting UInt16.

### External transforms

Native mathematical conversions inherit the NUM numerical baseline. ICC/OCIO adapters are contracts tied to the concrete engine and revision, immutable profile/configuration and referenced resources, settings and supported execution path. They do not inherit native strict or four-ULP guarantees by name.

The [FMT-12 ICC contract](FMT-12_icc_transform_contract.md) selects Little CMS 2.19.1 CPU with explicit optimized/reference configurations and native-unit adapters. It adds complete profile-defined RGB/Gray/CMYK/XYZ/Lab spaces plus explicit profile-to-analytic endpoint bindings. Arbitrary profile LUTs do not imply decomposable primaries/transfer; ICC PCS D50 is not equated with an analytic white preset.

[FMT-13](FMT-13_ocio_transform_contract.md) fixes OpenColorIO v2.5.2 CPU for configured spaces, display/view, Looks, NamedTransform, files and declarative trees. Config-native three-coordinate descriptions use frozen resource/context identity with explicit analytic bindings; space names do not infer units. Selected Float64 colors pass through RN32/F32/widen while alpha and AOVs bypass. Config/file resources and property snapshots are static; explicit reference-bridge and alpha-effect admission apply.

[FMT-18](FMT-18_softproof_contract.md) specifies proof colors and sampled gamut alarms on the LCMS foundation, including an identified source-domain gamut-table correction. It does not infer proofing from an OCIO display/view label.

## Dependency and support rules

Mathematical Data support of each output determines which input samples a result depends on; `Descriptor` support covers static schema and resource structure and does not imply sample reads. Member-defined static support is honored even when a pixel-dependent branch could use fewer samples. No validation reads are added: numerical/geometric premises and runtime payload structure are unchecked.

Examples: FMT-09 R <- R, FMT-11A l <- Y, and FMT-04B color <- that color plus alpha. FMT-01 bit copies establish no coverage validity. FMT-04A->B requests the published alpha edge as specified by FMT-04; no hidden reference can recover a previously consumed sample. Reads never extend into unrequested peers.

| Execution contract | Required numerical acceptance |
| --- | --- |
| Native strict | Correct rounding at each member-defined boundary, exact copy/branch/special-value rules. |
| Native accelerated, only where offered | NUM final-output FP32-scaled bound, including its Float64 rule and strict-required classifications; no automatic four-Float64-ULP claim. |
| ICC/OCIO | Pinned engine identity, resources and settings, and member-specific acceptance. OCIO Float64 selected colors retain RN32/Float32/widen even for a no-op processor. |

## Equivalence constraints

Any alternative evaluation of a member preserves its output bits or permitted error bound, output schema, sample/descriptor support and valid coverage.

| Case | Constraint |
| --- | --- |
| FMT-10D matrix chain | Every specified stage rounding and required intermediate failure is kept; a single combined matrix is not generally equivalent. |
| Transfer encode/decode pairs | Cancellation requires the actual domains, branch joins, saturation, units and rounded results; paired names are insufficient. |
| External-engine no-op | Pinned input/output adapters and point-path bit results, including dtype conversion, are preserved. |

FMT-10D with equal source/target bases still has rounded intermediate XYZ; large finite RGB can overflow a required intermediate even when an algebraically combined matrix is identity. HLG decode(1) in Float64 can fall outside a following encode domain premise; PQ's zero plateau and ACES floor/caps lose information. A different once-rounded primitive or changed FMT-14B iteration/termination algorithm requires a separate contract. Gamut containment and luminance bounds are reported separately from perceptual quality; FMT-14B does not promise maximum chroma or minimum color difference and FMT-14C is not idempotent.

## Acceptance cases

- Raw alpha arithmetic follows its formula; semantic coverage fixtures satisfy the documented alpha premise.
- A numeric red-channel read and a componentwise FMT-09 red request do not observe unrelated alpha. Unrequested alpha NaN, missing coverage or upstream failure cannot fail these observations. Semantic FMT-04 unassociation reads alpha for its requested color computation. Explicit alpha bypass requests copy its bits under the member contract.
- Two consumers can interpret the same immutable source differently through their own explicit descriptions without changing the other consumer or source.
- Data with a complete explicit interpretation works; a missing required description or selector is rejected, and an explicit selector that conflicts with the supplied description is rejected.
- Outputs carry no description; the caller-supplied description for the next consumer is validated against that consumer's input descriptor.
- Raw processing preserves the relevant numeric oracle and static structural requirements.
- Regression inputs: adjacent floats at branch thresholds, signed zero/NaN payload copies, exact Int64 endpoints, tiny alpha, large hue, near-singular bases, output tiles containing only selected components, missing alpha coverage. Member formulas decide which are accepted; the list adds no domains.

## Description representation

The `TensorDescription` canonical encoding fixes the description, typed endpoint, sampling, configured-space and resource-identity representation used by FMT `input_description` parameters.

| Item | Requirement |
| --- | --- |
| Canonical encoding | Tensor/axis/component/group/reference/encoding records with structural invariants. Each exact description value has one canonical encoding; semantic equivalence does not collapse distinct explicit records. |
| Typed parameters | Exact integer endpoints, Float64 bit patterns where significant, enum/default normalization and conditional-field rejection. Signed zero is preserved where FMT-06 endpoint/identity rules observe it; Int64 endpoints are not passed through Float64. |
| Resources | Immutable profile/configuration content and transitive resource content identities, not names or host paths, identify engine results. |
| Versions | Old ColorArray v1 payloads and older description encodings are rejected. |

## Further extensions

Members of FMT-01..06, FMT-09..15 and FMT-18 are specified in their family/member documents; FMT-08 is an editing function with no workflow member. [FMT-14](FMT-14_gamut_mapping_contract.md) fixes native mapping and geometric masks; [FMT-15](FMT-15_tone_view_contract.md) fixes native luminance/unit mapping and explicit view composition. Further FMT-06 code constraints, BT.2020 CL, GRD-29/30, external codec filters/siting/packing and additional appearance algorithms are separate extensions and create no implicit behavior in an existing member.

## References

- [W3C CSS Color 4, 2026-09-13 draft](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/) separates conversion and gamut mapping.
- [ICC.1:2022](https://www.color.org/specification/ICC.1-2022-05.pdf) defines profile-based color management.
- [OpenColorIO overview](https://opencolorio.readthedocs.io/en/latest/concepts/overview/overview.html) describes configuration-based color processing.
- Internal color planes, including Y/Cb/Cr, retain same-size 1:1:1 sampling with alpha in the same tensor. External chroma subsampling/reconstruction and physical import/export layout belong to the [boundary contract](FMT_codec_boundary.md).
- [Model conversion coverage](FMT_model_conversion_coverage.md) separates model descriptions, explicit conversion pairs and composable routes; named model entries alone do not establish complete support.
- Straight alpha is a convention, not Tensor certification: operations and callers do not declare is_straight, and explicit descriptions, association options and conversion operators select the domain computations.
