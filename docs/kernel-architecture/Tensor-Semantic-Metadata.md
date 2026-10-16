# Tensor descriptions and description editing (operations SDK)

[Chinese reader version](zh/Tensor-Semantic-Metadata.zh.md).

This page describes an operations-SDK configuration type. The kernel does not store, propagate, or validate tensor descriptions: Result and Tensor carry only structure, data, and storage owners. A `TensorDescription` exists only as an explicit static parameter of the operation that reads it.

## 1. Scope and ownership

`TensorDescription` records logical tensor, channel, axis, color-group, encoding, sampling, profile, and configured-space interpretation. The type and its codecs are declared in `photospider/ops/tensor_description.hpp`; the pure editor is declared in `photospider/ops/format/metadata.hpp`. Both belong to `Photospider::ops_sdk`, which depends only on the kernel's public interface.

The SDK owns the description value, its codec, and its editor. The caller owns the decision of which description applies to which operation input: a format or color operation receives it as a static String parameter (for example `input_description`), and that parameter enters the operation's compile identity. Immutable profile and configuration bytes are separate `ImmutableResource` handles; the caller passes them in a `ResourceSet` to `Compiler::compile`, and the operation's preparation selects them by `ResourceIdentity`. A description can name a profile identity, but it cannot create resource ownership.

A description does not own sample storage, certify sample values, or imply image layout.

## 2. Data layout and memory

```cpp
using TensorEndpoint =
    std::variant<std::int64_t, double, TensorRationalEndpoint>;

struct TensorDescription final {
  ResourceLease metadata_owner;
  std::optional<std::uint32_t> channel_axis;
  std::vector<TensorChannelDescription> channels;
  std::optional<TensorChannelDescription> component;
  std::vector<TensorAxisDescription> axes;
  std::string model, primaries, transfer, reference, association;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<ResourceIdentity> profile;
  std::vector<TensorColorGroup> groups;
  std::optional<TensorEncoding> encoding;
  std::optional<TensorSampling> sampling;
  std::string convention = "relative-v1";
  std::optional<TensorConfiguredSpace> configured;
  std::optional<TensorAnalyticBinding> analytic_binding;
  std::optional<TensorModelCoordinates> coordinates = {};
};

Result<std::vector<std::uint8_t>> encode_tensor_description(
    const TensorDescription&);
Result<TensorDescription> decode_tensor_description(
    const std::vector<std::uint8_t>&);
Result<std::string> tensor_description_parameter(const TensorDescription&);
Result<TensorDescription> tensor_description_from_parameter(
    const std::string&);
Status validate_tensor_description(const TensorDescription&,
                                   const TensorDescriptor&);
```

`TensorDescription` records ordered channels on an optional channel axis, one selected component, logical axes, global interpretation defaults, and complete color groups. `validate_tensor_description` checks the description against a `TensorDescriptor` without reading samples. Validation requires rank 1 through 8, a channel axis inside that rank, and either an empty axes table or one entry per logical axis. Each axis has a name, unit, finite origin, and positive finite step. A complete group names ordered distinct component indices and corresponding channel descriptions; its optional alpha index is within the same tensor and outside the color indices. Group and explicitly supplied channel declarations must agree.

`TensorEncoding` maps stored values to decoded values by

$$
D(x)=d_0 + (x-s_0)\frac{d_1-d_0}{s_1-s_0}.
$$

The stored endpoints must increase; decoded endpoints must differ and may descend. Stored endpoints must fit the tensor dtype. Endpoints retain their type as Int64, binary64, or a reduced exact rational. Rational magnitudes use little-endian base-2^32 limbs, with at most 128 words for each reduced numerator and positive denominator; integers never pass through binary64. The description states an interpretation only. It does not convert, clamp, or rescale samples. A complete integer color group needs an explicit decoder from its component, channel, or tensor default. Removing that decoder invalidates the complete native-color claim. Conflicting explicit encodings or sampling grids fail validation.

