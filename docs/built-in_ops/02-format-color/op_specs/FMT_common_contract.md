---
spec_schema_version: 1
id: FMT-common
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: common_scope_complete
inspection_commit: 548b2667
---

# FMT: scope and shared specification decisions

This file records the category-wide clarification requested on 2026-09-22.
It precedes individual FMT operator specifications. The maintainer selected a
unified tensor/metadata target after the initial category discussion; raw and
override policies are confirmed below. Individual formulas, ports and
implementation remain separate work.
Current implementation facts and those future requirements are distinguished
below. It registers no operation and changes no runtime ABI.
Decisions belong in this category's specifications; this clarification creates
neither an ADR nor a separate glossary.

## Inherited baseline

The later [relative-coordinate scale decision](FMT_relative_coordinate_scale.md)
sets native CIELAB/CIELCh lightness storage to l=L*/100 across NUM/CRV/FMT.
It supersedes old L*=0..100 storage wording while retaining finite extensions,
other coordinate scales and explicit absolute units. Its semantic migration is
pending; no existing ColorArray v1 payload is silently reinterpreted.

The maintainer explicitly selected the numerical semantics of
[NUM-common](../../01-numeric/op_specs/NUM_common_contract.md) and
[NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md) as the
default. Do not reopen ordinary rounding, strict/accelerated precision, special
values, checked shape arithmetic, resource accounting, cancellation, ownership,
error attribution or demand conventions without a concrete FMT exception.
The final FP32-scaled four-ULP rule also applies to Float64 accelerated arithmetic
as specified there; it is not four Float64 ULP. Exact copies, discrete choices,
metadata and quantized integer results retain exact requirements.

The selected metadata policy below revises the target's propagation and
validation rules independently of NUM arithmetic. It supersedes automatic
color validation merely because metadata is present, and replaces unconditional
facet clearing with explicit applicability/propagation rules. A description is
not a certificate of sample validity. Color validity follows the applicable
model contract when a consumer uses it, including finite color samples and alpha
constraints, even where raw numeric computation permits IEEE non-finite values.
An individual FMT formula
must still define its strict reference and rounding boundaries. External engines
use the separately selected conformance boundary below; no CMM/OCIO precision
guarantee follows merely from inheriting the numerical baseline.

[FMT-COLOR](FMT-COLOR_color_array_contract.md) already defines primary/white
coordinates, transfer separation, relative reference, native model units,
signed/HDR domains, hue units and winding preservation, RGB association,
complete-color validation and immutable CMYK profile ownership. Reuse the color
science and ownership rules; the new consumption policy determines when a
complete-color validation obligation exists. In particular, an RGB ramp's zero-alpha output policy
does not automatically define every future color conversion's policy.

## Terminology used for this clarification

| Term | Meaning in the specifications |
| --- | --- |
| Channel selection | Select a channel index or declared role; distinguish it from channel count and spatial layer selection. |
| Color model | RGB, XYZ, CIELAB, CIELCh(ab), OKLab, OKLCh, HSL, HSV, YCbCr, CMYK or an explicitly defined gray model. |
| Color-space description | Model plus applicable primaries, white, transfer, reference, coordinate units and/or device profile. |
| RGB primaries / gamut | Primary coordinates and white define the RGB basis; nominal representable gamut also depends on channel limits. |
| Transfer function | Explicit linear/encoded relationship. A gamma exponent alone does not describe every transfer. |
| Alpha association | Independent of color model and dtype; RGBA is RGB with an alpha channel. |
| Numeric encoding | Storage dtype, represented interval, offset/scale and quantization. Pure cast preserves numeric values; FMT-06 defaults to combined dtype/interval conversion with an explicit pure-cast option. |
| Sampling and layout | Chroma sampling/siting and physical packing/strides are distinct from color coordinates. |
| File format | PNG/JPEG/TIFF/EXR and similar host/codec concerns; not the meaning of model conversion. |

## Functional coverage audit

The original proposal groups extraction, alpha conversion, gamut conversion,
model conversion and dtype/range conversion. The category initially listed
FMT-01 through FMT-18. The maintainer confirmed full functional coverage,
including missing functions and composable primitives. Catalog coverage does
not mean individual operator acceptance or implementation completion.

Each FMT ID may identify an operator family. Independently specified members
use letter suffixes (FMT-01A, FMT-01B, and so on), following NUM-04A..V.
The family contract defines shared behavior; member specifications define
distinct ports, formulas and acceptance. An ID does not impose one registry key.

