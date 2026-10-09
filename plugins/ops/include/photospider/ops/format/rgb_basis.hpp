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
/** @brief Static RGB primary and white geometry for FMT-10.
 *
 * `primaries_xy` stores `{Rx, Ry, Gx, Gy, Bx, By}` and `white` stores `{x, y}`.
 * A named preset may be accompanied by coordinate assertions, which must match
 * its canonical values exactly. Custom geometry requires both arrays. Geometry
 * defines neither a transfer function nor a luminance scaling. */
struct RgbBasis final {
  std::string preset;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<std::array<double, 2>> white;
};
/** @brief Closed static options for FMT-10A/B/C and the D authoring helper.
 *
 * `metadata_mode` defaults to `respect`; `layout` defaults to `auto`, and
 * `profile` defaults to `strict`. Semantic options select a unique `group`;
 * raw options supply an ordered `components` triple. Raw `axis` may be given
 * explicitly or resolved from the input TensorDescription. Raw geometry is
 * member-specific. C resolves `source_white` from XYZ metadata in semantic mode
 * and requires it explicitly in raw mode. Basis, white, adaptation and
 * white-policy fields apply only to their documented members. Fields that do
 * not apply to the chosen member cause an error. */
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
/** @brief Encode exact static RGB geometry for workflow parameters.
 *
 * Presets use their canonical names. Custom geometry is encoded as bounded
 * `basis-v1:` text containing lowercase hexadecimal Float64 words in
 * big-endian field order; signed zeros are canonicalized. Singular, malformed
 * or non-finite geometry and insufficient exact-work capacity return an error.
 *
 * @param basis Preset or complete custom primary/white geometry.
 * @return Canonical static parameter text on success. */
PHOTOSPIDER_API Result<std::string> rgb_basis_parameter(const RgbBasis& basis);
/** @brief Encode a finite, valid white chromaticity as canonical bounded
 * `xy-v1:` Float64 words. Invalid xy or insufficient exact-work capacity
 * returns an error.
 *
 * @param white White chromaticity `{x, y}`.
 * @return Canonical static parameter text on success. */
PHOTOSPIDER_API Result<std::string> xyz_white_parameter(
    const std::array<double, 2>& white);
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
 * @param options Member-specific static metadata, geometry, policy and layout.
 * @param registry Operation registry used to infer and prepare graph edges;
 * defaults to the built-in registry.
 * @return The appended `values` edge on success; a status describing invalid
 * options, graph edges, geometry, capacity or backend availability on failure.
 */
PHOTOSPIDER_API Result<WorkflowNodeOutput> rgb_to_xyz(
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
PHOTOSPIDER_API Result<WorkflowNodeOutput> xyz_to_rgb(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
/** @brief Append FMT-10C and return its `values` output edge.
 *
 * Semantic mode resolves the source white from XYZ metadata; raw mode requires
 * an explicit source white. Both modes require a target white and method.
 * Writes to the document are transactional as described on `rgb_to_xyz`.
 *
 * @param document Workflow graph to extend.
 * @param input Existing workflow input edge.
 * @param options FMT-10C white and method options.
 * @param registry Registry used for edge inference; defaults to built-ins.
 * @return The appended output edge or the preparation/validation status. */
PHOTOSPIDER_API Result<WorkflowNodeOutput> adapt_xyz_white(
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
PHOTOSPIDER_API Result<WorkflowNodeOutput> convert_linear_rgb(
    WorkflowDocument& document, WorkflowInput input,
    const RgbBasisOptions& options,
    std::shared_ptr<OperationRegistry> registry = {});
}  // namespace format
}  // namespace ps
