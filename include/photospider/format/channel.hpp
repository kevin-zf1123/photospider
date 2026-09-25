#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/tensor_description.hpp"
#include "photospider/numeric/workflow_authoring.hpp"
#include "photospider/plugin/operation_types.hpp"

namespace ps::format {
namespace detail {
inline std::string layout_assertion(
    const std::optional<PlanarImageLayout>& layout) {
  if (!layout)
    return "none";
  std::string result =
      std::to_string(static_cast<unsigned>(layout->order)) + ":" +
      std::to_string(layout->height_axis) + ":" +
      std::to_string(layout->width_axis) + ":" +
      std::to_string(layout->channel_axis
                         ? static_cast<std::int64_t>(*layout->channel_axis)
                         : -1) +
      ":" + std::to_string(layout->row_pitch_bytes) + ":" +
      std::to_string(layout->groups.size());
  for (const auto& group : layout->groups)
    result += ":" + std::to_string(group.role.size()) + ":" + group.role + ":" +
              std::to_string(group.first_channel) + ":" +
              std::to_string(group.channel_count);
  return result;
}
}  // namespace detail

/** @brief Static authoring parameters shared by FMT-01A/B/C. Helpers write
 * all defaults explicitly; direct WorkflowDocument nodes must do the same. */
struct ChannelExtractOptions final {
  std::string metadata_mode = "respect";
  std::optional<std::uint32_t> axis;
  std::optional<TensorDescription> metadata_override;
  bool keepdims = false;
  std::string layout = "auto";
  std::string profile = "strict";
};

/** @brief One separately connectable FMT-01A result, not an exported root. */
struct ChannelHandle final {
  std::string name;
  WorkflowNodeOutput output;
};

/** @brief Expand FMT-01C into one A node per statically described channel.
 *
 * source_metadata must be the input edge's inferred metadata. For declared
 * inputs this function checks the declaration directly. Generated A nodes
 * assert the channel count again at compile time, including for forward
 * producer references. The document is left unchanged on any failure.
 * Allocation may throw std::bad_alloc; no execution or sample read occurs.
 */
inline Result<std::vector<ChannelHandle>> split_channels(
    WorkflowDocument& document, WorkflowInput input,
    const OperationMetadata& source_metadata,
    const ChannelExtractOptions& options = {}) {
  using Answer = Result<std::vector<ChannelHandle>>;
  const auto invalid = [](const char* message) {
    return Status{ErrorCode::InvalidArgument,
                  message,
                  FailureReason::InvalidDomain,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  if (source_metadata.result_schema ||
      source_metadata.descriptor.shape.empty() ||
      source_metadata.descriptor.shape.size() > 8)
    return Answer(invalid("split requires a tensor descriptor"));
  if (const auto* declaration = std::get_if<WorkflowInputReference>(&input)) {
    bool found = false;
    for (const auto& candidate : document.inputs)
      if (candidate.id == declaration->input_id) {
        found = true;
        if (candidate.descriptor.element_type !=
                source_metadata.descriptor.element_type ||
            candidate.descriptor.shape != source_metadata.descriptor.shape ||
            candidate.facets.size() != source_metadata.facets.size() ||
            candidate.planar_layout.has_value() !=
                source_metadata.planar_layout.has_value())
          return Answer(
              invalid("split descriptor disagrees with input declaration"));
        if (candidate.planar_layout) {
          const auto& actual = *candidate.planar_layout;
          const auto& supplied = *source_metadata.planar_layout;
          if (actual.order != supplied.order ||
              actual.height_axis != supplied.height_axis ||
              actual.width_axis != supplied.width_axis ||
              actual.channel_axis != supplied.channel_axis ||
              actual.row_pitch_bytes != supplied.row_pitch_bytes ||
              actual.groups.size() != supplied.groups.size())
            return Answer(
                invalid("split image layout disagrees with declaration"));
          for (std::size_t i = 0; i < actual.groups.size(); ++i)
            if (actual.groups[i].role != supplied.groups[i].role ||
                actual.groups[i].first_channel !=
                    supplied.groups[i].first_channel ||
                actual.groups[i].channel_count !=
                    supplied.groups[i].channel_count)
              return Answer(
                  invalid("split image groups disagree with declaration"));
        }
        for (std::size_t i = 0; i < candidate.facets.size(); ++i)
          if (candidate.facets[i].key != source_metadata.facets[i].key ||
              candidate.facets[i].version !=
                  source_metadata.facets[i].version ||
              candidate.facets[i].payload != source_metadata.facets[i].payload)
            return Answer(
                invalid("split facets disagree with input declaration"));
      }
    if (!found)
      return Answer(invalid("split input declaration is absent"));
  }
  if (options.profile != "strict" &&
      options.profile != "accelerated_apple_silicon" &&
      options.profile != "accelerated_x86_64")
    return Answer(invalid("unknown extraction profile"));
  if (options.metadata_mode != "respect" && options.metadata_mode != "raw" &&
      options.metadata_mode != "override")
    return Answer(invalid("unknown metadata mode"));
  if ((options.metadata_mode == "override") !=
      options.metadata_override.has_value())
    return Answer(invalid("override payload/mode mismatch"));
  std::optional<TensorDescription> description;
  if (options.metadata_override) {
    description = *options.metadata_override;
  } else if (options.metadata_mode != "raw") {
    for (const auto& facet : source_metadata.facets)
      if (facet.key == "photospider.tensor-description") {
        auto decoded = decode_tensor_description(facet);
        if (!decoded.ok())
          return Answer(decoded.status());
        description = decoded.take_value();
      }
  }
  if (description) {
    auto status =
        validate_tensor_description(*description, source_metadata.descriptor);
    if (!status.ok())
      return Answer(status);
  }
  std::optional<std::uint32_t> axis = options.axis;
  if (axis && *axis >= source_metadata.descriptor.shape.size())
    return Answer(invalid("split axis is outside tensor rank"));
  if (options.metadata_mode != "raw" && description &&
      description->channel_axis) {
    if (axis && *axis != *description->channel_axis)
      return Answer(invalid("split axis disagrees with description"));
    axis = description->channel_axis;
  }
  if (!axis)
    return Answer(invalid("split requires an explicit or described axis"));
  if (!options.keepdims && source_metadata.descriptor.shape.size() == 1)
    return Answer(invalid("rank-one split requires keepdims=true"));
  const auto count = source_metadata.descriptor.shape[*axis];
  if (count == 0 || count > 65536 || count > INT64_MAX)
    return Answer(invalid("split exceeds graph expansion capacity"));
  auto ids = numeric::available_workflow_node_ids(
      document, static_cast<unsigned>(count), {input});
  if (!ids.ok())
    return Answer(ids.status());
  std::map<std::string, ParameterValue> common{
      {"axis", static_cast<std::int64_t>(*axis)},
      {"expected_channels", static_cast<std::int64_t>(count)},
      {"expected_source_dtype",
       static_cast<std::int64_t>(source_metadata.descriptor.element_type)},
      {"keepdims", options.keepdims},
      {"layout", options.layout},
      {"metadata_mode", options.metadata_mode}};
  std::string shape_assertion;
  for (const auto extent : source_metadata.descriptor.shape) {
    if (!shape_assertion.empty())
      shape_assertion.push_back(',');
    shape_assertion += std::to_string(extent);
  }
  common.emplace("expected_source_shape", std::move(shape_assertion));
  std::string tensor_assertion = "none";
  for (const auto& facet : source_metadata.facets)
    if (facet.key == "photospider.tensor-description") {
      auto decoded = decode_tensor_description(facet);
      if (!decoded.ok())
        return Answer(decoded.status());
      auto encoded = tensor_description_parameter(decoded.value());
      if (!encoded.ok())
        return Answer(encoded.status());
      tensor_assertion = encoded.take_value();
    }
  common.emplace("expected_source_tensor", std::move(tensor_assertion));
  auto layout_assertion =
      detail::layout_assertion(source_metadata.planar_layout);
  if (layout_assertion.size() > 8192)
    return Answer(invalid("split image layout assertion is too large"));
  common.emplace("expected_source_layout", std::move(layout_assertion));
  if (options.metadata_override) {
    auto encoded = tensor_description_parameter(*options.metadata_override);
    if (!encoded.ok())
      return Answer(encoded.status());
    common.emplace("metadata_override", encoded.take_value());
  }
  std::vector<WorkflowNode> nodes;
  std::vector<ChannelHandle> handles;
  nodes.reserve(count);
  handles.reserve(count);
  for (std::uint64_t index = 0; index < count; ++index) {
    auto parameters = common;
    parameters.emplace("index", static_cast<std::int64_t>(index));
    nodes.push_back({ids.value()[index],
                     "channel.extract_index_" + options.profile,
                     {input},
                     std::move(parameters)});
    handles.push_back(
        {"c" + std::to_string(index), {ids.value()[index], "values"}});
  }
  WorkflowDocument staged = document;
  staged.nodes.insert(staged.nodes.end(),
                      std::make_move_iterator(nodes.begin()),
                      std::make_move_iterator(nodes.end()));
  document = std::move(staged);
  return Answer(std::move(handles));
}
}  // namespace ps::format
