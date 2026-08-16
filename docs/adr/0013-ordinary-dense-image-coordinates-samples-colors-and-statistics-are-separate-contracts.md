# ADR 0013: Ordinary DenseImage Coordinates, Samples, Colors, and Statistics Are Separate Contracts

## Status

Accepted for GitHub Issue #129 / DI-1 on 2026-08-17. This decision governs the
ordinary built-in DenseImage metadata baseline. It does not retire operation
ABI v2 or `ImageBuffer`, migrate Host/IPC/worker/durable/CLI formats, implement
automatic color conversion, or reinterpret provider-defined OpenEXR Deep
metadata as ordinary-image authority.

English architecture and OpenSpec documents are authoritative. Live remote
delivery, CI, and review state remain in Project 6, issue #129, the active
OpenSpec change, and `development_tracking.md`.

## Context

ADR 0008 established `DenseTensor + ImageFacet` as the ordinary-image model,
but the installed ImageFacet previously carried only x/y/optional-channel axes.
That left logical origin, display extent, stable channel identity, declared
sample meaning, color interpretation, and observed statistics open to implicit
zero-origin, channel-name, or `[0,1]` assumptions.

These facts do not share one lifetime or identity:

- payload coordinates are immutable descriptor facts;
- display extent is immutable presentation metadata;
- `RegionSet` is dynamic work or validity;
- storage capability, quantization, declared sample interval, and color
  interpretation can change independently;
- observed min/max and histograms depend on payload revision, Region,
  selection, algorithm, and version; and
- diagnostic names can change without changing semantics.

Later output-plan, operation-plugin, wire, codec, and artifact migrations need
one bounded record baseline. Appending optional fields to structural version 1
would make old and new payloads ambiguous and invite forbidden missing-tail
defaults.

## Decision

### Coordinates and readiness

`ImageBounds` is four signed 64-bit endpoints representing a nonempty half-open
window. Every built-in ordinary ImageFacet requires `data_window`; an optional
`display_window` is independent. Publication validates ordered spans without
signed overflow, exact span equality with the descriptor's explicit x/y axes,
and distinct in-rank x/y/optional-channel axes.

The data window is the logical pixel-coordinate authority. Display window does
not grant payload access. Dynamic dirty, dependency, execution, and HP-validity
selection remains `RegionSet`. A complete ImageRect is the exact data window,
and operation code subtracts its signed origin only after containment checks.

`Value::image_bounds()` reads retained metadata without polling readiness or
acquiring a payload lease. Pending, Failed, and ProducerCancelled Values retain
that access; `buffer_handle()`, `DenseTensorView`, and `ImageView` payload
construction remain Ready-only. `ImageView` keeps zero-based storage-index
access and adds a separate signed logical-coordinate accessor.

### Channels, samples, storage, and color

An optional bounded `ChannelSchema` contains one nonzero unique `ChannelId` per
channel-axis element and canonically ordered `ChannelGroupId` records with
valid, duplicate-free members. Channel and group names are bounded diagnostics
only and do not affect roles, semantic equality, descriptor digest, or content
digest.

Storage-representable range derives only from
`ElementSemantics + StorageEncoding`. `QuantizationSchema` remains orthogonal.
A versioned `SampleEncoding` and `SampleDomainFacet` declare normalized, legal,
or code-value inclusive finite intervals, with bounded stable-ID per-channel
overrides. Declared domains neither alter storage capability nor authorize
conversion.

A versioned `ColorFacet` binds one existing nonempty `ChannelGroupId` to an
explicit scene-linear/sRGB/Rec.709/PQ/HLG transfer function and explicit
Rec.709/Display-P3-D65/Rec.2020/ACES-AP0/ACES-AP1 primaries. Scene linearity is
a color fact, not a sample-domain flag. Names never imply RGB or alpha.

`ImageFacet` is the sole C++ owner of these built-in ordinary-image facts.
Canonical encoding nevertheless emits independent Image, Sample Domain, and
Color Facet records so those meanings can version independently without a
second Value authority.

### Observed statistics

Observed min/max and fixed-bin histograms are independent bounded derived-data
records, never ImageFacet or Value members. A query contains canonical
`RegionSet`, exactly one stable channel/group selector, algorithm, positive
algorithm version, and algorithm parameters. A cache key contains a valid
`ValueRevisionId`, optional `ContentDigest`, and the complete query. Results
contain bounded, stable-ID-ordered per-channel counts, finite extrema, explicit
NaN/infinity counts, and histogram counts where requested.

Creating, replacing, or evicting a statistics result cannot change Value
revision, descriptor/content identity, formal cache validity, or artifact
identity. DI-1 defines and validates the records; it does not install a scan
engine or cache owner.

### Canonical versions and compatibility edges

The built-in DenseTensor Schema advances to structural version 2. Its existing
payload field order stays fixed. The Image Facet advances to structural version
2 and encodes axes, signed data/display windows, stable channel order, group
IDs, and memberships. Independent Sample Domain and Color records start at
structural version 1. Signed integers are two's-complement little endian;
finite binary64 metadata canonicalizes both signed zeros to positive zero.
Diagnostic names, readiness, bindings, leases, and observed statistics are
excluded.

There is no structural-version-1 fallback, compatibility alias, or missing-tail
guess. A later decoder must select an exact supported version and reject any
other shape.

The transitional `ImageBuffer -> Value` bridge creates an explicit zero-origin
data window and no display/channel-schema/sample/color facts. Reverse projection
copies active elements and knowingly loses richer metadata because ImageBuffer
cannot represent it. The current isolated CPU wire remains axis-only and
accepts only this zero-origin/no-rich-metadata projection; it rejects richer
facets before encoding rather than silently omitting fields.

## Consequences

- Negative and nonzero ordinary-image origins are first-class and deterministic.
- Every repository-owned ImageFacet construction must supply explicit bounds;
  there is no seal-time default or compatibility overload.
- Pure descriptor inference and interpretation-preserving built-in operations
  copy the complete bounded ImageFacet without payload access.
- Canonical golden digests change deliberately with structural version 2.
- Diagnostic spelling and derived statistics do not perturb semantic identity.
- Product wire/artifact/ABI migrations remain later slices and must encode the
  frozen records exactly or reject them.
- OpenEXR Deep provider windows remain provider-defined metadata and are not
  reused as built-in ordinary DenseImage authority.

## References

- [ADR 0008](0008-generic-values-memory-bindings-and-regions-are-explicit-versioned-contracts.md)
- [Kernel Data Model](../kernel-architecture/Data-Model.md)
- [ImageBuffer Memory Contract](../kernel-architecture/ImageBuffer-Memory-Contract.md)
- [Kernel Cache Model](../kernel-architecture/Cache-Model.md)
- [Dense Image Value Migration](../roadmap/Kernel-Evolution.md#dense-image-value-migration)
- GitHub Project 6 / parent issue #128 / issue #129
- OpenSpec change `define-dense-image-coordinate-sample-statistics-contracts`