| Area | Existing home | Gap or clarification needed before individual specs |
| --- | --- | --- |
| Channel structure | FMT-01/02/03 | Extract plus merge/append, reorder/replace and constant channels; preserve meaningful roles. |
| Alpha | FMT-04/05 | Association plus add/set/extract/remove; explicit background composition and hidden-color behavior. |
| Numeric encoding | FMT-06; FMT-07 retired | A combines dtype/interval conversion; later FMT-06 members own legal codes/effective bits. GRD-29/30 own dither/halftone; FMT-05B owns opaque-code generation. |
| Assign interpretation | FMT-08 | Explicit source description for untagged data; distinguish relabeling from transforming samples. |
| Transfer | FMT-09 | Decode/encode separately from primary transforms; extended HDR/log support needs explicit units. |
| Primary/white transform | FMT-10 | Explicit chromatic adaptation, potentially a separately addressable primitive; method belongs to its later spec. |
| Models | FMT-11 | Add explicit gray and xyY coverage; HSV/HSB naming, Lab/LCh variants and inverse paths need individual definitions. |
| Profiles and configuration | FMT-12/13 | ICC and OCIO have different resource/transform semantics; neither is a generic pair of gamut names. |
| Output rendering | FMT-14/15/18 | Gamut mapping, tone/view transforms, proofing and gamut masks are separate observable functions. |
| Chroma sampling | Input/output codec | Internal Y/Cb/Cr planes are same-size and co-sited; external subsampling/reconstruction belongs to the codec boundary. RGB/YCbCr mathematics remains FMT-11. |
| External layout / packing | Input/output codec and kernel storage | All kernel images are planar; external packing/layout is handled at I/O. Logical tensor/channel transforms keep their NUM/FMT responsibilities. |

The existing category boundaries provide the following allocation for detailed
specification work: model-defined gray conversion belongs to
FMT-11; creative channel-weight monochrome remains
[GRD-12](../../07-grade/adjustments.md). Binary black/white workflows compose
gray conversion with [threshold/mask operations](../../04-mask-morphology/masks.md)
and explicit encoding. Dither/halftone requires its own algorithm/support
contract; one-bit file packing belongs to the codec boundary. Existing Layer
flatten/over owns background composition. These references establish composition
dependencies; they create no new registered aliases. The subsequent
[FMT-07 retirement](FMT-07_retired.md) assigns dither to GRD-29 and halftone to
GRD-30 in the grade catalog, with reusable NOI-01/07 sources where appropriate.
Their algorithms remain to be clarified; neither is ordinary rounding.

The required gap coverage includes explicit forward/inverse conversions where
mathematically defined; unavailable inverses and lossy paths must be identified.
Gray/HSV/xyY descriptions, explicit chromatic adaptation, unified tensor/metadata
interfaces and integer-encoding descriptions need concrete operator-local specs.
Their final primitive IDs and port structures are deferred to that work.
PQ/HLG/log and scene/display conversions remain in the full catalog; they require
explicit luminance/reference extensions, not an implicit extension of the current
relative linear/sRGB/gamma descriptor.

## Current implementation constraints

Inspected at the commit recorded above:

- [ElementType](../../../../include/photospider/data/value.hpp) has UInt8,
  Int64, Float32 and Float64. Int8/UInt16/Int16/Float16 and other widths require
  a separately implemented public dtype extension or an explicitly defined
  encoding in an existing container. An interval such as 0..65535 is not itself
  a native UInt16 storage type.
- [ColorArrayDescriptor](../../../../include/photospider/data/color_array.hpp)
  has RGB, XYZ, CIELAB, CIELCh, OKLab, OKLCh, HSL, YCbCr and CMYK. It has no Gray,
  HSV or xyY model; alpha is currently RGB-only. Transfer is linear/sRGB/gamma.
  Integer-coded colors and additional models cannot use unchanged v1 metadata
  while claiming validation under the current contract.
- Package 0.20.0 [removes the old channel/color and numeric-format operations](FMT_legacy_retirement.md),
  including numeric.cast, numeric.encode_range and color.rgb_to_ycbcr420.
  Their inspected HWC/Whole and fixed 420 behavior is historical; it does not
  implement the clarified FMT contracts. No compatibility aliases remain.
- ICC byte validation/ownership for CMYK ramps is implemented. This is not an
  implementation of an ICC color transform or arbitrary profile-class admission.

## Shared decisions

