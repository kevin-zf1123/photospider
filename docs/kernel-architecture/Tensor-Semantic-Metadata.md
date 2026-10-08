# Tensor semantic metadata and atomic editing

[Chinese reader version](zh/Tensor-Semantic-Metadata.zh.md).

## 1. Scope and ownership

`TensorDescription` carries logical tensor, channel, axis, color-group, encoding, sampling, profile, and configured-space descriptions. It does not own sample storage, certify sample values, or imply image layout. `ResourceBindings` owns immutable ICC profiles and frozen OCIO snapshots referenced by metadata. A Result owns its immutable schema and facets; a Result view can retain source backing and resource owners independently of the metadata header.

## 2. Data layout and memory

```cpp
using TensorEndpoint = std::variant<std::int64_t, double, TensorRationalEndpoint>;
struct TensorDescription final {
  std::optional<std::uint32_t> channel_axis;
  std::vector<TensorChannelDescription> channels;
  std::optional<TensorChannelDescription> component;
  std::vector<TensorAxisDescription> axes;
  std::string model, primaries, transfer, reference, association;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<ColorProfileIdentity> profile;
  std::vector<TensorColorGroup> groups;
  std::optional<TensorEncoding> encoding;
  std::optional<TensorSampling> sampling;
  std::string convention = "relative-v1";
  std::optional<TensorConfiguredSpace> configured;
  std::optional<TensorAnalyticBinding> analytic_binding;
  std::optional<TensorModelCoordinates> coordinates;
};
Result<ValueFacet> encode_tensor_description(const TensorDescription&);
Result<TensorDescription> decode_tensor_description(const ValueFacet&);
```

`TensorDescription` records ordered channels on an optional channel axis, one selected component, logical axes, global interpretation defaults, and complete color groups. Validation requires rank 1 through 8, a channel axis inside that rank, and either an empty axes table or one entry per logical axis. Each axis has a name, unit, finite origin, and positive finite step. A complete group names ordered distinct component indices and corresponding channel descriptions; its optional alpha index is within the same tensor and outside the color indices. Group and explicitly supplied channel declarations must agree.

`TensorEncoding` maps stored values to decoded values by

$$
D(x)=d_0 + (x-s_0)\frac{d_1-d_0}{s_1-s_0}.
$$

The stored endpoints must increase; decoded endpoints must differ and may descend. Stored endpoints must fit the tensor dtype. Endpoints retain their type as Int64, binary64, or a reduced exact rational. Rational magnitudes use little-endian base-2^32 limbs, with at most 128 words for each reduced numerator and positive denominator; integers never pass through binary64. The description attaches interpretation only. It does not convert, clamp, or rescale samples. A complete integer color group needs an explicit decoder from its component, channel, or tensor default. Removing that decoder invalidates the complete native-color claim. Conflicting explicit encodings or sampling grids fail validation.

Internal color groups use same-size co-sited sampling: an explicit grid, scale `(1,1)`, and offset `(0,0)`. External subsampling belongs to an I/O codec. Configured spaces name a frozen config identity, canonical space, and `scene` or `display` reference. Analytic bindings are caller-supplied model/primaries/transfer/reference/white, ordered roles/units, and convention assertions; they do not prove mathematical equivalence.

Color coordinates allow empty or `relative`/`absolute` scale, an optional descriptive observer, a gray interpretation (`linear_y`, `encoded_luma`, `cielab_l`, or `oklab_l`), and optional finite binary64 NCL coefficients `[Kr,Kb]`. Empty fields make no assertion. `relative-v1` stores CIELAB/CIELCh lightness as `L*/100`, leaves a/b/chroma unchanged, and retains the existing XYZ Y=1 reference scale. It is not a range clamp. ICC/OCIO-native conventions refer to resource-defined coordinates. None of these fields establishes sample finiteness, alpha validity, or premultiplication.

