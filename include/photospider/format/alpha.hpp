#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "photospider/format/channel_editing.hpp"

namespace ps::format {
/** @brief An explicit alpha source declaration for FMT-04A/FMT-05A.
 *
 * `internal` selects from the original input using `channel`. `external_plane`
 * requires another single-tensor Result with the erased-cell-axis shape and
 * matching batch prefix. `scalar` requires another unbatched Result of shape
 * `[1]`. These remain ordinary graph inputs, not persistent metadata links.
 */
struct AlphaSource final {
  std::string kind = "internal";
  ChannelSelector channel;
};
/** @brief FMT-04A/FMT-04B authoring options.
 *
 * `metadata_mode` is `respect`, `override`, or `raw`; raw association requires
 * explicit `input_structure` and `components`. `axis` indexes a cell axis and
 * excludes the Result batch prefix. Semantic modes require `group`. `algorithm`
 * accepts `auto`, `scalar`, `simd`, or `reference`. These helpers append nodes
 * named
 * `alpha.associate_<profile>` or `alpha.unassociate_<profile>`. Both keys are
 * registered for the three CPU profiles in the default registry. Callers must
 * serialize mutations to the same `WorkflowDocument`.
 */
struct AlphaAssociationOptions final {
  std::string metadata_mode = "respect";
  std::optional<TensorDescription> metadata_override;
  std::string group;
  std::optional<std::uint32_t> axis;
  std::string input_structure;
  std::vector<std::uint64_t> components;
  std::optional<AlphaSource> alpha_source;
  std::string profile = "strict";
  std::string algorithm = "auto";
};
/** @brief Shared FMT-05 semantic and layout options.
 *
 * `axis` is an optional cell-axis assertion; it excludes the Result batch
 * prefix. The complete sample rank, including batch and cell axes, is at most
 * 8 and the sample count is at most 2^40. Nonscalar source batch prefixes must
 * match exactly; scalar sources are unbatched shape `[1]`. `group` is required
 * in semantic modes. For a component Gray input, it selects the exact component
 * name. FMT-05A has a native Result key; FMT-05B/C are compile-time
 * compositions over registered Result operations.
 */
struct AlphaEditOptions {
  std::string group;
  /** `respect` or `override`; raw metadata mode is unsupported. */
  std::string metadata_mode = "respect";
  std::optional<TensorDescription> metadata_override;
  std::optional<std::uint32_t> axis;
  std::string layout = "auto";
  std::string profile = "strict";
};
/** @brief FMT-05A source and placement options.
 *
 * `channel_index` is a final channel slot, not an axis. `output_axis` is a
 * cell-axis index and excludes the Result batch prefix. It is required when
 * adding alpha to component Gray input. The helper appends an
 * `alpha.set_<profile>` node, registered for the three CPU profiles in the
 * default runtime.
 */
struct SetAlphaOptions final : AlphaEditOptions {
  AlphaSource alpha_source;
  std::string placement = "preserve";
  std::optional<std::uint64_t> channel_index;
  std::optional<std::uint32_t> output_axis;
};
/** @brief FMT-05B extraction policy.
 *
 * `keepdims` preserves a singleton cell channel axis; rank-one channel input
 * requires it to avoid a rank-zero result. `missing_alpha=opaque` requests an
 * exactly encoded coverage-one constant when the selected group has no alpha.
 * `alpha_encoding` configures only that fallback and does not change an alpha
 * channel that exists.
 */
struct ExtractAlphaOptions final : AlphaEditOptions {
  bool keepdims = false;
  std::string missing_alpha = "error";
  std::optional<TensorEncoding> alpha_encoding;
};
/** @brief FMT-05C missing-alpha behavior: `error` or explicit `identity`. */
struct RemoveAlphaOptions final : AlphaEditOptions {
  std::string missing_alpha = "error";
};
/** @brief Append a node for FMT-04A alpha association.
 *
 * The helper reads no samples and appends transactionally. When `alpha_source`
 * selects `external_plane` or `scalar`, pass its additional input in `alpha`.
 * The returned `values` edge is graph-owned. The node names
 * `alpha.associate_<profile>`, which is registered for all three CPU profiles
 * in the default built-in runtime. Callers must serialize mutations to the same
 * `WorkflowDocument`.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> associate_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const AlphaAssociationOptions& options,
    std::optional<WorkflowInput> alpha = {});
/** @brief Append a node for FMT-04B alpha unassociation.
 *
 * Graph expansion and edge ownership match `associate_alpha`. The appended
 * `alpha.unassociate_<profile>` key is registered for all three CPU profiles.
 * Callers must serialize mutations to the same `WorkflowDocument`.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> unassociate_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const AlphaAssociationOptions& options,
    std::optional<WorkflowInput> alpha = {});
/** @brief Append a node for FMT-05A alpha setting/insertion.
 *
 * The helper validates the profile, option encoding and alpha edge arity; it
 * does not inspect input tensor metadata or read sample payloads. It appends
 * transactionally and returns a graph-owned `values` edge. The
 * `alpha.set_<profile>` key is registered for all three CPU profiles in the
 * current built-in registry. Callers must serialize mutations to the same
 * `WorkflowDocument`.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> set_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const SetAlphaOptions& options, std::optional<WorkflowInput> alpha = {});
/** @brief FMT-05B/C compile-time compositions, not new registry keys.
 *
 * `source_metadata` must describe an input Result with exactly one tensor and
 * no fields. Its schema assertion includes semantic metadata and spatial axes;
 * a separate layout assertion covers physical storage order and row pitch.
 * `axis` in the options is relative to cell axes and excludes the batch prefix.
 * Generated primitives verify assertions at compile time, including forward
 * producer references. No sample reads occur. Existing alpha/surviving
 * channels are exact same-dtype copies; missing-alpha opaque constants are
 * solved in exact descriptor arithmetic. Callers must serialize mutations to
 * the same `WorkflowDocument`.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> extract_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const OperationMetadata& source_metadata,
    const ExtractAlphaOptions& options);
/** @brief Append the FMT-05C removal composition, not a new registry key.
 *
 * `source_metadata` must describe an input Result with exactly one tensor and
 * no fields. `axis` in the options is relative to cell axes and excludes the
 * batch prefix. The helper preserves surviving sample bits. A channel-bearing
 * source lowers to registered mapped assembly. Component Gray metadata-only
 * identity lowers to `metadata.assign`, which still reads and republishes
 * same-coordinate Data. All connected edge descriptors are checked at compile
 * time. Expansion is transactional and the returned `values` edge is graph-
 * owned. Callers must serialize mutations to the same `WorkflowDocument`.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> remove_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const OperationMetadata& source_metadata,
    const RemoveAlphaOptions& options);
}  // namespace ps::format
