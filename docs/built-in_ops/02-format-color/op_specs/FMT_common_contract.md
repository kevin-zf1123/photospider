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
| Numeric encoding | Storage dtype, represented interval, offset/scale and quantization. Casting alone does not rescale. |
| Sampling and layout | Chroma sampling/siting and physical packing/strides are distinct from color coordinates. |
| File format | PNG/JPEG/TIFF/EXR and similar host/codec concerns; not the meaning of model conversion. |

## Functional coverage audit

The original proposal groups extraction, alpha conversion, gamut conversion,
model conversion and dtype/range conversion. The existing category already lists
FMT-01 through FMT-18. The maintainer confirmed full specification coverage,
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
| Numeric encoding | FMT-06/07 | Separate cast from interval encoding; unsigned types, bit depth and per-channel intervals. |
| Assign interpretation | FMT-08 | Explicit source description for untagged data; distinguish relabeling from transforming samples. |
| Transfer | FMT-09 | Decode/encode separately from primary transforms; extended HDR/log support needs explicit units. |
| Primary/white transform | FMT-10 | Explicit chromatic adaptation, potentially a separately addressable primitive; method belongs to its later spec. |
| Models | FMT-11 | Add explicit gray and xyY coverage; HSV/HSB naming, Lab/LCh variants and inverse paths need individual definitions. |
| Profiles and configuration | FMT-12/13 | ICC and OCIO have different resource/transform semantics; neither is a generic pair of gamut names. |
| Output rendering | FMT-14/15/18 | Gamut mapping, tone/view transforms, proofing and gamut masks are separate observable functions. |
| Chroma sampling | FMT-16 | Matrix conversion plus full/limited encoding and 4:4:4/4:2:2/4:2:0 resampling must be distinguished. |
| Layout | FMT-17 | Planar/interleaved conversion and tensor layout/metadata transformations; explicit color interpretation must survive where applicable. |

The existing category boundaries provide the following allocation for detailed
specification work: model-defined gray conversion belongs to
FMT-11; creative channel-weight monochrome remains
[GRD-12](../../07-grade/adjustments.md). Binary black/white workflows compose
gray conversion with [threshold/mask operations](../../04-mask-morphology/masks.md)
and explicit encoding. Dither/halftone requires its own algorithm/support
contract; one-bit file packing belongs to the codec boundary. Existing Layer
flatten/over owns background composition. These references establish composition
dependencies; they create no new registered aliases. The later FMT-07
clarification must explicitly allocate dither/halftone primitives or cross-category
dependencies instead of treating either as ordinary rounding.

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
- Existing [channel and color nodes](../../../kernel-architecture/Channel-and-Color-Operations.md)
  have their documented typed HWC/Whole subset. Their names do not prove generic
  ColorArray or regional support. Existing
  [numeric.cast](../../../../plugins/ops/01-numeric/numeric_cast.cpp) is registered
  from 01-numeric; FMT-06 must reconcile this actual key rather than duplicate it.
- [color.rgb_to_ycbcr420](../../../../plugins/ops/02-format-color/color_rgb_to_ycbcr420.cpp)
  already supplies a specific multi-output conversion. Its fixed semantics do
  not establish every FMT-16 sampling mode.
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
| C01 | Category scope and operator granularity | Cover FMT-01..18 as composable primitives and fill gaps; classify wrappers explicitly. | Confirmed 2026-09-22 |
| C02 | Primary representation | Generic tensors/tensor collections with composable metadata; remove special Image/Layer semantic types from the target. Migrate existing interfaces explicitly. | Confirmed 2026-09-22; supersedes initial C02 |
| C03 | Default source metadata authority | Attached description is authoritative without explicit override; untagged inputs need an explicit interpretation through assign or call-local override; ordinary source assertions must match. | Confirmed, refined by C09 |
| C04 | Conversion stage boundaries | Explicit transfer, basis/white, model, rendering and quantization stages; no implicit clipping or appearance rendering, and no new unified automatic converter in this scope. | Confirmed 2026-09-22 |
| C05 | Alpha across models | Alpha has independent semantics but may share a tensor with a color group, including L*,a*,b*,Alpha. Separate alpha tensors also remain representable; non-RGB alpha is not forced into another tensor. | Revised and confirmed 2026-09-22 |
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

This is a target shared-contract change. Existing SemanticKind, Image facets,
ColorArray codec and Layer Result schemas remain implemented until a coherent
migration changes their consumers. Existing v1 bytes cannot silently acquire
new meanings. Their migration is a prerequisite, not an implicit adapter layer
or an implementation performed by these documentation changes.

Without explicit override, the attached description is the source of truth.
An ordinary supplied source declaration asserts agreement; conflicts fail before
conversion. Explicit call-local override replaces the selected interpretation
for that call and validates the resulting effective description. Untagged data
can obtain an explicit interpretation through assign or a complete call-local
override. Neither path guesses missing required fields.

Assign publishes a new tensor interpretation with unchanged samples; its spec
must distinguish attaching a description from requesting sample validation under
that description. Call-local override changes neither the input metadata nor
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
include required complete-color or cross-component relationships in its demand;
this rule does not permit skipping an invariant required by its formula.

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

Non-RGB color and alpha have independent semantics and may occupy different
planes of the same tensor. For example, [L*,a*,b*,Alpha] describes a Lab color
group plus an independent alpha plane; alpha is not a Lab coordinate. Separate
alpha tensors and explicit split/join remain possible but are not required merely
because the model is non-RGB. This supersedes the earlier mandatory sidecar rule.

For an RGBA to Lab to RGBA workflow, restore straight RGB when required, convert
the declared color group and carry alpha unchanged through its declared path,
then explicitly associate RGB if requested. Alpha does not pass through transfer
or color-model formulas. Operations declare which groups/planes they consume and
produce, including the correspondence for alpha. Hidden colors at zero alpha and
tiny alpha arithmetic remain FMT-04/05 questions. This defines no premultiplied
Lab/CMYK coordinates or implicit Layer emission handling.

### Computation and storage encoding

Floating color coordinates retain native model units. Quantized storage is an
explicit encoding boundary. Numeric cast changes dtype without implicit interval
rescaling; encode/decode_range specifies the represented intervals separately.
UInt8 covers 0..255 and UInt16 covers 0..65535; Int8 and Int16 represent signed
integer values. The proposed native widths must be implemented before claiming
those storage types at public ports. Float16/Int32/UInt32/UInt64 are outside this
selected expansion. Existing Int64 remains supported.

An integer output cannot retain the existing Float32/64-only ColorArray facet.
FMT-06/07 must specify how its encoding description retains the information needed
to reconstruct color interpretation, including per-channel scales where applicable.
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
- A numeric red-channel read does not observe unrelated alpha merely because
  color metadata exists; an operation requiring a complete color does observe it.
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

## Deferred operator-local questions

Extraction index/name and rank removal; merge arity; swizzle constants; alpha
zero/tiny values and hidden colors; chromatic-adaptation method; exact Gray/HSV/
xyY formula and degenerate cases; ICC intent/BPC/profile-class support; OCIO
context/view contract; gamut/tone methods; dither and quantization details;
chroma filters/siting/odd dimensions; layout ownership and each operation's
exact Data/Control/Validation/dirty support belong to the later individual
clarifications. None is resolved merely by this scope audit.

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
