#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "photospider/ops/export.hpp"
#include "photospider/ops/format/channel_editing.hpp"

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
 * `input_description` supplies explicit interpretation; raw association
 * requires explicit `input_structure` and `components`. `axis` indexes a cell
 * axis and excludes the Result batch prefix. An explicit description requires
 * `group`.
 * `algorithm` accepts `auto`, `scalar`, `simd`, or `reference`. These helpers
 * append nodes named `alpha.associate_<profile>` or
 * `alpha.unassociate_<profile>`. Both keys are registered for the three CPU
 * profiles with fixed invocation ABI 8. Semantic arithmetic requires finite
 * participating colors, finite alpha in [0,1], and a finite rounded result;
 * unassociation at zero alpha requires zero color. These caller premises cover
 * all evaluated coordinates in retained output tiles. Raw arithmetic preserves
 * NUM IEEE results. Kernels use fixed source-tile closures and independent
 * output allocations. Callers must serialize mutations to the
 * same `WorkflowDocument`.
 */
struct AlphaAssociationOptions final {
  std::optional<TensorDescription> input_description;
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
 * to select from `input_description`. For a component Gray input, it selects
 * the exact component
 * name. FMT-05A has a native Result key; FMT-05B/C are compile-time
 * compositions over registered Result operations.
 */
struct AlphaEditOptions {
  std::string group;
  /** Explicit source interpretation used by this authoring call. */
  std::optional<TensorDescription> input_description;
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
 * default runtime. Auto/materialize copy independent dense output tiles with
 * zero scratch. Nonempty view fails ViewUnavailable with schema-only input
 * dependencies; Empty creates no task. Selected colors and represented alpha
 * obey semantic caller premises, without runtime validation-only reads.
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
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> associate_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const AlphaAssociationOptions& options,
    std::optional<WorkflowInput> alpha = {});
/** @brief Append a node for FMT-04B alpha unassociation.
 *
 * Graph expansion and edge ownership match `associate_alpha`. The appended
 * `alpha.unassociate_<profile>` key is registered for all three CPU profiles.
 * Callers must serialize mutations to the same `WorkflowDocument`.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> unassociate_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const AlphaAssociationOptions& options,
    std::optional<WorkflowInput> alpha = {});
/** @brief Append a node for FMT-05A alpha setting/insertion.
 *
 * The helper validates the profile, option encoding and alpha edge arity; it
 * does not inspect input tensor metadata or read sample payloads. It appends
 * transactionally and returns a graph-owned `values` edge. The
 * `alpha.set_<profile>` key is registered for all three CPU profiles in the
 * current built-in registry. Each output slot reads only its mapped source;
 * copying colors adds no alpha payload dependency. Complete output tiles can
 * still contain alpha slots and require their source. Produced Results retain
 * independent storage after input/context destruction. Callers must serialize
 * mutations to the same `WorkflowDocument`.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> set_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const SetAlphaOptions& options, std::optional<WorkflowInput> alpha = {});
/** @brief FMT-05B/C compile-time compositions, not new registry keys.
 *
 * `source_metadata` must describe an input Result with exactly one tensor and
 * no fields. Its schema assertion covers the canonical structure, including
 * schema identity, dtype, shape, batch axes, atomic axes and spatial axes.
 * A separate topology assertion records spatial height/width/channel axes.
 * Physical Plan and Run binding checks validate strides, offsets and capacity.
 * `axis` in the options is relative to cell axes and excludes the batch prefix.
 * Generated primitives verify assertions at compile time, including forward
 * producer references. No sample reads occur. Existing alpha/surviving
 * channels are exact same-dtype copies; missing-alpha opaque constants are
 * solved in exact descriptor arithmetic. Both extraction branches use fixed
 * CPU output tiles; auto/materialize copy or fill, and nonempty view fails
 * InvalidArgument. The opaque branch uses fixed CPU
 * tasks with schema-only source dependencies and zero scratch. Auto/materialize
 * fill selected output tiles.
 * Empty executes no fill, and retained outputs own their storage independently
 * of the source and execution context. Callers must serialize mutations to the
 * same `WorkflowDocument`.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> extract_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const OperationMetadata& source_metadata,
    const ExtractAlphaOptions& options);
/** @brief Append the FMT-05C removal composition, not a new registry key.
 *
 * `source_metadata` must describe an input Result with exactly one tensor and
 * no fields. `axis` in the options is relative to cell axes and excludes the
 * batch prefix. The helper preserves surviving sample bits. A channel-bearing
 * source lowers to fixed mapped assembly: auto/materialize copy independent
 * output tiles, while nonempty view fails with schema-only dependencies. An
 * axis-free component uses core.identity COPY tasks to preserve sample bits,
 * without a source-schema assertion or applying the helper layout policy.
 * Mapped assembly checks source assertions at
 * compile time. Expansion is transactional. The graph-owned
 * edge names `values` for mapped assembly and `value` for `core.identity`.
 * Callers must serialize mutations to the same `WorkflowDocument`.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> remove_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const OperationMetadata& source_metadata,
    const RemoveAlphaOptions& options);
}  // namespace ps::format
