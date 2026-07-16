#include <new>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>

#include "photospider/core/graph_error.hpp"
#include "photospider/plugin/opencv_adapter.hpp"
#include "photospider/plugin/plugin_api.hpp"

namespace {

#if defined(PHOTOSPIDER_THRESHOLD_BAD_ALLOC_TESTING)
/**
 * @brief Injects deterministic resource exhaustion at a parameter conversion.
 *
 * @param parameter Public scalar selected by the threshold callback.
 * @param probe_value Test-only string value that arms this conversion point.
 * @return Nothing.
 * @throws std::bad_alloc when parameter carries probe_tag.
 * @note This helper is compiled only into test_bad_alloc_boundaries. It has
 * internal linkage, adds no plugin ABI, and runs inside the real registered
 * operation callback immediately before public value conversion.
 */
void throw_if_threshold_parameter_probe(
    const ps::plugin::ParameterValue& parameter, const char* probe_value) {
  if (parameter.is_string() && parameter.as_string() == probe_value) {
    throw std::bad_alloc{};
  }
}
#endif

/**
 * @brief Reads a numeric public parameter with a fallback value.
 *
 * @param parameters Parameter map to read.
 * @param key Parameter key.
 * @param fallback Fallback value used for missing, non-scalar, or invalid
 * input.
 * @return Parsed double value or fallback.
 * @throws std::bad_alloc if parameter lookup or conversion exhausts memory.
 * @note The plugin keeps this helper local so invalid optional parameters do
 * not fail loading or planning.
 */
double threshold_parameter_as_double(const ps::plugin::ParameterMap& parameters,
                                     const std::string& key, double fallback) {
  try {
    const auto found = parameters.find(key);
    if (found == parameters.end()) {
      return fallback;
    }
    const ps::plugin::ParameterValue& parameter = found->second;
#if defined(PHOTOSPIDER_THRESHOLD_BAD_ALLOC_TESTING)
    throw_if_threshold_parameter_probe(parameter,
                                       "photospider-test-numeric-bad-alloc");
#endif
    if (parameter.is_double()) {
      return parameter.as_double();
    }
    if (parameter.is_int64()) {
      return static_cast<double>(parameter.as_int64());
    }
    return fallback;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    return fallback;
  }
}

/**
 * @brief Reads a string public parameter with a fallback value.
 *
 * @param parameters Parameter map to read.
 * @param key Parameter key.
 * @param fallback Fallback value used for missing or invalid input.
 * @return Parsed string value or fallback.
 * @throws std::bad_alloc if lookup, conversion, or result storage exhausts
 * memory.
 * @note Threshold mode parsing is intentionally permissive for plugin examples.
 */
std::string threshold_parameter_as_string(
    const ps::plugin::ParameterMap& parameters, const std::string& key,
    const std::string& fallback) {
  try {
    const auto found = parameters.find(key);
    if (found == parameters.end()) {
      return fallback;
    }
    const ps::plugin::ParameterValue& parameter = found->second;
#if defined(PHOTOSPIDER_THRESHOLD_BAD_ALLOC_TESTING)
    throw_if_threshold_parameter_probe(parameter,
                                       "photospider-test-string-bad-alloc");
#endif
    return parameter.as_string();
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
 * @param context Immutable dirty ROI and topology snapshot.
 * @return The identical upstream ROI required from the input image.
 * @throws Nothing.
 * @note Registering this callback makes the operation's pointwise behavior an
 * explicit part of the public plugin contract.
 */
ps::PixelRect threshold_dirty_roi(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

/**
 * @brief Projects an upstream threshold ROI to the same output coordinates.
 *
 * @param context Immutable forward ROI snapshot with the active input edge.
 * @return The identical output ROI affected by the upstream change.
 * @throws Nothing.
 * @note ROI clipping remains owned by core graph extent services.
 */
ps::PixelRect threshold_forward_roi(const ps::plugin::RoiContext& context) {
  return context.requested_roi;
}

/**
 * @brief Executes a pointwise threshold operation over one input image.
 *
 * @param node Public node snapshot containing optional `thresh`, `maxval`, and
 * `type` effective parameters.
 * @param inputs Borrowed public input views; the first input must contain a
 * valid image buffer.
 * @return Public operation output containing the thresholded image buffer.
 * @throws std::bad_alloc if parameter parsing, OpenCV, or output allocation
 * exhausts memory.
 * @throws ps::GraphError when the required input image is missing.
 * @throws cv::Exception if OpenCV thresholding or buffer conversion fails.
 * @note The plugin is monolithic HP work. It uses only callback-local CPU
 * `cv::Mat` values, does not mutate graph or shared provider state, is
 * reentrant across independent inputs, and keeps ROI semantics pointwise.
 */
ps::plugin::OperationOutput op_threshold(
    const ps::plugin::NodeView& node,
    ps::plugin::ArrayView<ps::plugin::OperationInputView> inputs) {
  if (inputs.empty() || !inputs[0].image_buffer ||
      inputs[0].image_buffer->width == 0) {
    throw ps::GraphError(ps::GraphErrc::MissingDependency,
                         "Threshold op requires one valid input image.");
  }

  const auto& parameters = node.parameters();
  double thresh = threshold_parameter_as_double(parameters, "thresh", 0.5);
  double maxval = threshold_parameter_as_double(parameters, "maxval", 1.0);
  std::string type_str =
      threshold_parameter_as_string(parameters, "type", "binary");

  const cv::Mat input = ps::plugin::opencv::to_mat(*inputs[0].image_buffer);

  int threshold_type = cv::THRESH_BINARY;
  if (type_str == "binary_inv") {
    threshold_type = cv::THRESH_BINARY_INV;
  }

  cv::Mat output;
  cv::threshold(input, output, thresh, maxval, threshold_type);

  ps::plugin::OperationOutput result;
  result.image_buffer = ps::plugin::opencv::from_mat(output);
  return result;
}

}  // namespace

/**
 * @brief Registers the threshold operation and its explicit ROI contracts.
 *
 * @param registrar Host-provided registration API. The pointer is valid only
 * during this call.
 * @return Nothing.
 * @throws std::invalid_argument when the loader passes a null registrar.
 * @throws std::logic_error if the host registrar is incomplete.
 * @throws std::bad_alloc if operation names or callback storage exhausts
 * memory.
 * @note The callback stores only public SDK values and receives all registry
 * services through the host-provided registrar.
 */
extern "C" PHOTOSPIDER_OPERATION_PLUGIN_EXPORT void register_photospider_ops_v2(
    ps::plugin::OperationPluginRegistrar* registrar) {
  if (!registrar) {
    throw std::invalid_argument(
        "register_photospider_ops_v2 requires registrar");
  }
  registrar->register_op_hp_monolithic("image_process", "threshold",
                                       op_threshold);
  registrar->register_dirty_propagator("image_process", "threshold",
                                       threshold_dirty_roi);
  registrar->register_forward_propagator("image_process", "threshold",
                                         threshold_forward_roi);
}
