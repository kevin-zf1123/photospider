#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps {
class OperationRegistry;
namespace format {
/** @brief Exact RN64 primary/white geometry. A preset can optionally be
 * accompanied by agreeing coordinate assertions. Custom geometry needs both
 * arrays. Presets choose no transfer function or luminance scaling. */
struct RgbBasis final {
  std::string preset;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<std::array<double, 2>> white;
};
/** @brief Closed FMT-10 static options. Semantic calls require an explicit
 * unique group name; raw calls require components and explicit geometry.
 * Fields not applicable to a member are rejected, not silently ignored. */
struct RgbBasisOptions final {
  std::string metadata_mode = "respect";
  std::string group;
  std::optional<std::array<std::uint64_t, 3>> components;
  std::optional<std::uint32_t> axis;
  std::optional<RgbBasis> source_basis, target_basis;
  std::optional<std::array<double, 2>> source_white, target_white;
  std::optional<std::string> method, white_handling;
  std::optional<TensorDescription> metadata_override;
  std::string layout = "auto";
  std::string profile = "strict";
};
/** @brief Canonical static parameter codecs. Basis presets serialize by name;
 * custom geometry uses versioned, lowercase, big-endian binary64 hex words.
 * Metadata signed zeros are canonicalized. Validation is exact and bounded. */
PHOTOSPIDER_API Result<std::string> rgb_basis_parameter(const RgbBasis& basis);
PHOTOSPIDER_API Result<std::string> xyz_white_parameter(
    const std::array<double, 2>& white);
/** @brief Transactional authoring helpers. Infer the actual input edge through
 * the supplied registry (default: built-ins); no caller-supplied metadata is
 * trusted. Stage complete expansion and publish only after static validation.
 * D emits A -> optional C -> B with all dtype rounding/failures preserved.
 * No native D key exists. Existing nodes/exports are unchanged on failure.
 * Helpers do not read samples. Serialize writes to the same document; plans
 * and their immutable preparations support concurrent execution. */
PHOTOSPIDER_API Result<WorkflowNodeOutput> rgb_to_xyz(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
PHOTOSPIDER_API Result<WorkflowNodeOutput> xyz_to_rgb(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
PHOTOSPIDER_API Result<WorkflowNodeOutput> adapt_xyz_white(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
PHOTOSPIDER_API Result<WorkflowNodeOutput> convert_linear_rgb(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
}  // namespace format
}  // namespace ps
