#include "core/ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/param_utils.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::ops {
namespace {

/**
 * @brief Reads one exact public numeric parameter as a bounded host integer.
 *
 * @param parameters Effective request-local parameter snapshot.
 * @param key Parameter name to inspect.
 * @return Integral value when the Int64 or Double alternative is exactly
 *         integral and fits `int`; otherwise nullopt.
 * @throws Nothing.
 * @note Numeric alternatives are handled explicitly because public typed
 *       accessors reject cross-alternative reads.
 */
std::optional<int> parameter_map_int(const plugin::ParameterMap& parameters,
                                     const char* key) noexcept {
  const auto found = parameters.find(key);
  if (found == parameters.end()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  if (found->second.is_int64()) {
    value = found->second.as_int64();
  } else if (found->second.is_double()) {
    const double real = found->second.as_double();
    if (!std::isfinite(real) || std::trunc(real) != real ||
        real < static_cast<double>(std::numeric_limits<int>::min()) ||
        real > static_cast<double>(std::numeric_limits<int>::max())) {
      return std::nullopt;
    }
    value = static_cast<std::int64_t>(real);
  } else {
    return std::nullopt;
  }
  if (value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

/**
 * @brief Derives the maximum declared radius from exact public parameters.
 *
 * @param parameters Effective request-local parameter snapshot.
 * @param radius_keys Parameters already expressed as radii.
 * @param size_keys Parameters expressed as kernel sizes.
 * @param fallback Initial radius when no usable value exists.
 * @return Non-negative maximum radius.
 * @throws Nothing.
 * @note Invalid alternatives are ignored consistently with provider execution
 *       defaults; size values use symmetric `(size - 1) / 2` geometry.
 */
int infer_radius_from_params(const plugin::ParameterMap& parameters,
                             std::initializer_list<const char*> radius_keys,
                             std::initializer_list<const char*> size_keys,
                             int fallback = 0) noexcept {
  int radius = std::max(0, fallback);
  const auto try_update = [&](std::optional<int> candidate) {
    if (candidate.has_value()) {
      radius = std::max(radius, std::max(0, *candidate));
    }
  };
  for (const char* key : radius_keys) {
    try_update(parameter_map_int(parameters, key));
  }
  for (const char* key : size_keys) {
    const std::optional<int> value = parameter_map_int(parameters, key);
    if (value.has_value()) {
      const int computed =
          *value > 0
              ? static_cast<int>((static_cast<std::int64_t>(*value) - 1) / 2)
              : 0;
      radius = std::max(radius, computed);
    }
  }
  return radius;
}

/**
 * @brief Preserves downstream dirty demand for dependency-neutral operations.
 *
 * @param node Unused operation node.
 * @param downstream_roi Dirty output ROI.
 * @param graph Unused graph context.
 * @param output_extent Unused output extent.
 * @param input_extents Unused input extents.
 * @param parameters Unused effective parameter snapshot.
 * @param inputs Unused resolved input snapshots.
 * @return Unchanged dirty ROI.
 * @throws Nothing.
 * @note Graph planning remains responsible for extent clipping.
 */
PixelRect identity_dirty_roi(
    const Node& node, const PixelRect& downstream_roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* inputs) noexcept {
  (void)node;
  (void)graph;
  (void)output_extent;
  (void)input_extents;
  (void)parameters;
  (void)inputs;
  return downstream_roi;
}

/**
 * @brief Preserves upstream change geometry for dependency-neutral operations.
 *
 * @param node Unused operation node.
 * @param upstream_roi Changed input ROI.
 * @param graph Unused graph context.
 * @param parent_extent Unused source extent.
 * @param child_extent Unused destination extent.
 * @param input_index Unused active input index.
 * @param input_extents Unused input extents.
 * @param parameters Unused effective parameter snapshot.
 * @return Unchanged upstream ROI.
 * @throws Nothing.
 * @note Graph propagation remains responsible for extent clipping.
 */
PixelRect identity_forward_roi(
    const Node& node, const PixelRect& upstream_roi, const GraphModel& graph,
    const PixelSize& parent_extent, const PixelSize& child_extent,
    std::size_t input_index, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters) noexcept {
  (void)node;
  (void)graph;
  (void)parent_extent;
  (void)child_extent;
  (void)input_index;
  (void)input_extents;
  (void)parameters;
  return upstream_roi;
}

/**
 * @brief Reports the dimensions of one resolved image input.
 *
 * @param node Unused analyzer node.
 * @param inputs One required image input.
 * @return Named `width` and `height` integer values.
 * @throws GraphError with `GraphErrc::MissingDependency` for an absent or empty
 *         input.
 * @throws std::bad_alloc if named-output storage allocation fails.
 * @note The callback reads only the dependency-neutral ImageBuffer descriptor.
 */
NodeOutput op_get_dimensions(const Node& node,
                             const std::vector<const NodeOutput*>& inputs) {
  (void)node;
  if (inputs.empty() || inputs[0] == nullptr) {
    throw GraphError(GraphErrc::MissingDependency,
                     "analyzer:get_dimensions requires an image input.");
  }
  const ImageBuffer& input_buffer = inputs[0]->image_buffer;
  if (input_buffer.width == 0 || input_buffer.height == 0) {
    throw GraphError(GraphErrc::MissingDependency,
                     "analyzer:get_dimensions input image is empty.");
  }

  NodeOutput output;
  output.data["width"] = input_buffer.width;
  output.data["height"] = input_buffer.height;
  return output;
}

/**
 * @brief Divides two request-effective scalar parameters.
 *
 * @param node Node carrying numeric `operand1` and `operand2` values.
 * @param inputs Unused operation inputs.
 * @return Named floating-point `result`.
 * @throws GraphError with `GraphErrc::InvalidParameter` for missing,
 *         non-numeric, or zero-divisor parameters.
 * @throws std::bad_alloc if named-output storage allocation fails.
 * @note Runtime parameters are the compute service's resolved effective
 *       snapshot for this callback.
 */
NodeOutput op_divide(const Node& node,
                     const std::vector<const NodeOutput*>& inputs) {
  (void)inputs;
  const plugin::ParameterMap& parameters = node.runtime_parameters;
  const plugin::ParameterValue* operand1 =
      find_parameter(parameters, "operand1");
  const plugin::ParameterValue* operand2 =
      find_parameter(parameters, "operand2");
  const std::optional<double> left =
      operand1 == nullptr ? std::nullopt : parameter_value_as_double(*operand1);
  const std::optional<double> right =
      operand2 == nullptr ? std::nullopt : parameter_value_as_double(*operand2);
  if (!left || !right) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "math:divide requires numeric 'operand1' and 'operand2'.");
  }
  if (*right == 0.0) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "math:divide attempted to divide by zero.");
  }