The `photospider.tensor-description` facet is bounded to 4096 bytes. Text is strict UTF-8 and at most 128 bytes; at most 128 groups are allowed, each with at most 64 components. The canonical little-endian codec preserves integer and IEEE binary64 bits, ordered tables, explicit presence bytes, and resource identities. Version 4 preserves its existing meaning; version 5 adds model-coordinate records. Encoding selects v5 when coordinates are present at any tensor, component, channel, or group level, including a present-but-empty record; otherwise it selects v4. Older versions, mismatched version/discriminator pairs, noncanonical encodings, and trailing bytes are rejected. Opaque annotations are separate facets on the Result tensor and retain the host's normal facet limits.

## 3. Execution and state machine

`ps::format::assign_metadata` appends one node after checking edit syntax, types, options, path overlap, and bounded transaction encoding. The compiler then resolves source-relative selectors and validates the complete candidate. Current registrations use Result operation ABI 2, WorkflowDocument 5, OperationTraits 24 and package 0.32.0. Each input Result contains one tensor member and no fields. An internal node may supply an optional canonical-schema assertion; the public helper does not set it.

```text
authoring: validate edit syntax -> encode bounded transaction -> append node
                                                        |
compile: resolve selectors on source -> build candidate -> validate
                                                +----------+----------+
                                                |                     |
                                              valid                invalid
                                                |                     |
runtime: exact coordinate mapping -> view/copy  compiler error; no output
```

The public helpers are in `ps::format`:

```cpp
struct MetadataOptions final {
  std::string mode = "patch";
  std::vector<MetadataSet> set;
  std::optional<TensorDescription> description;
  std::vector<std::string> remove;
  std::string dependencies = "error", missing = "error";
  std::string layout = "auto", profile = "strict";
};
Result<WorkflowNodeOutput> assign_metadata(
    WorkflowDocument&, WorkflowInput, const MetadataOptions& = {});
Result<WorkflowNodeOutput> remove_metadata(
    WorkflowDocument&, WorkflowInput, const std::vector<std::string>&,
    const MetadataOptions& = {});
```

Defaults are `mode=patch`, `dependencies=error`, `missing=error`, `layout=auto`, and `profile=strict`. Registered CPU profiles are `strict`, `accelerated_apple_silicon`, and `accelerated_x86_64`; they produce identical metadata and sample bits under their backend admission rules. Replace requires a complete `description` (which may be empty), retains opaque annotations, and permits only annotation edits. Patch forbids `description`. `remove_metadata` lowers a deletion-only patch through the same authoring path. Malformed authoring leaves the document unchanged. Source-relative selectors and target structure are checked during compilation. The output schema retains the source schema id, tensor key, descriptor, batches and physical layout, changing only the semantic and edited annotation facets.

For requested output footprint Q, the Result continuation requests Data support on Q and Descriptor support for the source description using role mask 9 (Data 1 | Descriptor 8). It requests no pixel Validation or Control support. Dependency-v2 maps each output coordinate to the same input coordinate, so a changed sample dirties the corresponding output sample. Empty output demand uses a stateless continuation and publishes an empty Result without requesting payload.

`auto` and `view` publish a Result view retaining the source backing, resources and association when the requested mapping can be represented. `auto` falls back to materialization only after `ViewUnavailable`; forced `view` reports that error. `materialize` allocates requested output coverage and copies sample bytes transactionally. Copy work checks cancellation at most every 256 samples. Positive, negative and zero-stride generic tensors are supported when their Result views are valid. For spatial layouts the implementation can preserve the source physical owner and DAG tile geometry. The operation retains the source publication policy and disables Result caching.

## 4. Algorithms and math

Paths are slash-separated; `~0` escapes `~` and `~1` escapes `/`.

- `/semantic` selects the complete semantic record.
- `/semantic/channels/index:0/unit` selects an original channel. `name:` and `role:` selectors require one exact unique match; `missing=ignore` can ignore a zero-match deletion, while ambiguous selectors still fail.
- `/semantic/groups/color/interpretation/primaries` selects a group field; `/semantic/axes/0/origin` selects an axis field.
- Coordinate subtree and leaf paths include `/semantic/coordinates`, `/semantic/component/interpretation/coordinates`, `/semantic/channels/index:0/interpretation/coordinates`, and `/semantic/groups/gray/interpretation/coordinates`, with the supported channel selectors.
- `/annotations/app.note` selects an opaque facet. The `photospider.` namespace is reserved for semantic facets.