Internal color groups use same-size co-sited sampling: an explicit grid, scale `(1,1)`, and offset `(0,0)`. External subsampling belongs to an I/O codec. Configured spaces name a frozen config identity, canonical space, and `scene` or `display` reference. Analytic bindings are caller-supplied model/primaries/transfer/reference/white, ordered roles/units, and convention assertions; they do not prove mathematical equivalence.

Color coordinates allow empty or `relative`/`absolute` scale, an optional descriptive observer, a gray interpretation (`linear_y`, `encoded_luma`, `cielab_l`, or `oklab_l`), and optional finite binary64 NCL coefficients `[Kr,Kb]`. Empty fields make no assertion. `relative-v1` stores CIELAB/CIELCh lightness as `L*/100`, leaves a/b/chroma unchanged, and retains the existing XYZ Y=1 reference scale. It is not a range clamp. ICC/OCIO-native conventions refer to resource-defined coordinates. None of these fields establishes sample finiteness, alpha validity, or premultiplication.

The canonical encoding is bounded to 4096 bytes. Text is strict UTF-8 and at most 128 bytes; at most 128 groups are allowed, each with at most 64 components. The canonical little-endian codec preserves integer and IEEE binary64 bits, ordered tables, explicit presence bytes, and resource identities. Version 4 preserves its existing meaning; version 5 adds model-coordinate records. Encoding selects v5 when coordinates are present at any tensor, component, channel, or group level, including a present-but-empty record; otherwise it selects v4. Older versions, mismatched version/discriminator pairs, noncanonical encodings, and trailing bytes are rejected. `tensor_description_parameter` writes the canonical bytes as lowercase hexadecimal for a String parameter, and `tensor_description_from_parameter` accepts only that canonical form.

## 3. Execution: pure description editing

```cpp
namespace ps::format {
struct MetadataSet final {
  std::string path;
  MetadataValue value;
};
struct MetadataOptions final {
  std::string mode = "patch";        // "patch" or "replace"
  std::vector<MetadataSet> set;
  std::optional<TensorDescription> description;
  std::vector<std::string> remove;
  std::string dependencies = "error";  // "error" or "cascade"
  std::string missing = "error";       // "error" or "ignore"
};
Result<TensorDescription> edit_description(
    const TensorDescription& source, const TensorDescriptor& descriptor,
    const MetadataOptions& options = {});
}  // namespace ps::format
```

`edit_description` is a pure function. It validates `source` against `descriptor`, applies one transactional edit, validates the candidate, and returns a new description. It appends no workflow node, reads no payload, looks up no resource, and leaves `source` unchanged on failure. Allocation failure may throw `std::bad_alloc`. There is no `metadata.assign` or `metadata.remove` workflow operation; a caller that needs an edited description passes the returned value to the consuming operation as its static parameter.

```text
source description + descriptor
              |
       validate source
              |
   resolve paths on source -> build candidate -> validate candidate
                                                  +--------+--------+
                                                  |                 |
                                                valid            invalid
                                                  |                 |
                                    new TensorDescription    InvalidArgument;
                                                             source unchanged
```

`replace` requires a complete `description` (which may be empty) and accepts no path edits. `patch` forbids `description`. Only `/semantic` paths are accepted.

## 4. Algorithms and math

Paths are slash-separated; `~0` escapes `~` and `~1` escapes `/`. A path is at most 1024 bytes.

- `/semantic` selects the complete description.
- `/semantic/channels/index:0/unit` selects an original channel. `name:` and `role:` selectors require one exact unique match; `missing=ignore` can ignore a zero-match deletion, while ambiguous selectors still fail.
- `/semantic/groups/color/interpretation/primaries` selects a group field; `/semantic/axes/0/origin` selects an axis field.
- Coordinate subtree and leaf paths include `/semantic/coordinates`, `/semantic/component/interpretation/coordinates`, `/semantic/channels/index:0/interpretation/coordinates`, and `/semantic/groups/gray/interpretation/coordinates`, with the supported channel selectors.

