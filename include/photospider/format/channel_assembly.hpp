#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/format/channel.hpp"

namespace ps::format {
/** @brief Invocation-local FMT-02 metadata and layout policy. Helpers serialize
 * defaults explicitly. Output interpretation relabels bytes without conversion.
 * Profiles: strict, accelerated_apple_silicon, accelerated_x86_64.
 */
struct ChannelAssemblyOptions final {
  std::string metadata_mode = "respect";
  std::map<std::uint32_t, TensorDescription> input_overrides;
  std::optional<TensorDescription> output_description;
  std::string layout = "auto";
  std::string profile = "strict";
};
/** @brief C source structure; component has no axis; channels resolves an axis
 * from metadata or the explicit assertion. Raw requires an explicit axis. */
struct ChannelSourceStructure final {
  bool component = false;
  std::optional<std::uint32_t> axis;
};
/** @brief One C row; selector is index decimal text, or exact UTF-8 name/role.
 * Destinations must cover [0,row_count) once. Sources may repeat. */
struct ChannelMapping final {
  std::uint32_t input = 0;
  std::string match = "index";
  std::string selector = "0";
  std::uint64_t destination = 0;
  std::optional<TensorChannelDescription> destination_component;
};
namespace detail {
inline std::string assembly_hex(const std::string& text) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : text) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}
inline Result<WorkflowNodeOutput> append_assembly(
    WorkflowDocument& document, const std::string& member,
    std::vector<WorkflowInput> inputs,
    std::map<std::string, ParameterValue> parameters,
    const ChannelAssemblyOptions& options) {
  using Answer = Result<WorkflowNodeOutput>;
  if (inputs.empty() || inputs.size() > 1024 ||
      (options.profile != "strict" &&
       options.profile != "accelerated_apple_silicon" &&
       options.profile != "accelerated_x86_64"))
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid assembly arity/profile"});
  parameters["metadata_mode"] = options.metadata_mode;
  parameters["layout"] = options.layout;
  if (!options.input_overrides.empty()) {
    std::string encoded = "v1";
    for (const auto& entry : options.input_overrides) {
      auto value = tensor_description_parameter(entry.second);
      if (!value.ok())
        return Answer(value.status());
      encoded += ";" + std::to_string(entry.first) + ":" + value.take_value();
    }
    parameters["input_overrides"] = std::move(encoded);
  }
  if (options.output_description) {
    auto value = tensor_description_parameter(*options.output_description);
    if (!value.ok())
      return Answer(value.status());
    parameters["output_description"] = value.take_value();
  }
  auto ids = numeric::available_workflow_node_ids(document, 1, inputs);
  if (!ids.ok())
    return Answer(ids.status());
  const auto id = ids.value()[0];
  WorkflowNode node{id, "channel." + member + "_" + options.profile,
                    std::move(inputs), std::move(parameters)};
  document.nodes.push_back(std::move(node));
  return Answer(WorkflowNodeOutput{id, "values"});
}
}  // namespace detail
/** @brief FMT-02A: insert axis into equal component shapes. Does no payload
 * I/O. On failure the document is unchanged; allocation may throw bad_alloc.
 * Returned edge is independently connectable; compile validates all metadata.
 */
inline Result<WorkflowNodeOutput> assemble_channels(
    WorkflowDocument& document, std::vector<WorkflowInput> inputs,
    std::uint32_t axis, const ChannelAssemblyOptions& options = {}) {
  return detail::append_assembly(document, "assemble", std::move(inputs),
                                 {{"axis", static_cast<std::int64_t>(axis)}},
                                 options);
}
/** @brief FMT-02B: concatenate channel blocks; null axes resolve from metadata.
 * An empty axes vector omits all assertions. Compile rejects mismatched shapes,
 * dtype or axes. Same exception/ownership contract as assemble_channels.
 */
inline Result<WorkflowNodeOutput> concatenate_channels(
    WorkflowDocument& document, std::vector<WorkflowInput> inputs,
    std::uint32_t output_axis,
    const std::vector<std::optional<std::uint32_t>>& axes = {},
    const ChannelAssemblyOptions& options = {}) {
  std::map<std::string, ParameterValue> parameters{
      {"output_axis", static_cast<std::int64_t>(output_axis)}};
  if (!axes.empty()) {
    std::string text = "v1";
    for (auto axis : axes)
      text += ";" + (axis ? std::to_string(*axis) : "_");
    parameters["input_axes"] = std::move(text);
  }
  return detail::append_assembly(document, "concatenate", std::move(inputs),
                                 std::move(parameters), options);
}
/** @brief FMT-02C: copy explicitly selected components into destination slots.
 * Structure and mappings are static, canonical v1 String records. Helpers do
 * not infer structure or expand constant/broadcast inputs. No sample I/O.
 */
inline Result<WorkflowNodeOutput> assemble_mapped_channels(
    WorkflowDocument& document, std::vector<WorkflowInput> inputs,
    std::uint32_t output_axis,
    const std::vector<ChannelSourceStructure>& structure,
    const std::vector<ChannelMapping>& mapping,
    const ChannelAssemblyOptions& options = {}) {
  using Answer = Result<WorkflowNodeOutput>;
  std::string sources = "v1", rows = "v1";
  for (const auto& s : structure) {
    if (s.component && s.axis)
      return Answer(
          Status{ErrorCode::InvalidArgument, "component axis is forbidden"});
    sources +=
        s.component ? ";c" : ";h" + (s.axis ? std::to_string(*s.axis) : "_");
  }
  for (const auto& row : mapping) {
    std::string destination = "_";
    if (row.destination_component) {
      TensorDescription description;
      description.component = row.destination_component;
      auto encoded = tensor_description_parameter(description);
      if (!encoded.ok())
        return Answer(encoded.status());
      destination = encoded.take_value();
    }
    rows += ";" + std::to_string(row.input) + "," + row.match + "," +
            detail::assembly_hex(row.selector) + "," +
            std::to_string(row.destination) + "," + destination;
  }
  return detail::append_assembly(
      document, "assemble_mapped", std::move(inputs),
      {{"output_axis", static_cast<std::int64_t>(output_axis)},
       {"input_structure", sources},
       {"mapping", rows}},
      options);
}
}  // namespace ps::format
