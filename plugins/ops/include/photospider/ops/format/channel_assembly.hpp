#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/ops/format/channel.hpp"

namespace ps::format {
/** @brief Static FMT-02 metadata, output-layout and CPU-profile policy.
 *
 * The helpers serialize these options into an invocation-local graph node and
 * perform no sample reads. `output_description` can define result semantics;
 * execution copies source element bits without color or numeric conversion.
 * `auto` retains a view when the complete requested mapping is representable,
 * otherwise it materializes requested samples. `view` requires a legal retained
 * view; `materialize` always copies requested samples. Profiles are `strict`,
 * `accelerated_apple_silicon`, and `accelerated_x86_64`.
 */
struct ChannelAssemblyOptions final {
  std::string metadata_mode = "respect";
  std::map<std::uint32_t, TensorDescription> input_overrides;
  std::optional<TensorDescription> output_description;
  std::string layout = "auto";
  std::string profile = "strict";
};
/** @brief Static source shape for one FMT-02C input.
 *
 * Each input Result carries one tensor. `component=true` describes an input
 * with no channel axis and requires `axis` to be empty. Otherwise `axis`
 * optionally asserts its channel cell axis, excluding the batch prefix; an
 * empty axis resolves from effective metadata, except raw mode requires it.
 */
struct ChannelSourceStructure final {
  bool component = false;
  std::optional<std::uint32_t> axis;
};
/** @brief One static FMT-02C source-to-destination row.
 *
 * `selector` is canonical unsigned decimal for `index`, or an exact UTF-8
 * `name`/`role` value. Every destination in `[0, mapping.size())` occurs once;
 * source selections may repeat. `destination_component` supplies explicit
 * semantic fields for this result slot.
 */
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
/** @brief Append FMT-02A for ordered, equal-shape component Results.
 *
 * Inputs are single-tensor Results. The helper accepts 1 to 1024 input Results.
 * `axis` is the inserted output cell-axis index and excludes the Result batch
 * prefix. The complete sample rank, including batch and cell axes, is at
 * most 8. The helper reads no samples. It returns a connectable `values` edge
 * on success; validation of connected Result descriptors occurs at compilation
 * and execution. On a returned error the document is unchanged. Allocation
 * failures may throw `std::bad_alloc`; callers must serialize document writes.
 */
inline Result<WorkflowNodeOutput> assemble_channels(
    WorkflowDocument& document, std::vector<WorkflowInput> inputs,
    std::uint32_t axis, const ChannelAssemblyOptions& options = {}) {
  return detail::append_assembly(document, "assemble", std::move(inputs),
                                 {{"axis", static_cast<std::int64_t>(axis)}},
                                 options);
}
/** @brief Append FMT-02B for ordered channel-block concatenation.
 *
 * Inputs are single-tensor Results. `axes` is empty to resolve all input cell
 * axes from metadata, or supplies one optional cell-axis assertion per input;
 * null entries resolve from metadata.
 * `output_axis` is a cell-axis index too. All axis values exclude the Result
 * batch prefix. Complete sample rank, including
 * batch and cell axes, is at most 8. The helper reads no samples. Compilation
 * and execution validate dtype, batch prefix, remaining shape and axis
 * assertions. Error, exception and document-mutation guarantees match
 * `assemble_channels`.
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
/** @brief Append FMT-02C with a complete static source-to-slot mapping.
 *
 * `inputs` are Results with one tensor each. `structure` describes each input,
 * and `mapping` covers
 * every output slot exactly once. `output_axis` and source axes index cell
 * axes, excluding the Result batch prefix; complete sample rank is at most 8.
 * The helper serializes canonical records but does not read samples, infer
 * missing source structure, or add broadcast sources. It returns a connectable
 * `values` edge. On a returned error the document is unchanged; allocation may
 * throw `std::bad_alloc` and callers must serialize document writes.
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