  NodeOutput output;
  output.data["result"] = *left / *right;
  return output;
}

}  // namespace

/** @copydoc ps::ops::builtin_input_halo_radius */
int builtin_input_halo_radius(const std::string& type,
                              const std::string& subtype,
                              const plugin::ParameterMap& parameters) noexcept {
  if (type != "image_process") {
    return 0;
  }
  if (subtype == "gaussian_blur") {
    int kernel_size = parameter_map_int(parameters, "ksize").value_or(3);
    if (kernel_size <= 0) {
      return 0;
    }
    if (kernel_size % 2 == 0) {
      ++kernel_size;
    }
    return kernel_size / 2;
  }
  if (subtype == "convolve") {
    return infer_radius_from_params(parameters, {"kernel_radius", "radius"},
                                    {"ksize", "kernel_size"}, 1);
  }
  return 0;
}

/** @copydoc ps::ops::register_core_operations */
void register_core_operations() {
  OpRegistry& registry = OpRegistry::instance();
  registry.register_op_hp_monolithic("analyzer", "get_dimensions",
                                     MonolithicOpFunc(op_get_dimensions));
  registry.register_op_hp_monolithic("math", "divide",
                                     MonolithicOpFunc(op_divide));

  const DirtyRoiPropFunc dirty(identity_dirty_roi);
  const ForwardRoiPropFunc forward(identity_forward_roi);
  registry.register_dirty_propagator("analyzer", "get_dimensions", dirty);
  registry.register_dirty_propagator("math", "divide", dirty);
  registry.register_forward_propagator("analyzer", "get_dimensions", forward);
  registry.register_forward_propagator("math", "divide", forward);
}

}  // namespace ps::ops
