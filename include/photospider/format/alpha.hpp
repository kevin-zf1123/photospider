#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "photospider/format/channel_editing.hpp"

namespace ps::format {
/** @brief An explicit weight source. Internal selectors address the ORIGINAL
 * input. External planes have the erased-channel shape; scalars have shape [1].
 * These are ordinary input edges, never persistent metadata attachments. */
struct AlphaSource final {
  std::string kind = "internal";
  ChannelSelector channel;
};
/** @brief FMT-04 source interpretation and CPU arithmetic policy. Raw requires
 * input_structure, components and alpha_source. Semantic modes require group.
 * algorithm: auto, scalar, simd, reference. All have identical numerical rules;
 * reference uses NUM fixed-capacity integer ratios and exact rounding,
 * independently of native floating multiply/divide. */
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
/** @brief FMT-05 semantic options. Raw is deliberately not supported. For a
 * component Gray input, group selects its exact component name. */
struct AlphaEditOptions {
  std::string group;
  std::string metadata_mode = "respect";
  std::optional<TensorDescription> metadata_override;
  std::optional<std::uint32_t> axis;
  std::string layout = "auto";
  std::string profile = "strict";
};
struct SetAlphaOptions final : AlphaEditOptions {
  AlphaSource alpha_source;
  std::string placement = "preserve";
  std::optional<std::uint64_t> channel_index;
  std::optional<std::uint32_t> output_axis;
};
struct ExtractAlphaOptions final : AlphaEditOptions {
  bool keepdims = false;
  std::string missing_alpha = "error";
  std::optional<TensorEncoding> alpha_encoding;
};
struct RemoveAlphaOptions final : AlphaEditOptions {
  std::string missing_alpha = "error";
};
/** @brief Append one native primitive, without reading samples. Static source
 * checks run during compile/direct preparation. Helpers are transactional,
 * including allocation failure; returned handles are not exported roots. */
PHOTOSPIDER_API Result<WorkflowNodeOutput> associate_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const AlphaAssociationOptions& options,
    std::optional<WorkflowInput> alpha = {});
PHOTOSPIDER_API Result<WorkflowNodeOutput> unassociate_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const AlphaAssociationOptions& options,
    std::optional<WorkflowInput> alpha = {});
PHOTOSPIDER_API Result<WorkflowNodeOutput> set_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const SetAlphaOptions& options, std::optional<WorkflowInput> alpha = {});
/** @brief FMT-05B/C compile-time compositions, not new registry keys. Supplied
 * metadata must be the edge's inferred metadata; generated primitives assert
 * it again at compile time, including forward producer references. No sample
 * reads occur. Existing alpha/surviving channels are exact same-dtype copies.
 * Missing-alpha opaque constants are solved in exact descriptor arithmetic. */
PHOTOSPIDER_API Result<WorkflowNodeOutput> extract_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const OperationMetadata& source_metadata,
    const ExtractAlphaOptions& options);
PHOTOSPIDER_API Result<WorkflowNodeOutput> remove_alpha(
    WorkflowDocument& document, WorkflowInput input,
    const OperationMetadata& source_metadata,
    const RemoveAlphaOptions& options);
}  // namespace ps::format
