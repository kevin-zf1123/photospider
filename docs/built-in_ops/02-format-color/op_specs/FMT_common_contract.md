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

It precedes individual FMT operator specifications. The maintainer selected a
unified tensor target with explicit per-operation interpretation; raw and
interpretation policies are confirmed below. Individual formulas, ports and
implementation are recorded by the individual member specifications.
Current implementation facts and those future requirements are distinguished
below. It registers no operation and changes no runtime ABI.
Decisions belong in this category's specifications; this clarification creates
neither an ADR nor a separate glossary.

The shared representation and independent NUM/CRV migration are implemented.
All enumerated active FMT members are implemented under their own contracts.
Decision status remains Proposed; see the
[NUM/CRV migration record](../../01-numeric/NUM-CRV_color_migration.md).

Implementation note: registered FMT operations take their interpretation as
explicit static parameters. A tensor description is the operations-SDK
`TensorDescription` (canonical v4/v5 encoding) passed as the `input_description`
String parameter; RGB geometry, white points, transfer curves and model options
are separate static parameters. Results carry no description. Runtime behavior
is documented in [Channel and color operations](../../../kernel-architecture/Channel-and-Color-Operations.md)
and [Tensor descriptions](../../../kernel-architecture/Tensor-Semantic-Metadata.md).

## Inherited baseline

The later [relative-coordinate scale decision](FMT_relative_coordinate_scale.md)
sets native CIELAB/CIELCh lightness storage to l=L*/100 across NUM/CRV/FMT.
It supersedes old L*=0..100 storage wording while retaining finite extensions,
other coordinate scales and explicit absolute units. NUM/CRV and FMT use the
existing TensorDescription v4/v5 codecs. The old ColorArray v1 API and codec
have been removed; old payloads are rejected.

The maintainer explicitly selected the numerical semantics of
[NUM-common](../../01-numeric/op_specs/NUM_common_contract.md) and
[NUM-acceleration](../../01-numeric/op_specs/NUM_accelerated_contract.md) as the
default. Do not reopen ordinary rounding, strict/accelerated precision, special
values, checked shape arithmetic, resource accounting, cancellation, ownership,
error attribution or demand conventions without a concrete FMT exception.
The final FP32-scaled four-ULP rule also applies to Float64 accelerated arithmetic
as specified there; it is not four Float64 ULP. Exact copies, discrete choices,
metadata and quantized integer results retain exact requirements.