The maintainer confirmed the category decisions on 2026-09-22 and then selected
unified tensors/tensor collections plus composable metadata, removing Image/Layer
as special semantic types from the target. That decision supersedes the initial
ColorArray-plus-Image/Layer-adapters direction. The later planar/virtual-storage
decisions are specified in the shared kernel contract linked below; operator-local
and migration details remain explicit.
Confirmation does not imply implementation completion.

| ID | Shared decision | Selected contract | Status |
| --- | --- | --- | --- |
| C01 | Category scope and operator granularity | Cover the functional catalog with explicit primitives/wrappers; FMT-07, FMT-16 and FMT-17 are subsequently retired with responsibilities allocated; other IDs retain their numbers. | Confirmed 2026-09-22; FMT-07/16/17 retirement confirmed subsequently |
| C02 | Primary representation | Generic tensors/tensor collections with composable metadata; remove special Image/Layer semantic types from the target. Migrate existing interfaces explicitly. | Confirmed 2026-09-22; supersedes initial C02 |
| C03 | Default source metadata authority | Attached description is authoritative without explicit override; untagged inputs need an explicit interpretation through assign or call-local override; ordinary source assertions must match. | Confirmed, refined by C09 |
| C04 | Conversion stage boundaries | Explicit transfer, basis/white, model, rendering and quantization stages; no implicit clipping or appearance rendering, and no new unified automatic converter in this scope. | Confirmed 2026-09-22 |
| C05 | Alpha across models | Complete color images use straight coordinates; any alpha is an independent plane in the same tensor, including normalized l,a*,b*,Alpha. Separate planes/scalars may be explicit operator inputs, but no persistent external alpha association is allowed. | Revised and confirmed 2026-09-23 |
| C06 | Numeric storage scope | UInt8/UInt16/Int8/Int16/Float32/Float64 plus existing Int64; native missing widths are implementation dependencies, separate from floating color computation. | Confirmed 2026-09-22 |
| C07 | External-engine conformance | Define versioned ICC/OCIO adapter contracts separately from native mathematical primitives; individual precision/determinism guarantees remain to be established. | Confirmed 2026-09-22 |
| C08 | Raw output metadata | Retain applicable descriptions without inherited validity guarantees; remove or rebuild inapplicable descriptions. | Confirmed 2026-09-22 |
| C09 | Call-local source override | Explicitly override interpretation for one call, validating its effective description; leave the source and other consumers unchanged. | Confirmed 2026-09-22 |
| C10 | Validation trigger | Validate semantics actually consumed by an operation; ordinary numeric arithmetic does not automatically consume color metadata. | Confirmed 2026-09-22 |
| C11 | Image storage | Mandatory planar and one DAG-wide tile geometry. Interior payloads are tight full blocks; edge tiles retain valid rows and pad their width to Tw. Every next tile starts page-aligned, with gaps distinct from row padding. | Revised and confirmed 2026-09-22 |
| C12 | Virtual image backing | Reserve one full-image continuous virtual range; explicitly prepare page backing before operator access, retain produced pages until final owner retirement and fail on budget exhaustion. | Confirmed 2026-09-22 |

### Representation and source authority

The subsequent storage clarification requires every image to use planar storage.
Interleaved image imports must undergo an explicit layout conversion; planar is
a requirement, not merely an allocation preference. Generic raw tensors retain
their independent layout capabilities. This image storage requirement is enforced
by the relevant metadata consumers and physical layout contract, without
restoring a special Image/Layer carrier type. Assign/override cannot change actual
address maps or satisfy a required storage conversion by relabeling bytes.

The [kernel tensor-storage specification](../../../kernel-specs/Tensor-Storage-and-Region-Access.md)
owns address mapping, tile lookup, page provisioning, access windows and ownership.
It requires one DAG-wide tile size, row-contiguous planar samples, valid edge rows
padded to the tile width, page-aligned tile starts, and a full-image continuous virtual range with backing supplied
by page. Operators do not choose their own tile geometry. Produced pages remain
until final owner retirement; budget exhaustion fails without automatic eviction,
replay or temporary-file paging. FMT operations consume this contract rather than
independently defining physical layout. Its Chinese reader version is linked there.

The target carrier is a generic tensor or explicitly related tensor collection.
Storage dtype, shape, strides, bounds and owners remain structural facts. Color
model, channel roles, transfer, primaries/white, alpha, coverage, emission and
coordinate/sampling interpretation are composable metadata consumed as declared
by each operation. Image/Layer cease to be special semantic carrier types.
Their useful mathematical meanings remain expressible through metadata.

