#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "photospider/data/tensor_description.hpp"
#include "photospider/numeric/workflow_authoring.hpp"

namespace ps::format {
/** @brief Same-dtype finite constants for two-level Gray selection. The bit
 * representation, including negative zero, is carried unchanged to the node.
 * The compiler rejects NaN/Inf and dtype mismatch before any sample request. */
using ModelConstant = std::variant<float, double>;
/** @brief Static options for native FMT-11 model conversions.
 *
 * Each helper appends a registered CPU Result operation with one input Result
 * containing one Float32 or Float64 tensor and returns its `values` output.
 * `axis` and `output_axis` index cell axes; the Result batch prefix is
 * excluded. Defaults are respect/auto/auto/strict; hue units, raw
 * selectors/whites, the forward NCL matrix, threshold and two-level constants
 * are never inferred. Explicit unit strings are radian or pi_multiple. NCL
 * presets are bt601, bt709 and bt2020_ncl, independent of the underlying RGB
 * basis/transfer. Reference bypasses numerical fast filters; scalar suppresses
 * batch SIMD; auto enables profile-admitted SIMD and independently certified
 * fast filters.
 */
struct ModelConversionOptions final {
  std::string metadata_mode = "respect", layout = "auto", algorithm = "auto",
              profile = "strict";
  std::optional<TensorDescription> metadata_override;
  std::optional<std::uint32_t> axis, output_axis;
  std::vector<std::uint64_t> components;
  bool axis_free = false;
  std::optional<std::string> group, input_hue_unit, output_hue_unit, gray_kind,
      ncl_matrix;
  std::optional<std::array<double, 2>> white, gray_white, ncl_coefficients;
  std::optional<double> threshold;
  std::optional<ModelConstant> black_value, white_value;
};
namespace model_detail {
inline std::string constant_parameter(const ModelConstant& constant) {
  const bool narrow = std::holds_alternative<float>(constant);
  std::uint64_t bits = 0;
  if (narrow) {
    const auto value = std::get<float>(constant);
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    bits = raw;
  } else {
    const auto value = std::get<double>(constant);
    std::memcpy(&bits, &value, 8);
  }
  std::string encoded = narrow ? "f32:" : "f64:";
  for (unsigned i = narrow ? 8u : 16u; i-- > 0;)
    encoded.push_back("0123456789abcdef"[(bits >> (4 * i)) & 15]);
  return encoded;
}
inline Result<WorkflowNodeOutput> append(
    WorkflowDocument& document, WorkflowInput input, const char* key,
    const ModelConversionOptions& options) {
  using Answer = Result<WorkflowNodeOutput>;
  if (options.profile != "strict" &&
      options.profile != "accelerated_apple_silicon" &&
      options.profile != "accelerated_x86_64")
    return Answer(Status{ErrorCode::InvalidArgument,
                         "unknown FMT-11 profile",
                         FailureReason::InvalidDomain,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  std::map<std::string, ParameterValue> params{
      {"metadata_mode", options.metadata_mode},
      {"layout", options.layout},
      {"algorithm", options.algorithm}};
  if (options.metadata_override) {
    auto encoded = tensor_description_parameter(*options.metadata_override);
    if (!encoded.ok())
      return Answer(encoded.status());
    params["metadata_override"] = encoded.take_value();
  }
  if (options.axis)
    params["axis"] = static_cast<std::int64_t>(*options.axis);
  if (options.output_axis)
    params["output_axis"] = static_cast<std::int64_t>(*options.output_axis);
  if (options.axis_free)
    params["axis_free"] = true;
  if (!options.components.empty()) {
    std::string encoded;
    for (const auto c : options.components)
      encoded += (encoded.empty() ? "" : ",") + std::to_string(c);
    params["components"] = std::move(encoded);
  }
  const auto text = [&](const char* name,
                        const std::optional<std::string>& value) {
    if (value)
      params[name] = *value;
  };
  text("group", options.group);
  text("input_hue_unit", options.input_hue_unit);
  text("output_hue_unit", options.output_hue_unit);
  text("gray_kind", options.gray_kind);
  text("ncl_matrix", options.ncl_matrix);
  const auto pair = [&](const char* a, const char* b,
                        const std::optional<std::array<double, 2>>& value) {
    if (value) {
      params[a] = (*value)[0];
      params[b] = (*value)[1];
    }
  };
  pair("white_x", "white_y", options.white);
  pair("gray_white_x", "gray_white_y", options.gray_white);
  pair("kr", "kb", options.ncl_coefficients);
  if (options.threshold)
    params["threshold"] = *options.threshold;
  if (options.black_value)
    params["black_value"] = constant_parameter(*options.black_value);
  if (options.white_value)
    params["white_value"] = constant_parameter(*options.white_value);
  auto ids = numeric::available_workflow_node_ids(document, 1, {input});
  if (!ids.ok())
    return Answer(ids.status());
  const auto id = ids.value()[0];
  WorkflowNode node{id,
                    std::string(key) + "_" + options.profile,
                    {std::move(input)},
                    std::move(params)};
  document.nodes.push_back(std::move(node));
  return Answer(WorkflowNodeOutput{id, "values"});
}
}  // namespace model_detail

// Public helpers append their explicit registered operation keys. They do not
// insert implicit adaptation, transfer conversion or dtype conversion.
#define PHOTOSPIDER_MODEL_HELPER(name, key)                                \
  inline Result<WorkflowNodeOutput> name(                                  \
      WorkflowDocument& document, WorkflowInput input,                     \
      const ModelConversionOptions& options = {}) {                        \
    return model_detail::append(document, std::move(input), key, options); \
  }
PHOTOSPIDER_MODEL_HELPER(xyz_to_cielab, "color.xyz_to_cielab")
PHOTOSPIDER_MODEL_HELPER(cielab_to_xyz, "color.cielab_to_xyz")
PHOTOSPIDER_MODEL_HELPER(cielab_to_cielch, "color.cielab_to_cielch")
PHOTOSPIDER_MODEL_HELPER(cielch_to_cielab, "color.cielch_to_cielab")
PHOTOSPIDER_MODEL_HELPER(xyz_to_oklab, "color.xyz_to_oklab")
PHOTOSPIDER_MODEL_HELPER(oklab_to_xyz, "color.oklab_to_xyz")
PHOTOSPIDER_MODEL_HELPER(oklab_to_oklch, "color.oklab_to_oklch")
PHOTOSPIDER_MODEL_HELPER(oklch_to_oklab, "color.oklch_to_oklab")
PHOTOSPIDER_MODEL_HELPER(rgb_to_hsl, "color.rgb_to_hsl")
PHOTOSPIDER_MODEL_HELPER(hsl_to_rgb, "color.hsl_to_rgb")
PHOTOSPIDER_MODEL_HELPER(rgb_to_hsv, "color.rgb_to_hsv")
PHOTOSPIDER_MODEL_HELPER(hsv_to_rgb, "color.hsv_to_rgb")
PHOTOSPIDER_MODEL_HELPER(rgb_to_ycbcr_ncl, "color.rgb_to_ycbcr_ncl")
PHOTOSPIDER_MODEL_HELPER(ycbcr_ncl_to_rgb, "color.ycbcr_ncl_to_rgb")
PHOTOSPIDER_MODEL_HELPER(xyz_to_xyy, "color.xyz_to_xyy")
PHOTOSPIDER_MODEL_HELPER(xyy_to_xyz, "color.xyy_to_xyz")
PHOTOSPIDER_MODEL_HELPER(color_to_gray, "color.color_to_gray")
PHOTOSPIDER_MODEL_HELPER(gray_to_color, "color.gray_to_color")
/** @brief Append the registered `mask.threshold_channel_<profile>` operation.
 *
 * This helper serializes the FMT-11S parameters and appends one MASK operation;
 * it does not register a native color-model key. The compiler validates the
 * resulting graph. Bypass channels retain the FMT-11 contract. */
PHOTOSPIDER_MODEL_HELPER(gray_to_black_white, "mask.threshold_channel")
PHOTOSPIDER_MODEL_HELPER(black_white_to_gray, "color.black_white_to_gray")
#undef PHOTOSPIDER_MODEL_HELPER
}  // namespace ps::format
