#include <new>
#include <string>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "plugin_api.hpp"  // NOLINT(build/include_subdir)

namespace {

#if defined(PHOTOSPIDER_THRESHOLD_BAD_ALLOC_TESTING)
/**
 * @brief Injects deterministic resource exhaustion at a parameter conversion.
 *
 * @param parameter Real YAML scalar selected by the threshold callback.
 * @param probe_tag Test-only YAML tag that arms this exact conversion point.
 * @return Nothing.
 * @throws std::bad_alloc when parameter carries probe_tag.
 * @note This helper is compiled only into test_bad_alloc_boundaries. It has
 * internal linkage, adds no plugin ABI, and runs inside the real registered
 * operation callback immediately before yaml-cpp conversion.
 */
void throw_if_threshold_parameter_probe(const YAML::Node& parameter,
                                        const char* probe_tag) {
  if (parameter.Tag() == probe_tag) {
    throw std::bad_alloc{};
  }
}
#endif

/**
 * @brief Reads a numeric YAML parameter with a fallback value.
 *
 * @param parameters Parameter map to read.
 * @param key Parameter key.
 * @param fallback Fallback value used for missing, non-scalar, or invalid
 * input.
 * @return Parsed double value or fallback.
 * @throws std::bad_alloc if YAML lookup or conversion exhausts memory.
 * @note The plugin keeps this helper local so invalid optional parameters do
 * not fail loading or planning.
 */
double threshold_parameter_as_double(const YAML::Node& parameters,
                                     const std::string& key, double fallback) {
  try {
    if (!parameters) {
      return fallback;
    }
    const YAML::Node parameter = parameters[key];
    if (!parameter) {
      return fallback;
    }
#if defined(PHOTOSPIDER_THRESHOLD_BAD_ALLOC_TESTING)
    throw_if_threshold_parameter_probe(parameter,
                                       "!photospider-test-numeric-bad-alloc");
#endif
    if (parameter.IsScalar()) {
      return parameter.as<double>();
    }
    return fallback;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return fallback;
  }
}

/**
 * @brief Reads a string YAML parameter with a fallback value.
 *
 * @param parameters Parameter map to read.
 * @param key Parameter key.
 * @param fallback Fallback value used for missing or invalid input.
 * @return Parsed string value or fallback.
 * @throws std::bad_alloc if YAML lookup, conversion, or result storage exhausts
 * memory.
 * @note Threshold mode parsing is intentionally permissive for plugin examples.
 */
std::string threshold_parameter_as_string(const YAML::Node& parameters,
                                          const std::string& key,
                                          const std::string& fallback) {
  try {
    if (!parameters) {
      return fallback;
    }
    const YAML::Node parameter = parameters[key];
    if (!parameter) {
      return fallback;
    }
#if defined(PHOTOSPIDER_THRESHOLD_BAD_ALLOC_TESTING)
    throw_if_threshold_parameter_probe(parameter,
                                       "!photospider-test-string-bad-alloc");
#endif
    return parameter.as<std::string>();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return fallback;
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

/**
 * @brief Executes a pointwise threshold operation over one input image.
 *
 * @param node Graph node containing optional `thresh`, `maxval`, and `type`
 * runtime parameters.
 * @param inputs Borrowed upstream outputs; the first input must contain a valid
 * image buffer.
 * @return NodeOutput containing the thresholded image buffer.
 * @throws std::bad_alloc if parameter parsing, OpenCV, or output allocation
 * exhausts memory.
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

  const auto& P = node.runtime_parameters;
  double thresh = threshold_parameter_as_double(P, "thresh", 0.5);
  double maxval = threshold_parameter_as_double(P, "maxval", 1.0);
  std::string type_str = threshold_parameter_as_string(P, "type", "binary");

  const cv::UMat& u_input = ps::toCvUMat(inputs[0]->image_buffer);

  int threshold_type = cv::THRESH_BINARY;
  if (type_str == "binary_inv")
    threshold_type = cv::THRESH_BINARY_INV;

  cv::UMat u_output;
  cv::threshold(u_input, u_output, thresh, maxval, threshold_type);

  ps::NodeOutput result;
  result.image_buffer = ps::fromCvUMat(u_output);
  return result;
}

}  // namespace

/**
 * @brief Registers the threshold operation and its explicit ROI contracts.
 *
 * @return Nothing.
 * @param registrar Host-provided registration API. The pointer is valid only
 * during this call.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::bad_alloc if operation names or callback storage exhausts
 * memory.
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
