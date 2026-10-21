#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/compiler/workflow_document.hpp"
#include "photospider/data/resource_set.hpp"
#include "photospider/ops/export.hpp"
#include "photospider/ops/tensor_description.hpp"

namespace ps {
class OperationRegistry;
namespace format {
/** @brief One explicitly configured FMT-15C stage.
 *
 * operation names an FMT-09/10 CPU key, FMT-14A/B/C, FMT-15A/B or FMT-13A..F.
 * The FMT-10D authoring member is named "color.convert_linear_rgb"; its
 * parameters include an explicit profile and the ordinary FMT-10 options.
 * input_description and output_description assert the complete transition.
 * parameters supplies group and applicable operation options. Raw selectors
 * and an additional input_description parameter are forbidden.
 *
 * FMT-13 stages use the resource-taking overload. Complete engine construction
 * validates the closed configuration and selected route before graph mutation.
 * D/E/F endpoint declarations are caller assertions, not inferred LUT meaning.
 * This helper does not introduce native C or FMT-10D registrations.
 */
struct ViewStage final {
  std::string operation;
  std::map<std::string, ParameterValue> parameters;
  TensorDescription input_description;
  TensorDescription output_description;
};

/** @brief Explicit native display conditions for a view recipe.
 *
 * description is the final complete interpretation; group selects its RGB or
 * linear-Y/encoded Gray group (empty for an axis-free Gray component).
 * black_nits/white_nits must be finite with 0 <= black < white. They are
 * descriptive unless a listed display-range/BT.1886 stage uses them, in which
 * case their exact values must agree with that stage. PQ white is <=10000 nits.
 * No monitor calibration, viewing environment or exposure is inferred.
 */
struct NativeDisplayConditions final {
  TensorDescription description;
  std::string group;
  double black_nits = 0;
  double white_nits = 0;
};

/** @brief Config-defined display endpoint with frozen context and files.
 *
 * config identifies the complete OcioConfigResource manifest. display/view
 * use exact config names. The output space is resolved from that actual view.
 * No analytic units, black level or peak luminance are inferred.
 */
struct ConfiguredDisplayConditions final {
  TensorDescription description;
  std::string group;
  ResourceIdentity config;
  std::string display;
  std::string view;
  std::string reference_bridge = "reject";
};

/** @brief Append an explicit view recipe atomically.
 *
 * Validates the real input edge, every complete input/output interpretation,
 * geometry, units, transfer, reference, encoding, profile bindings and final
 * display conditions before committing the staged graph. Retains all stage
 * rounding, dependencies, materialization, errors and resource behavior.
 * Does not fuse, reorder or elide nodes. Invalid recipes leave nodes and
 * exports unchanged. Callers serialize mutations of document.
 *
 * @param document Workflow to extend.
 * @param input Existing source edge.
 * @param entry Complete entry interpretation.
 * @param stages Nonempty ordered stage list.
 * @param display Complete final native display interpretation and conditions.
 * @param registry Registry for graph inference; defaults to built-ins.
 * @return The last values edge, or the first validation/preparation failure.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> compose_view(
    WorkflowDocument& document, WorkflowInput input,
    const TensorDescription& entry, const std::vector<ViewStage>& stages,
    const NativeDisplayConditions& display,
    std::shared_ptr<OperationRegistry> registry = {});
/** @brief Native display recipe that may contain explicit OCIO stages.
 * Resources are used for synchronous construction validation and remain owned
 * by the caller's ResourceSet; graph nodes serialize only immutable identities.
 */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> compose_view(
    WorkflowDocument& document, WorkflowInput input,
    const TensorDescription& entry, const std::vector<ViewStage>& stages,
    const NativeDisplayConditions& display, const ResourceSet& resources,
    std::shared_ptr<OperationRegistry> registry = {});
/** @brief Configured display recipe with the same atomic stage semantics. */
PHOTOSPIDER_OPS_API Result<WorkflowNodeOutput> compose_view(
    WorkflowDocument& document, WorkflowInput input,
    const TensorDescription& entry, const std::vector<ViewStage>& stages,
    const ConfiguredDisplayConditions& display, const ResourceSet& resources,
    std::shared_ptr<OperationRegistry> registry = {});
}  // namespace format
}  // namespace ps