Selectors resolve against the original source before edits. Whole-subtree sets replace the subtree; leaf sets preserve siblings. Duplicate paths, overlapping ancestor/descendant edits, and aliases that resolve to one target fail. A channel or axis description can be deleted while its data slot remains. Cascade removes only affected old dependent descriptions and preserves every explicit set; if cleanup would erase a new explicit value, the transaction fails. The compiler publishes one immutable candidate without modifying the source or repairing samples.

Coordinate assertions merge field by field across channel, component, and overlapping group declarations. Empty values make no assertion and cannot erase an earlier nonempty assertion. Unequal nonempty values for one field conflict. Exact `operator==` compares complete records and is not the compatibility predicate. The codec checks compatibility without rewriting provenance; channel assembly resolves overlays in its output description.

The edit transaction is a bounded v1 record encoded as lowercase hexadecimal in the node's `edits` string. Its decoded form is at most 4096 bytes, 1024 nodes, and depth 12; the hex String is at most 8192 bytes. The tree encoding uses `o` for ordered maps, `s` for strings, `u`/`i` for unsigned/exact signed integers, `d` for eight little-endian Float64 bytes, and `b` for opaque bytes, each with a decimal length or count and `:` delimiter. This transaction codec is separate from the TDM4/TDM5 facet codec. Prefer `MetadataOptions` and typed paths over manual encoding.

`ResourceBindings` seals and deduplicates explicit ICC and OCIO handles. Every referenced identity must resolve at compile, binding, and output admission, and an ICC header model must match the declared interpretation. ICC admission checks explicit v2/v4 bytes, profile class/PCS, required tags, TRC/LUT envelopes, and profile identity. It accepts RGB/Gray matrix/TRC profiles and admitted LUT forms; profile-defined XYZ/Lab endpoints require an explicit LUT, and the CMYK endpoint path requires its declared LUT set. Abstract and DeviceLink profiles are not endpoint resources. This structural admission does not run a color-management module.

An OCIO snapshot includes explicit config bytes, the complete sorted logical file map, resolved context, declared canonical spaces/references, and pinned engine/build/settings identity. Its hash and byte length cover the framed bytes, including lookup absence. Admission validates snapshot structure and ownership, not transform executability. After freezing, lookup does not consult files, network, or process environment. Snapshot copies and comparisons consume `ResourceBudget` and cancellation/work checks in chunks of at most 1024 bytes; the file/context/space tables are limited to 1024 entries in total. `ValueFragments::retained_bytes` deduplicates actual resource storage identities, including config manifests.

## 5. Limitations and non-goals

- Tensor metadata describes logical axes and interpretation; it does not define physical strides, planar/tiled storage, or image ownership.
- Profile identities do not carry resource bytes. Callers must provide resolvable handles in `ResourceBindings`.
- A profile/configured-space declaration does not infer analytic primaries, transfer, or white. An analytic binding remains caller provenance.
- Metadata validation does not certify numeric sample range, finiteness, coverage, or color correctness. Registered `photospider.` facets other than `photospider.tensor-description` require an explicit import before this editor can consume them.
- ICC admission is structural; OCIO admission validates the frozen snapshot rather than transform execution. Metadata assignment does not run color-management conversion or introduce a private worker pool. See [ICC profile validation](../../src/lib/data/icc_validation.cpp) for the accepted profile classes, tags, and LUT envelopes.
- Static authoring is bounded by facet and transaction limits. Its preparation path has no runtime cancellation token; runtime resource admission uses the applicable root budget and cancellation checks.
- Metadata edits disable sample-only cache reuse because output identity includes metadata.

Resource admission, copy, or hash failure prevents publication and releases unpublished owners. A Result header can drop one resource reference while an older view still retains sample backing; removing a facet does not promise immediate release of every ancestor allocation. The focused Result CTest set passes 5/5, the public workflow passes, and the installed consumer passes 2/2 checks. These tests do not exercise a cross-family workflow through `channel.extract` or `channel.assemble`. The older [metadata_performance](../../examples/metadata_performance/README.md) workload measures Value/planar execution and is not performance evidence for these Result operations.