Existing ColorArray color science remains the starting vocabulary, including
Float32/Float64 computation and [H,W,C] image examples. Its current closed
descriptor and channel-last restrictions are implementation facts, not authority
to reintroduce a universal special image type into the target. Each operator
must identify the relevant axes/roles; their canonical schema and supported
layouts will be specified before migration. A tensor collection must retain
explicit correspondence between related components. A metadata string alone
cannot replace actual storage/resource ownership.

The generic metadata design is a target shared-contract change. Shared
SemanticKind vocabulary and the ColorArray codec remain for maintained consumers.
Legacy Image invocation paths and Layer Result schemas are already rejected by
the planar migration gates; their retained declarations do not provide a usable
compatibility path. Existing v1 bytes cannot silently acquire new meanings.
Implementing the new metadata consumers is a prerequisite, not an implicit
adapter layer or an implementation performed by these documentation changes.

Without explicit override, the attached description is the source of truth.
An ordinary supplied source declaration asserts agreement; conflicts fail before
conversion. Explicit call-local override replaces the selected interpretation
for that call and validates the resulting effective description. Untagged data
can obtain an explicit interpretation through assign or a complete call-local
override. Neither path guesses missing required fields.

[FMT-08 assign/remove](FMT-08_metadata_assignment_contract.md) publishes a new
immutable tensor interpretation with unchanged samples. Static patch/replacement
and deletion validate the final description's structure, references and owned
resources, not its pixel-domain validity. Independent component descriptions are
allowed; declared complete groups must be structurally complete. Explicit cascade
can remove dependent descriptions but not samples, and unknown annotations remain
opaque unless explicitly edited. Shared backing never means shared mutable metadata.
Call-local override changes neither the input metadata nor
another consumer's interpretation. A conversion publishes its actual destination
description, not the old source label. Do not infer a profile, alpha role, white
or transfer from dtype, channel count or a filename.

### Metadata consumption, raw computation and validity

Each operation declares the metadata fields it consumes, its required sample
domain, and propagation/removal/reconstruction rules for output descriptions.
Plain numerical arithmetic consumes dtype, shape and numerical operands by
default; merely attaching color metadata adds no color-domain validation or
whole-color read closure. A color/coverage/compositing consumer validates the
effective semantic fields and associated samples it actually relies on. It must
include sample dependencies required by its formula or consumed semantic
invariants. Structural completeness of a color group does not itself demand
all color samples or alpha. Alpha samples enter demand/validation only when the
formula or consumed alpha/coverage/compositing semantics require them. Descriptor
checks remain mandatory even when their corresponding samples are not read.

The canonical logical mode field is `metadata_mode=respect|override|raw`, with
`respect` as default wherever the member exposes this choice. Authoring helpers
serialize the resolved choice explicitly; compiler, direct invocation and cache
identity use the same field. `interpretation` is not an alternative parameter
name. This naming rule adds no mode to members that do not offer it (for example,
FMT-08 edits and the FMT-15C semantic composition helper). Member-specific raw
domains remain authoritative: FMT-09 retains NUM nonfinite formula outcomes,
whereas FMT-14/15 require finite consumed samples and requested arithmetic.

Explicit raw mode ignores semantic interpretation for numerical processing.
It still checks storage bounds, dtype/shape compatibility, memory ownership and
the selected numeric formula's domain. It obeys normal resource, cancellation
and failure rules. A semantic formula needing primaries, axes or alpha roles
cannot invent those inputs merely because metadata was ignored; they must be
specified explicitly or the invocation is incomplete. The supported raw form
of each primitive must be stated, rather than promised for arbitrary operations.

Raw arithmetic retains descriptions still applicable to the result structure,
but carries no old sample-validity guarantee across changed values. For example,
alpha .8 multiplied by 2 can yield 1.6 with an unvalidated coverage description;
a later coverage consumer rejects it. A changed shape/channel mapping must
transform, remove or explicitly rebuild incompatible descriptions. Retention
does not silently normalize samples or certify them. Metadata without a known
propagation rule must not be advertised as preserved valid semantics; exact
handling of individual fields belongs to their schema/operation specifications.

Validation is tied to immutable input identity, effective metadata and the
observed sample domain. It is not a mutable global boolean on a shared tensor.
Reusing a proof requires those facts to match; raw arithmetic and source override
cannot reuse an incompatible proof. Numerical sample caches, metadata propagation
and semantic validation identities must distinguish the effective behavior of
the call. An explicit mode/override therefore belongs to the relevant compiled
invocation identity. Other consumers retain their own interpretation and outcome.