The selected interpretation policy below (C13) makes every color, alpha,
transfer, sampling and LUT interpretation an explicit static parameter of the
operation that consumes it. Data carries no description, so there is nothing to
propagate, clear or match between operations. A description is not a
certificate of sample validity. Color validity follows the applicable
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
science; resource ownership follows C13 (resources are selected by the consuming
operation's preparation, not attached to data), and the consumption policy
determines when a complete-color validation obligation exists. In particular, an RGB ramp's zero-alpha output policy
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
| Edit interpretation | FMT-08 | Pure SDK editing of an explicitly supplied description; the result is passed to the next consuming operation as a static parameter. No workflow node relabels data. |
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
Gray/HSV/xyY descriptions, explicit chromatic adaptation, explicit description
parameters and integer-encoding descriptions need concrete operator-local specs.
Their final primitive IDs and port structures are deferred to that work.
PQ/HLG/log and scene/display conversions remain in the full catalog; they require
explicit luminance/reference extensions, not an implicit extension of the current
relative linear/sRGB/gamma descriptor.

## Current implementation constraints

Current public types:

- [ElementType](../../../../include/photospider/data/tensor_span.hpp) has UInt8,
  Int8, UInt16, Int16, Int64, Float32 and Float64. Float16 and other widths require
  a separately implemented public dtype extension or an explicitly defined
  encoding in an existing container. An interval such as 0..65535 is not itself
  a native UInt16 storage type.
- [Numeric color tuples](../../../../include/photospider/ops/numeric/color_description.hpp)
  restrict TensorDescription to one canonical logical tuple for NUM/CRV:
  RGB, XYZ, CIELAB, CIELCh, OKLab, OKLCh, HSL, YCbCr or CMYK. The tuple
  uses axis zero independently of its sampled port's leading dimensions.
  Native coordinates are straight; individual members define alpha arity.
  These helpers use the existing TensorDescription codec.
- [TensorDescription](../../../../include/photospider/ops/tensor_description.hpp)
  (operations SDK) is the general description used by the `input_description`
  parameters of FMT-01, FMT-04/05 and FMT-09..11 and by the per-input
  `input_descriptions` parameter of FMT-02 (which FMT-03 helpers reuse). FMT-06
  takes explicit numeric ranges instead of a description.

- ICC byte validation/ownership (`IccProfile`, operations SDK) is implemented.
  This is not an implementation of an ICC color transform or arbitrary
  profile-class admission.

## Shared decisions

That decision supersedes the initial
ColorArray-plus-Image/Layer-adapters direction. The later planar/virtual-storage
decisions are specified in the shared kernel contract linked below; operator-local
and migration details remain explicit.
Confirmation does not imply implementation completion.
[ADR 0022](../../../adr/0022-freeze-operation-c-abi-v3.md) (Proposed), decision D9,
applies C02, C05 and C11 to the operation ABI for every category: Float32 and
Float64 images, explicit logical axis roles, in-tensor alpha at any channel
index, physical planar storage and explicit migration of legacy image and
ColorArray consumers.

| ID | Shared decision | Selected contract | Status |
| --- | --- | --- | --- |
| C01 | Category scope and operator granularity | Cover the functional catalog with explicit primitives/wrappers; FMT-07, FMT-16 and FMT-17 are subsequently retired with responsibilities allocated; other IDs retain their numbers. | Confirmed 2026-09-22; FMT-07/16/17 retirement confirmed subsequently |
| C02 | Primary representation | Generic tensors/tensor collections; remove special Image/Layer semantic types. Interpretation is an explicit operation parameter (C13), not data metadata. | Confirmed 2026-09-22; metadata carrier replaced by C13 |
| C03 | Source interpretation authority | The consuming operation's explicit static description is the only authority. There is no attached description to respect or override. | C13 replaces the attached-description rule |
| C04 | Conversion stage boundaries | Explicit transfer, basis/white, model, rendering and quantization stages; no implicit clipping or appearance rendering, and no new unified automatic converter in this scope. | Confirmed 2026-09-22 |
| C05 | Alpha across models | Complete color images use straight coordinates; any alpha is an independent plane in the same tensor, including normalized l,a*,b*,Alpha. Separate planes/scalars may be explicit operator inputs, but no persistent external alpha association is allowed. | Revised and confirmed 2026-09-23 |
| C06 | Numeric storage scope | UInt8/UInt16/Int8/Int16/Float32/Float64 plus existing Int64; native missing widths are implementation dependencies, separate from floating color computation. | Confirmed 2026-09-22 |
| C07 | External-engine conformance | Define versioned ICC/OCIO adapter contracts separately from native mathematical primitives; individual precision/determinism guarantees remain to be established. | Confirmed 2026-09-22 |
| C08 | Output interpretation | Outputs carry no description. A caller that needs the destination interpretation constructs it explicitly, for example with `format::edit_description`, and passes it to the next consumer. | C13 replaces description retention |
| C09 | Per-call interpretation | Every interpretation is local to one invocation and enters that invocation's identity; the source and other consumers are unaffected. | Confirmed 2026-09-22; generalized by C13 |
| C10 | Validation trigger | Validate only the semantics an operation actually consumes from its explicit parameters, or a port's explicitly bound validator; ordinary numeric arithmetic consumes no color interpretation. | Confirmed 2026-09-22 |
| C11 | Image storage | Mandatory planar and one DAG-wide tile geometry. Interior payloads are tight full blocks; edge tiles retain valid rows and pad their width to Tw. Every next tile starts page-aligned, with gaps distinct from row padding. | Revised and confirmed 2026-09-22 |
| C12 | Virtual image backing | Reserve one full-image continuous virtual range; explicitly prepare page backing before operator access, retain produced pages until final owner retirement and fail on budget exhaustion. | Confirmed 2026-09-22 |
| C13 | Explicit static interpretation | Result/Tensor carry structure, data and storage owners only. Color space, white, transfer, alpha association, sampling and LUT interpretation are explicit static parameters of each consuming operation. No attached description, automatic propagation, `metadata_mode`, or connection-time label matching. Same-structure data with different intended meanings connects without rejection; domain legality is proved by explicit SDK calls or port-bound validators. | Approved by the maintainer as a breaking boundary change |

### Representation and source authority

The subsequent storage clarification requires every image to use planar storage.
Interleaved image imports must undergo an explicit layout conversion; planar is
a requirement, not merely an allocation preference. Generic raw tensors retain
their independent layout capabilities. This image storage requirement is enforced
by the consuming operations and the physical layout contract, without
restoring a special Image/Layer carrier type. An interpretation parameter cannot
change actual address maps or satisfy a required storage conversion.

The [kernel tensor-storage specification](../../../kernel-specs/Tensor-Storage-and-Region-Access.md)
owns address mapping, tile lookup, page provisioning, access windows and ownership.
It requires one DAG-wide tile size, row-contiguous planar samples, valid edge rows
padded to the tile width, page-aligned tile starts, and a full-image continuous virtual range with backing supplied
by page. Operators do not choose their own tile geometry. Produced pages remain
until final owner retirement; budget exhaustion fails without automatic eviction,
replay or temporary-file paging. FMT operations consume this contract rather than
independently defining physical layout. Its Chinese reader version is linked there.

The carrier is a generic tensor or explicitly related tensor collection.
Storage dtype, shape, strides, bounds, `atomic_trailing_axes` and owners are
structural facts carried by the Result schema. Color model, channel roles,
transfer, primaries/white, alpha, coverage, emission and coordinate/sampling
interpretation are not carried by data. Each operation that needs them receives
them as explicit static parameters: a `TensorDescription` in `input_description`,
RGB geometry and white parameters, a transfer definition, model options, or
an explicit NUM/CRV TensorDescription tuple parameter. Image/Layer are not special semantic carrier
types.

The shared color science includes Float32/Float64 computation and [H,W,C]
image examples. Numeric tuple consumers require a final physical color axis;
FMT describes the actual tensor axes explicitly. Neither restriction introduces
a universal special image type. Each operator identifies the
relevant axes/roles through its explicit parameters. A tensor collection must
retain explicit correspondence between related components. A description string
alone cannot create storage or resource ownership; immutable profile and
configuration bytes are `ImmutableResource` handles selected by the consuming
operation's preparation from the caller's `ResourceSet`.

The kernel has no semantic kind, no facet list, no attached description and no
connection-time label matching. Two Results with the same structure connect to
the same port even if one holds linear RGB and the other sRGB-encoded RGB; the
consumer's explicit description decides how it reads the samples. Workflows
written against attached descriptions or `metadata_mode` (WorkflowDocument
schema 5 and earlier) are rejected by the compiler.

An operation that offers semantic selection accepts an optional
`input_description`. When present, it is validated against the input descriptor
during static preparation, and conflicts with other explicit selectors (for
example an `axis` that disagrees with the description's channel axis) fail
before execution. When absent, the operation runs its documented raw form, which
requires explicit selectors such as `axis` and `components`; a member that has
no raw form (for example FMT-01B named selection) fails. No path guesses missing
required fields. Do not infer a profile, alpha role, white or transfer from dtype,
channel count or a filename.

An operation output carries no description. A caller that needs the destination
interpretation for the next consumer constructs it explicitly. The pure SDK
function [`format::edit_description`](FMT-08_metadata_assignment_contract.md)
patches or replaces a description without touching samples; there is no
workflow node that relabels data.

### Interpretation consumption, raw computation and validity

Each operation declares the interpretation parameters it consumes and its
required sample domain. Plain numerical arithmetic consumes dtype, shape and
numerical operands; it has no color parameter and adds no color-domain
validation or whole-color read closure. A color/coverage/compositing consumer
validates the explicit semantic fields and associated samples it actually
relies on. It must include sample dependencies required by its formula or
consumed semantic invariants. Structural completeness of a color group in the
supplied description does not itself demand all color samples or alpha. Alpha
samples enter demand/validation only when the formula or consumed
alpha/coverage/compositing semantics require them. Descriptor checks remain
mandatory even when their corresponding samples are not read.

The choice between semantic and raw forms is made by the parameters themselves:
a member that offers both runs semantic selection when `input_description` (and
any required `group`) is present and its raw form when it is absent. There is no
separate mode parameter. Member-specific raw domains remain authoritative: FMT-09
retains NUM nonfinite formula outcomes, whereas FMT-14/15 require finite consumed
samples and requested arithmetic.

The raw form ignores color interpretation for numerical processing. It still
checks storage bounds, dtype/shape compatibility, memory ownership and the
selected numeric formula's domain. It obeys normal resource, cancellation and
failure rules. A semantic formula needing primaries, axes or alpha roles cannot
invent those inputs; they must be specified explicitly or the invocation is
incomplete. The supported raw form of each primitive must be stated, rather than
promised for arbitrary operations.

Arithmetic output carries no sample-validity guarantee across changed values.
For example, alpha .8 multiplied by 2 can yield 1.6; a later coverage consumer
whose explicit description declares that channel as alpha, or whose port binds
an alpha-range validator, rejects it.

Validation is tied to immutable input identity, the explicit interpretation
parameters and the observed sample domain. It is not a mutable global boolean on
a shared tensor. Interpretation parameters are static parameters, so they enter
the compiled invocation identity and cache keys; two consumers reading the same
source with different descriptions have distinct identities and outcomes.

No downstream raw request cancels an already required producer's intrinsic
failure or manufactures an unpublished value. A color-transform producer that
fails its own formula still fails.

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
independent plane in the same tensor, identified explicitly by the color group
in the consuming operation's `input_description` (or by an explicit raw channel
selector); it is not required to be the last channel. Images without alpha remain legal.
For example, [l,a*,b*,Alpha], with l=L*/100, has a Lab group and a separate alpha plane, with no
multiplication of Lab coordinates by alpha.

A standalone alpha tensor or explicit scalar can be connected as an operator
input, extracted as data, or processed independently. It must be assembled into
the result image when establishing image alpha. There is no persistent alpha
owner, snapshot or cross-tensor sample binding. Group-to-alpha references in a
description are indices into that same logical tensor. Multiple groups may share
an internal alpha plane subject to each operation's explicit editing rules.
Ordinary owner retention for views and declared DAG dependencies is still
required; it does not establish a semantic external alpha relation.

A partial result retains the full logical channel structure and exact produced
coverage. An unproduced alpha region is missing, never implicitly opaque/zero
and never resolved through a hidden reference to another tensor.
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

Raw numeric operations produce no sample-validity certificates, as in C10. They
do not establish a second canonical image association state. An explicit
description supplied to one call may describe the source differently from
another call, but it may not bypass a semantic image operator's straight output
requirement. Explicit premultiplied representation payloads, if exposed at an
adapter boundary, are ordinary numeric tensors; the adapter's explicit
parameters state the association, and the tensor carries no label. Describing
P as C in a later call is not unassociation unless the caller explicitly
requests a numerical reinterpretation rather than preservation of color.

This decision supersedes earlier FMT-04/05 permissions for persistent external
alpha bindings and ordinary premultiplied image results. Numeric RGB ramps now
consume and emit straight RGB/RGBA. Their old association options and the old
ColorArray codec are removed; internal alpha weighting remains part of the
linear-light interpolation formula.

### Computation and storage encoding

Native floating color coordinates retain model units. Encoded storage has an
explicit decoder and does not become native merely because its dtype is floating.
The [FMT-06 specification](FMT-06_numeric_conversion_contract.md) now combines
dtype and interval conversion by default, using full integer ranges and floating
[0,1], with explicit shared/per-channel overrides. Explicit rescale=false performs
pure cast without changing numeric values. This supersedes the initial separate-
only cast/range allocation and the old catalog defaults. The operation takes
explicit `source_range`/`target_range` parameters and reads no description; it
does not silently change color-model units.
UInt8 covers 0..255 and UInt16 covers 0..65535; Int8 and Int16 represent signed
integer values. The proposed native widths must be implemented before claiming
those storage types at public ports. Float16/Int32/UInt32/UInt64 are outside this
selected expansion. Existing Int64 remains supported.

NUM/CRV native tuple consumers accept Float32/64 and reject numeric encodings.
An integer output needs a `TensorDescription` with an explicit `TensorEncoding`
when a later FMT consumer must interpret it. The caller constructs that description; FMT-06 does
not emit one. Later FMT-06 members must define legal code/effective-bit
conventions; FMT-07 is retired.
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
no host resource, cancellation, ownership, interpretation or failure obligations.
Selecting an engine-based contract here selects no particular engine/version,
rendering intent, precision tolerance or backend implementation.

The later [FMT-12 ICC contract](FMT-12_icc_transform_contract.md) selects Little
CMS 2.19.1 CPU with explicit optimized/reference configurations and native-unit
adapters. It adds complete profile-defined RGB/Gray/CMYK/XYZ/Lab spaces plus
explicit profile-to-analytic endpoint bindings. Arbitrary profile LUTs do not
imply decomposable primaries/transfer; ICC PCS D50 is not silently equated with
an analytic white preset. These description records are implemented in the SDK
`TensorDescription` v4/v5 codec. Removed ColorArray payloads are rejected. The pinned LCMS integration is implemented separately from the codec;
see [external-engine implementation](../FMT-12-13-18_implementation.md).

[FMT-13](FMT-13_ocio_transform_contract.md) separately fixes OpenColorIO v2.5.2
CPU for configured spaces, display/view, Looks, NamedTransform, files and
declarative trees. Config-native three-coordinate descriptions use frozen
resource/context identity with explicit analytic bindings; space names do not
infer units. Selected Float64 colors pass through RN32/F32/widen while alpha
and AOVs bypass. Config/file resources and property snapshots are static;
explicit reference-bridge and alpha-effect admission apply. Configured-space
descriptions and immutable `OcioConfigResource` snapshots are implemented in the
operations SDK; all six pinned CPU integrations are implemented.
[FMT-18](FMT-18_softproof_contract.md)
separately specifies proof colors and sampled gamut alarms on the LCMS foundation,
including an identified source-domain gamut-table correction. It does not infer
proofing from an OCIO display/view label.

## Migration dependencies and acceptance cases

The kernel has no implicit validation hooks, facet propagation or label-based
tuple closure: sample closure comes only from `atomic_trailing_axes`, and domain
validity comes from explicit descriptions and port-bound validators. Image/Layer
operations consume explicit parameters. Compiler inference, partial requests,
cache/validation identity, immutable resources and generic collection
correspondence follow C13. Reintroducing a dedicated Layer schema behind a
renamed image tensor does not satisfy the selected target.

Operation changes must satisfy at least these independent checks:

- Raw alpha arithmetic succeeds outside coverage range; a semantic coverage
  consumer then rejects the affected observation.
- A numeric red-channel read and a componentwise FMT-09 red request do not
  observe unrelated alpha. A structurally complete color group does not expand
  their sample support. Unrequested alpha NaN, missing coverage or upstream
  failure cannot fail these observations unless required upstream complete-input behavior
  already makes that failure unavoidable. Semantic FMT-04 unassociation requests and
  validates alpha for a requested color component. Explicit alpha bypass requests
  copy its bits under the member contract without adding coverage validation.
- Two consumers can interpret the same immutable source differently through
  their own explicit descriptions without changing the other consumer or source.
- Data with a complete explicit interpretation works; a missing required
  description or selector fails at preparation, and an explicit selector that
  conflicts with the supplied description fails.
- Outputs carry no description; the caller-supplied description for the next
  consumer is validated against that consumer's input descriptor.
- Distinct descriptions produce distinct invocation and cache identities;
  resources selected by preparation stay alive while the plan or a Run needs them.
- Raw processing preserves the relevant numeric oracle and cannot bypass bounds,
  resource, cancellation, or required upstream failures.

These are acceptance cases for operation changes; they are not a record of
completed test runs.

## Shared representation implementation gate

The operations-SDK [TensorDescription codec](../../../kernel-architecture/Tensor-Semantic-Metadata.md)
(canonical v4/v5) fixes the description, typed endpoint, sampling,
configured-space and resource-identity representation used by FMT
`input_description` parameters. Older encodings reject. External FMT-12/13
engine validation remains separate from serialization; descriptive OCIO
snapshots run no engine until an operation or inspection constructs one.

Before registering a member that consumes new description fields, freeze and
review the following shared artifacts:

| Artifact | Required decision and acceptance |
| --- | --- |
| Generic description codec | Canonical tensor/axis/component/group/reference/encoding records, structural invariants, unknown-field policy and equality. Each exact description value has one canonical encoding; semantic equivalence does not collapse distinct explicit records. |
| Version and migration | Existing TensorDescription v4/v5 discriminators identify the native convention. Old ColorArray v1 interfaces and decoding are retired; old payloads are rejected. CRV-09 report/table schema v4 and configuration hash domain v2 distinguish migrated bake metadata. The Markdown `spec_schema_version` is not a runtime discriminator. |
| Typed parameters | Exact integer endpoints, Float64 bit patterns where significant, enum/default normalization and conditional-field rejection. Preserve signed zero where FMT-06 endpoint/identity rules observe it; do not pass Int64 endpoints through Float64. |
| Resource binding | Immutable profile/configuration and transitive resource content identities selected by preparation from the compiler's `ResourceSet`, execution/build settings, owned lifetimes and cache-key rules. Names or host paths alone cannot identify engine results. |
| Consumer migration | Compiler inference, NUM/CRV consumers, LUT axes, import paths, fixtures and cache/validation identity must agree on the version and coordinate units. |

The active FMT and NUM/CRV consumers implement these version/resource checks;
executed validation is recorded in their implementation documents. NUM/CRV
migration changes operator configuration and contract identity while keeping
the kernel frozen. Old descriptions, bake tables and reports are retired.
See the [relative-coordinate migration](FMT_relative_coordinate_scale.md).

## Numerical and dependency plans

Compile immutable per-invocation plans from the explicit description and typed
parameters. Resolve roles, axes, encodings, exact basis/inverse matrices, branch
thresholds and coefficient enclosures once where possible. For each output,
represent three distinct support sets: `Data` (formula/copy operands),
`Validation` (sample-domain checks), and `Descriptor` (structural schema and
resource checks). Use the union of required sample sets for reads and its
transpose for dirty propagation; descriptor checks do not imply sample reads.
Honor member-defined static support even when a pixel-dependent branch could
use fewer samples. Do not replace local FMT demand with an existing NUM complete-input
callback merely because its arithmetic is reusable.

Examples include FMT-09 R <- R, FMT-11A l <- Y, and FMT-04B color <- that color
plus alpha. FMT-01 bit copies establish no coverage validity. FMT-04A->B
must request the published alpha edge as specified by FMT-04; no hidden
reference can recover a previously consumed sample. Batch/SIMD reads may not cross into
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
output schema, sample/descriptor support, valid coverage, observable failures and
publication, ownership, resource and cancellation rules. Numerical equality alone
is insufficient. Apply the following constraints before admitting an optimization:

| Candidate | Admission condition |
| --- | --- |
| Channel selection/copy traversal and simultaneous replacement maps | Preserve exact source mappings, bits, requested support and physical alias legality. |
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
2. Exercise a minimal public workflow spanning channel mapping, an SDK
   description edit passed to the next node, FMT-06, a transfer, RGB/XYZ and
   XYZ/Lab. Check bits, descriptors, sparse ROI,
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
coverage, low construction budgets and cancellation during native refinement
or bounded pixel execution. FMT-12/13/18 external-engine construction is
synchronous and noncancellable; it retains memory/work admission. Member
formulas decide which of these inputs are accepted; the list adds no new domains.

Memory measurements distinguish virtual reservation, committed backing, retained
ancestor owners, valid sample bytes and private scratch/engine state. A view can
retain a large backing; a full virtual reservation does not prove full physical
allocation. Page retention until final owner retirement and the prohibition on
automatic eviction/replay remain unchanged. Gamut containment and luminance
bounds must be reported separately from perceptual quality; FMT-14B does not
promise maximum chroma/minimum color difference and FMT-14C is not idempotent.
These are regression coverage requirements. The family implementation documents
record the executed workloads, numerical oracles and measured limits.

## Completed member scope and future extensions

Current members of FMT-01..06, FMT-09..15 and FMT-18 are specified in their
family/member documents; FMT-08 is an SDK editing function with no workflow
member. [FMT-14](FMT-14_gamut_mapping_contract.md) fixes native
mapping and geometric masks; [FMT-15](FMT-15_tone_view_contract.md) fixes native
luminance/unit mapping and explicit view composition. Active members are
implemented; their Proposed decision status remains independent of runtime
status. See [native gamut/tone](../FMT-14-15_implementation.md) and
[external engines/configured views](../FMT-12-13-18_implementation.md).

Further FMT-06 code constraints, BT.2020 CL, GRD-29/30, external codec
filters/siting/packing and additional appearance algorithms remain separate
extensions. They do not create implicit behavior in a completed member. All
explicit-interpretation, exact-demand and external-engine acceptance gates still
apply.

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
Runtime and numerical acceptance evidence belongs to the linked implementation
records; the external references alone do not establish that evidence.

The maintainer confirmed that internal color planes, including Y/Cb/Cr, retain
same-size 1:1:1 sampling, with alpha in the same tensor. External chroma
subsampling/reconstruction and physical import/export layout belong to separate
input/output codecs. Every kernel image operator obeys planar storage, including
raw calls. See the [boundary contract](FMT_codec_boundary.md).
The maintainer also confirmed the basis-pair/explicit-composition direction in
[model conversion coverage](FMT_model_conversion_coverage.md). It separates model descriptions, explicit conversion pairs and composable routes;
named model entries alone do not establish complete support.

The straight-alpha and host tiled-planar defaults are conventions, not Tensor certification. Ordinary operations and callers do not declare is_straight or layout conformance, and the kernel installs no default RGBA validator. Explicit descriptions, association options and conversion operators select real domain computations. Legal external, negative-stride and broadcast views remain supported after structural/ownership checks.