Selectors resolve against the original source before edits. Whole-subtree sets replace the subtree; leaf sets preserve siblings. Duplicate paths, overlapping ancestor/descendant edits, and aliases that resolve to one target fail. A channel or axis description can be deleted while its data slot remains. `cascade` removes only affected old dependent descriptions and preserves every explicit set; if cleanup would erase a new explicit value, the transaction fails. Group order follows the source order in patch mode.

Coordinate assertions merge field by field across channel, component, and overlapping group declarations. Empty values make no assertion and cannot erase an earlier nonempty assertion. Unequal nonempty values for one field conflict. Exact `operator==` compares complete records and is not the compatibility predicate. The codec checks compatibility without rewriting provenance.

Internally, the editor encodes the options as a bounded v1 transaction tree: at most 4096 encoded bytes, 1024 nodes, and depth 12. The tree encoding uses `o` for ordered maps, `s` for strings, `u`/`i` for unsigned/exact signed integers, `d` for eight little-endian Float64 bytes, and `b` for opaque bytes, each with a decimal length or count and `:` delimiter. This transaction codec is separate from the TDM4/TDM5 description codec and is not a public parameter format.

`IccProfile` (`photospider/ops/icc_profile.hpp`) copies and admits explicit profile bytes. Admission checks v2/v4 bytes, profile class/PCS, required tags, TRC/LUT envelopes, and profile identity. It accepts RGB/Gray matrix/TRC profiles and admitted LUT forms; profile-defined XYZ/Lab endpoints require an explicit LUT, and the CMYK endpoint path requires its declared LUT set. Abstract and DeviceLink profiles are not endpoint resources. This structural admission does not run a color-management module. `resource()` returns the owning `ImmutableResource` for the caller's `ResourceSet`.

`OcioConfigResource` (`photospider/ops/ocio_config_resource.hpp`) seals an explicit snapshot: config bytes, the complete sorted logical file map, resolved context, declared canonical spaces/references, and pinned engine/build/settings identity. Its SHA-256 identity and byte length cover the framed bytes, including lookup absence. Admission validates snapshot structure and ownership, not transform executability. After freezing, lookup does not consult files, network, or process environment. Snapshot copies and comparisons consume `ResourceBudget` and cancellation/work checks in chunks of at most 1024 bytes; the file/context/space tables are limited to 1024 entries in total.

## 5. Limitations and non-goals

- A description is static operator configuration. It is not attached to a Result or Tensor, it does not flow along graph edges, and no operation output carries the input's description forward.
- The kernel does not reject a connection because two Results have different intended descriptions; structure alone decides port compatibility. An operation checks its explicit description against the input descriptor when it prepares, and a bound validator can check sample values.
- Tensor descriptions describe logical axes and interpretation; they do not define physical strides, planar/tiled storage, or image ownership.
- Profile identities do not carry resource bytes. Callers must provide the resources in the `ResourceSet` given to the compiler.
- A profile or configured-space declaration does not infer analytic primaries, transfer, or white. An analytic binding remains caller provenance.
- Description validation does not certify numeric sample range, finiteness, coverage, or color correctness.
- ICC admission is structural; OCIO admission validates the frozen snapshot rather than transform execution. No ICC or OCIO color-conversion engine is provided.
- Color-group and alpha indices name positions on the channel axis, and axis roles name logical dimensions; neither selects byte order. [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) D9 (Proposed) records the canonical image target.

Malformed descriptions and edits return `InvalidArgument` from the SDK call. A format operation decodes and validates its `input_description` parameter during static preparation, so a malformed parameter or a description that does not match the input descriptor fails compilation. ICC and OCIO resource admission charges the `ResourceBudget` supplied to the import call; resource admission, copy, or hash failure publishes no owner.