No downstream raw request cancels an already required producer's intrinsic
failure or manufactures an unpublished value. Invalid source color labels may
be ignored by a raw consumer under the new binding/consumption policy, but a
color-transform producer that fails its own formula still fails. Retained
profile/configuration descriptions retain the owners they require; ignoring
color semantics never turns an unowned resource reference into a valid binding.

### Stage composition and alpha

Each mathematical primitive has its own declared domain. An encoded Display P3
to encoded sRGB workflow explicitly decodes transfer, transforms the linear RGB
basis and encodes the destination transfer. Different whites require a selected
adaptation stage. Gamut mapping, tone/view rendering and quantization are selected
operations with observable effects. A reference label alone never requests
exposure or scene-to-display rendering. No generic converter silently inserts
these decisions in this scope.

### Canonical straight images and internal alpha

Confirmed 2026-09-23: a complete color image uses straight (unassociated) color
coordinates. RGB, Gray and non-RGB models do not offer a per-image
straight/premultiplied choice in the ordinary computation graph. Any alpha is an
independent plane in the same tensor, identified explicitly by group metadata;
it is not required to be the last channel. Images without alpha remain legal.
For example, [l,a*,b*,Alpha], with l=L*/100, has a Lab group and a separate alpha plane, with no
multiplication of Lab coordinates by alpha.

A standalone alpha tensor or explicit scalar can be connected as an operator
input, extracted as data, or processed independently. It must be assembled into
the result image when establishing image alpha. Do not store a persistent alpha
owner, snapshot or cross-tensor sample binding in image metadata. Group-to-alpha
references are indices into that same logical tensor. Multiple groups may share
an internal alpha plane subject to each operation's explicit editing rules.
Ordinary owner retention for views and declared DAG dependencies is still
required; it does not establish a semantic external alpha relation.

A partial result retains the full logical channel structure and exact produced
coverage. An unproduced alpha region is missing, never implicitly opaque/zero
and never resolved through a hidden metadata reference to another tensor.
A consumer needing alpha must request that channel through the declared graph
or receive valid published samples. This does not force alpha production for
an observation that does not require its samples.

Semantic image operators publish straight image outputs. They may use
premultiplied working values internally where their formulas require them, but
must actually restore the public representation before publication. Temporary
premultiplication, rounding stages, zero-alpha handling and internal fusion are
part of each operator's numerical contract; a default label cannot replace
conversion. Alpha association does not select a transfer function, working
color space or blending color space.

Straight samples may retain finite hidden colors at alpha=0. Merely setting or
removing alpha does not erase those colors or reconstruct colors already lost
in another operation. A transform, composite or filter must specify its own
zero-alpha output rule. Alpha is not transformed as a color coordinate or
transfer-encoded implicitly. Independent emission/AOVs are not alpha-weighted
without an explicit operator contract.

For RGBA -> Lab+alpha -> RGBA, convert the declared straight color group and
carry its internal alpha plane through the operation. No associate/unassociate
stage is required merely to enter or leave Lab. Import/export or external-engine
adaptation of premultiplied samples is an explicit representation boundary;
FMT-04 supplies explicit shape-preserving adapters with internal alpha in both
input and output tensors; it does not add a second canonical image state.

Raw numeric operations retain applicable descriptions without sample-validity
certificates, as in C08/C10. They do not establish a second canonical image
association state. A call-local override may reinterpret source fields but may
not bypass a semantic image operator's straight output requirement. Explicit
premultiplied representation payloads, if exposed at an adapter boundary, are
ordinary numeric tensors with a boundary interpretation, not complete canonical
images. Relabeling P as C is not unassociation unless the caller explicitly
requests a numerical reinterpretation rather than preservation of color.

This decision supersedes earlier FMT-04/05 permissions for persistent external
alpha bindings and ordinary premultiplied image results. The v1 ColorArray
codec and existing numeric ramp association options remain implementation and
migration facts, not exceptions to the new category target. Their migration
must be explicit; this specification change does not implement it.

### Computation and storage encoding

Native floating color coordinates retain model units. Encoded storage has an
explicit decoder and does not become native merely because its dtype is floating.
The [FMT-06 specification](FMT-06_numeric_conversion_contract.md) now combines
dtype and interval conversion by default, using full integer ranges and floating
[0,1], with explicit shared/per-channel overrides. Explicit rescale=false performs
pure cast without changing numeric values. This supersedes the initial separate-
only cast/range allocation and the old catalog defaults. Respect rejects conflicts
between a declared source encoding and the selected source range. The operation
updates encoding correspondence rather than silently changing color-model units.
UInt8 covers 0..255 and UInt16 covers 0..65535; Int8 and Int16 represent signed
integer values. The proposed native widths must be implemented before claiming
those storage types at public ports. Float16/Int32/UInt32/UInt64 are outside this
selected expansion. Existing Int64 remains supported.

