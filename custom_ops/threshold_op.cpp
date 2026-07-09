#include <string>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "plugin_api.hpp"  // NOLINT(build/include_subdir)

namespace {

/**
 * @brief Reads a numeric YAML parameter with a fallback value.
 *
 * @param n Parameter map to read.
 * @param key Parameter key.
 * @param defv Fallback value used for missing, non-scalar, or invalid input.
 * @return Parsed double value or `defv`.
 * @throws Nothing; parse failures are reduced to the fallback value.
 * @note The plugin keeps this helper local so invalid optional parameters do
 * not fail loading or planning.
 */
double as_double_flexible(const YAML::Node& n, const std::string& key,
                          double defv) {
  if (!n || !n[key])
    return defv;
  try {
    if (n[key].IsScalar())
      return n[key].as<double>();
    return defv;
  } catch (...) {
    return defv;
  }
}

/**
 * @brief Reads a string YAML parameter with a fallback value.
 *
 * @param n Parameter map to read.
 * @param key Parameter key.
 * @param defv Fallback value used for missing or invalid input.
 * @return Parsed string value or `defv`.
 * @throws Nothing; parse failures are reduced to the fallback value.
 * @note Threshold mode parsing is intentionally permissive for plugin examples.
 */
std::string as_str(const YAML::Node& n, const std::string& key,
                   const std::string& defv) {
  if (!n || !n[key])
    return defv;
  try {
    return n[key].as<std::string>();
  } catch (...) {
    return defv;
  }
}

/**
 * @brief Maps threshold dirty demand directly to its input ROI.
 *
 * Threshold is pointwise: each output pixel depends only on the input pixel at
 * the same coordinate plus scalar node parameters. No halo or data-dependent
 * LUT is required for ROI planning.
 *
 * @param node Threshold node being planned; parameters do not affect ROI shape.
 * @param roi Downstream ROI requested from the threshold output.
 * @param graph Graph containing the node; unused for pointwise mapping.
 * @return The identical upstream ROI required from the input image.
 * @throws Nothing.
 * @note This explicit contract prevents reliance on legacy identity fallback.
 */
cv::Rect threshold_dirty_roi(const ps::Node& node, const cv::Rect& roi,
                             const ps::GraphModel& graph) {
  (void)node;
  (void)graph;
  return roi;
}

/**
 * @brief Projects an upstream threshold ROI to the same output coordinates.
 *
 * @param node Threshold node being planned; parameters do not affect ROI shape.
 * @param roi Upstream input ROI that changed.
 * @param graph Graph containing the node; unused for pointwise mapping.
 * @param parent_size Upstream image size; unused because the mapping is
 * one-to-one.
 * @param child_size Output image size; unused because the mapping is
 * one-to-one.
 * @return The identical output ROI affected by the upstream change.
 * @throws Nothing.
 * @note ROI clipping remains owned by core graph extent services.
 */
cv::Rect threshold_forward_roi(const ps::Node& node, const cv::Rect& roi,
                               const ps::GraphModel& graph,
                               const cv::Size& parent_size,
                               const cv::Size& child_size) {
  (void)node;
  (void)graph;
  (void)parent_size;
  (void)child_size;
  return roi;
}

}  // namespace

/**
 * @brief Executes a pointwise threshold operation over one input image.
 *
 * @param node Graph node containing optional `thresh`, `maxval`, and `type`
 * runtime parameters.
 * @param inputs Borrowed upstream outputs; the first input must contain a valid
 * image buffer.
 * @return NodeOutput containing the thresholded image buffer.
 * @throws ps::GraphError when the required input image is missing.
 * @throws cv::Exception if OpenCV thresholding or buffer conversion fails.
 * @note The plugin is monolithic HP work. It does not mutate graph state and
 * keeps ROI semantics pointwise.
 */
ps::NodeOutput op_threshold(const ps::Node& node,
                            const std::vector<const ps::NodeOutput*>& inputs) {
  if (inputs.empty() || inputs[0]->image_buffer.width == 0) {
    throw ps::GraphError(ps::GraphErrc::MissingDependency,
                         "Threshold op requires one valid input image.");
  }

  const cv::UMat& u_input = ps::toCvUMat(inputs[0]->image_buffer);

  const auto& P = node.runtime_parameters;
  double thresh = as_double_flexible(P, "thresh", 0.5);
  double maxval = as_double_flexible(P, "maxval", 1.0);
  std::string type_str = as_str(P, "type", "binary");

  int threshold_type = cv::THRESH_BINARY;
  if (type_str == "binary_inv")
    threshold_type = cv::THRESH_BINARY_INV;

  cv::UMat u_output;
  cv::threshold(u_input, u_output, thresh, maxval, threshold_type);

  ps::NodeOutput result;
  result.image_buffer = ps::fromCvUMat(u_output);
  return result;
}

/**
 * @brief Registers the threshold operation and its explicit ROI contracts.
 *
 * @return Nothing.
 * @param registrar Host-provided registration API. The pointer is valid only
 * during this call.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws Exceptions from host registry allocation or callback storage may
 * propagate to the plugin loader.
 * @note The plugin uses the host-provided registrar, not
 * `OpRegistry::instance()`, so dynamic plugins do not depend on a shared
 * registry singleton.
 */
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v1 requires registrar");
  }
  registrar->register_op_hp_monolithic("image_process", "threshold",
                                       op_threshold);
  registrar->register_dirty_propagator(
      "image_process", "threshold", ps::DirtyRoiPropFunc(threshold_dirty_roi));
  registrar->register_forward_propagator(
      "image_process", "threshold",
      ps::ForwardRoiPropFunc(threshold_forward_roi));
}
