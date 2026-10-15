#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/ops/export.hpp"
#include "photospider/ops/format/rgb_geometry.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps {
class OperationRegistry;
namespace format {
/** @brief Closed static options for FMT-10A/B/C and the D authoring helper.
 *
 * `input_description` supplies explicit color interpretation and optional group
 * selection. Without it, supply ordered components, axis and member-specific
 * geometry. `source_white` is required for raw XYZ adaptation. `layout`
 * defaults to `auto`, and `profile` defaults to `strict`. Inapplicable basis,
 * white or adaptation parameters are rejected. Results retain only structure
 * and samples; callers supply configuration to each downstream operation. */
struct RgbBasisOptions final {
  std::string group;
  std::optional<std::array<std::uint64_t, 3>> components;
  std::optional<std::uint32_t> axis;
  std::optional<RgbBasis> source_basis, target_basis;
  std::optional<std::array<double, 2>> source_white, target_white;
  std::optional<std::string> method, white_handling;
  std::optional<TensorDescription> input_description;
  std::string layout = "auto";
  std::string profile = "strict";
};
/** @brief Append an FMT-10 operation to a workflow transactionally.
 *
 * The helper resolves the actual input edge through `registry` (the built-in
 * registry when omitted), validates static metadata/options, stages the node,
 * and mutates `document` only after success. It returns the appended node's
 * `values` output edge. Helpers do not read sample payloads. Serialize writes
 * to one WorkflowDocument; compiled plans and immutable prepared state may be
 * executed concurrently. Failures leave the document unchanged.
 *
 * @param document Workflow graph to extend.
 * @param input Existing workflow input edge.
 * @param options Explicit descriptions, geometry, policy and layout.
 * @param registry Operation registry used to infer and prepare graph edges;
 * defaults to the built-in registry.
 * @return The appended `values` edge on success; a status describing invalid
 * options, graph edges, geometry, capacity or backend availability on failure.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> rgb_to_xyz(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
/** @brief Append FMT-10B and return its `values` output edge.
 *
 * Static options and mutation guarantees are described on `rgb_to_xyz`.
 *
 * @param document Workflow graph to extend.
 * @param input Existing workflow input edge.
 * @param options FMT-10B source and target options.
 * @param registry Registry used for edge inference; defaults to built-ins.
 * @return The appended output edge or the preparation/validation status. */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> xyz_to_rgb(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
/** @brief Append FMT-10C and return its `values` output edge.
 *
 * An explicit `input_description` supplies the XYZ source white. Without that
 * description, `source_white` is required. Both paths require `target_white`
 * and `method`.
 * Writes to the document are transactional as described on `rgb_to_xyz`.
 *
 * @param document Workflow graph to extend.
 * @param input Existing workflow input edge.
 * @param options FMT-10C white and method options.
 * @param registry Registry used for edge inference; defaults to built-ins.
 * @return The appended output edge or the preparation/validation status. */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> adapt_xyz_white(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
/** @brief Transactionally append A -> optional C -> B and return B's output.
 *
 * This helper does not create a native D operation. It preserves each stage's
 * rounding, dependency and failure behavior.
 *
 * @param document Workflow graph to extend.
 * @param input Existing workflow input edge.
 * @param options Source/target basis and white-handling options for the stages.
 * @param registry Registry used for edge inference; defaults to built-ins.
 * @return The final B output edge or the first static preparation failure. */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> convert_linear_rgb(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
}  // namespace format
}  // namespace ps