An integer output cannot retain the existing Float32/64-only ColorArray facet.
FMT-06 defines the required per-channel decoder propagation; its generic encoding
metadata and extended native dtypes remain implementation dependencies. Later
FMT-06 members must define legal code/effective-bit conventions; FMT-07 is retired.
FMT-05B now specifies opaque generation through explicit or dtype-default alpha
encoding, requiring a legal exactly representable code for coverage one.
Quantizing alpha, limited-range YCbCr codes, packed 10/12-bit storage and dithering
need explicit individual contracts; none follows from selecting UInt16.

### External transforms

Native mathematical conversions inherit the NUM numerical baseline. ICC/OCIO
adapters may define separately identified contracts tied to the concrete engine
and revision, immutable profile/configuration and referenced resources, settings
and supported execution path. Their specs must define reproducibility, numerical
acceptance and compatibility limits before implementation is accepted. They do
not inherit native strict or four-ULP guarantees by name. This exception changes
no host resource, cancellation, ownership, metadata or failure obligations.
Selecting an engine-based contract here selects no particular engine/version,
rendering intent, precision tolerance or backend implementation.

The later [FMT-12 ICC contract](FMT-12_icc_transform_contract.md) selects Little
CMS 2.19.1 CPU with explicit optimized/reference configurations and native-unit
adapters. It adds complete profile-defined RGB/Gray/CMYK/XYZ/Lab spaces plus
explicit profile-to-analytic endpoint bindings. Arbitrary profile LUTs do not
imply decomposable primaries/transfer; ICC PCS D50 is not silently equated with
an analytic white preset. These generic metadata additions are unimplemented
and do not reinterpret ColorArray v1 bytes.

[FMT-13](FMT-13_ocio_transform_contract.md) separately fixes OpenColorIO v2.5.2
CPU for configured spaces, display/view, Looks, NamedTransform, files and
declarative trees. Config-native three-coordinate descriptions use frozen
resource/context identity with explicit analytic bindings; space names do not
infer units. Selected Float64 colors pass through RN32/F32/widen while alpha
and AOVs bypass. Config/file resources and property snapshots are static;
explicit reference-bridge and alpha-effect admission apply. These descriptors
and engine integrations remain unimplemented. [FMT-18](FMT-18_softproof_contract.md)
separately specifies proof colors and sampled gamut alarms on the LCMS foundation,
including an identified source-domain gamut-table correction. It does not infer
proofing from an OCIO display/view label.

## Migration dependencies and acceptance cases

This is a shared representation/consumer change, not merely deletion of type
names. Its implementation must replace the current implicit validation hooks in
[input_validation.cpp](../../../../src/lib/data/input_validation.cpp), reconcile
NUM facet propagation and complete-color closure, and migrate Image/Layer
operations to declared tensor/metadata consumption. Compiler inference, source
bindings, partial requests, cache/validation identity, immutable resources and
generic collection correspondence must agree. Preserving a dedicated Layer
schema behind a renamed image tensor does not satisfy the selected target.
No migration code is included here.

The later implementation requires at least these independent checks:

- Raw alpha arithmetic succeeds outside coverage range; a semantic coverage
  consumer then rejects the affected observation.
- A numeric red-channel read and a componentwise FMT-09 red request do not
  observe unrelated alpha. A structurally complete color group does not expand
  their sample support. Unrequested alpha NaN, missing coverage or upstream
  failure cannot fail these observations unless required upstream Whole behavior
  already makes that failure unavoidable. Semantic FMT-04 unassociation requests and
  validates alpha for a requested color component. Explicit alpha bypass requests
  copy its bits under the member contract without adding coverage validation.
- Two consumers can interpret the same immutable source differently through
  one explicit override without changing the other consumer or source.
- Untagged data with a complete explicit interpretation works; missing required
  semantics fail, and ordinary conflicting assertions still fail without override.
- Shape/channel transformations remove or rebuild incompatible descriptions;
  retaining an applicable description does not retain a stale validity proof.
- Distinct effective metadata/modes cannot reuse incompatible semantic cache
  results; profile owners survive context teardown while retained results need them.
- Raw processing preserves the relevant numeric oracle and cannot bypass bounds,
  resource, cancellation, or required upstream failures.

These are required future acceptance cases, not tests run in this session.

## Shared representation implementation gate

Before registering a member that consumes the revised descriptions or emitting
persisted workflows using them, freeze and review the following shared artifacts:

| Artifact | Required decision and acceptance |
| --- | --- |
| Generic metadata codec | Canonical tensor/axis/component/group/reference/encoding records, structural invariants, unknown-field policy and equality. Equivalent descriptions must have one canonical encoding. |
| Version and migration | An explicit persisted schema/version discriminator for the revised coordinate convention, old-v1 import conversion or retirement, and rejection of silently mixed meanings. The Markdown `spec_schema_version` is not this runtime discriminator. |
| Typed parameters | Exact integer endpoints, Float64 bit patterns where significant, enum/default normalization and conditional-field rejection. Preserve signed zero where FMT-06 endpoint/identity rules observe it; do not pass Int64 endpoints through Float64. |
| Resource binding | Immutable profile/configuration and transitive resource content identities, execution/build settings, owned lifetimes and cache-key rules. Names or host paths alone cannot identify engine results. |
| Consumer migration | Compiler inference, bindings, NUM/CRV consumers, LUT axes, import paths, fixtures and cache/validation identity must agree on the version and coordinate units. |

The gate is still open. These documents do not select a numeric persisted version,
provide its codec or claim migration completion. Implementations may prototype
arithmetic independently; they must not attach new formulas to old v1 descriptors
and defer the discriminator until after registration. See the
[relative-coordinate migration](FMT_relative_coordinate_scale.md).

## Numerical and dependency plans

Compile immutable per-invocation plans from the effective description and typed
parameters. Resolve roles, axes, encodings, exact basis/inverse matrices, branch
thresholds and coefficient enclosures once where possible. For each output,
represent three distinct support sets: `Data` (formula/copy operands),
`Validation` (sample-domain checks), and `Descriptor` (structural metadata and
resource checks). Use the union of required sample sets for reads and its
transpose for dirty propagation; descriptor checks do not imply sample reads.
Honor member-defined static support even when a pixel-dependent branch could
use fewer samples. Do not replace local FMT demand with an existing NUM Whole
callback merely because its arithmetic is reusable.

Examples include FMT-09 R <- R, FMT-11A l <- Y, and FMT-04B color <- that color
plus alpha. FMT-01/FMT-08 bit copies establish no coverage validity. FMT-04A->B
must request the published alpha edge as specified by FMT-04; hidden metadata
cannot recover a previously consumed sample. Batch/SIMD reads may not cross into
unrequested peers, unprepared pages, row padding or page gaps.

A suitable native implementation computes a machine-precision candidate, proves
branch/domain decisions and a conservative enclosure of the complete reference,
then certifies the final result or falls back to exact/refined evaluation.
Refinement, scratch and exact state remain budgeted and cancellable. Stable
algebraic forms can improve certification only within the selected reference
branch; raw expression and special-value rules still apply.

| Execution contract | Required numerical acceptance |
| --- | --- |
| Native strict | Correct rounding at each member-defined boundary, exact copy/branch/special-value rules. |
| Native accelerated, only where offered | NUM final-output FP32-scaled bound, including its Float64 rule and strict-required classifications; no automatic four-Float64-ULP claim. |
| ICC/OCIO | Pinned engine/integration/build/resource/settings identity and member-specific acceptance. OCIO Float64 selected colors retain RN32/Float32/widen even for a no-op processor. |

## Optimization admission

An optimization must preserve the member's output bits or permitted error bound,
metadata, sample/descriptor support, valid coverage, observable failures and
publication, ownership, resource and cancellation rules. Numerical equality alone
is insufficient. Apply the following constraints before admitting an optimization:

| Candidate | Admission condition |
| --- | --- |
| Channel selection/copy traversal and simultaneous replacement maps | Preserve exact source mappings, bits, requested support and physical alias legality. |
| FMT-08 copy elimination | Retain the new immutable descriptor, resource owners and validation/cache identity. |
| Static coefficient preparation and vectorized candidates | Preserve exact coefficient meaning, branch decisions and per-output certification. |
| FMT-10D matrix-chain fusion | Keep every specified stage rounding and required intermediate failure; a single combined matrix is not generally equivalent. |
| Transfer encode/decode cancellation | Prove the actual domains, branch joins, saturation, units and rounded results. Paired names are insufficient. |
| External-engine no-op elimination or batching | Preserve the pinned input/output adapters and required point-path bit results, including dtype conversion. |

FMT-10D with equal source/target bases still has rounded intermediate XYZ; large
finite RGB can overflow a required intermediate even when an algebraically
combined matrix is identity. HLG decode(1) in Float64 can fail a following encode
domain check; PQ's zero plateau and ACES floor/caps lose information. These
behaviors cannot be removed by fusion. A different once-rounded primitive or
changed FMT-14B iteration/termination algorithm requires a separate contract.

## Implementation sequence and focused acceptance

1. Close the shared representation gate and validate canonicalization, version
   rejection, typed endpoints and owned resource identity independently.
2. Exercise a minimal public workflow spanning channel mapping, metadata assign,
   FMT-06, a transfer, RGB/XYZ and XYZ/Lab. Check bits, descriptors, sparse ROI,
   dirty propagation and failure scope against independent expectations.
3. Prototype difficult native paths early: FMT-14B's fixed 32/64 iterations,
   ill-conditioned bases, near-neutral cancellation and strict/accelerated
   boundaries. Report input distribution, certification/fallback frequency,
   timing and cancellation latency; do not infer speed from operation counts.
4. Validate pinned ICC/OCIO integrations with independent direct-engine harnesses
   and analytic/synthetic fixtures. Cover Lab and CMYK unit adapters, FMT-18's
   source-domain dimension correction (including K-only variation), construction
   failure latching, resource isolation and OCIO batch/point agreement over tails,
   alignment, ROI and thread partitions. Wrapper self-comparison is insufficient.
5. Admit broader optimization only after the preceding behavior is established.

Focused regression inputs include adjacent floats at branch thresholds, signed
zero/NaN payload copies, exact Int64 endpoints, tiny alpha, large hue, near-singular
bases, one-component requests with invalid unrequested peers, missing alpha
coverage, low budgets and cancellation during construction/refinement. Member
formulas decide which of these inputs are accepted; the list adds no new domains.

Memory measurements distinguish virtual reservation, committed backing, retained
ancestor owners, valid sample bytes and private scratch/engine state. A view can
retain a large backing; a full virtual reservation does not prove full physical
allocation. Page retention until final owner retirement and the prohibition on
automatic eviction/replay remain unchanged. Gamut containment and luminance
bounds must be reported separately from perceptual quality; FMT-14B does not
promise maximum chroma/minimum color difference and FMT-14C is not idempotent.
These are future implementation gates, not evidence of tests or engines run by
this documentation revision.

## Completed member scope and future extensions

Current members of FMT-01..06, FMT-08..15 and FMT-18 are now specified in their
family/member documents. [FMT-14](FMT-14_gamut_mapping_contract.md) fixes native
mapping and geometric masks; [FMT-15](FMT-15_tone_view_contract.md) fixes native
luminance/unit mapping and explicit view composition. FMT-14/15/18 decisions
were delegated by the maintainer on 2026-09-24. Their Proposed status and
not_implemented state remain distinct from completed design.

Further FMT-06 code constraints, BT.2020 CL, GRD-29/30, external codec
filters/siting/packing and additional appearance algorithms remain separate
extensions. They do not create implicit behavior in a completed member. All
runtime metadata, exact-demand and external-engine acceptance gates still apply.

## Sources and verification boundary

Repository sources linked above establish current implementation facts. Public
references support terminology and the separation of conversion responsibilities:
[W3C CSS Color 4, 2026-09-13 draft](https://www.w3.org/TR/2026/CRD-css-color-4-20260913/)
separates conversion and gamut mapping;
[ICC.1:2022](https://www.color.org/specification/ICC.1-2022-05.pdf)
defines profile-based color management;
[OpenColorIO overview](https://opencolorio.readthedocs.io/en/latest/concepts/overview/overview.html)
describes configuration-based color processing (the page identifies its content
as originating from v1; it is used only for this conceptual distinction).
These are reference material,
not automatic adoption of CSS rendering behavior or a fixed OCIO implementation.
No operator execution or numerical acceptance test is performed by this
documentation-only clarification.

## Same-size color planes and codec boundary (2026-09-23)

The maintainer confirmed that internal color planes, including Y/Cb/Cr, retain
same-size 1:1:1 sampling, with alpha in the same tensor. External chroma
subsampling/reconstruction and physical import/export layout belong to separate
input/output codecs. Every kernel image operator obeys planar storage, including
raw/override calls. See the [boundary contract](FMT_codec_boundary.md).
The maintainer also confirmed the basis-pair/explicit-composition direction in
[model conversion coverage](FMT_model_conversion_coverage.md). It separates model descriptions, explicit conversion pairs and composable routes;
named model entries alone do not establish complete support.
