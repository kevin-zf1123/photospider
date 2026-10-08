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
/** @brief Build a bounded digest assertion for a resolved Result schema.
 *
 * The caller supplies an already-resolved and validated `SchemaTemplate`.
 * This function hashes its complete canonical representation, including
 * metadata and batch axes, with domain-separated SHA-256 and returns the
 * lowercase 64-character digest. It reads no sample data and makes no
 * validity claim. Physical layout is asserted separately. The returned string
 * belongs to the caller. Canonicalization and string allocation may throw;
 * calls are thread-safe.
 */
PHOTOSPIDER_API std::string schema_assertion(const SchemaTemplate& schema);
inline std::string layout_assertion(const ResultTensorLayout& layout) {
  if (!layout.spatial)
    return "none";
  std::string result =
      std::to_string(static_cast<unsigned>(layout.order)) + ":" +
      std::to_string(layout.height_axis) + ":" +
      std::to_string(layout.width_axis) + ":" +
      std::to_string(layout.channel_axis
                         ? static_cast<std::int64_t>(*layout.channel_axis)
                         : -1) +
      ":" + std::to_string(layout.row_pitch_bytes) + ":" +
      std::to_string(layout.groups.size());
  for (const auto& group : layout.groups)
    result += ":" + std::to_string(group.role.size()) + ":" + group.role + ":" +
              std::to_string(group.first_channel) + ":" +
              std::to_string(group.channel_count);
  return result;
}
}  // namespace detail

/** @brief Static options shared by FMT-01A/B and the split helper.
 *
 * `metadata_mode` is `respect`, `raw` or `override`; override requires a typed
 * `metadata_override` used only by the generated nodes. `axis`, when present,
 * indexes the tensor descriptor's cell axes and excludes the Result batch
 * prefix. Without it, respect/override can use a described channel axis; raw
 * always requires it. `keepdims` controls whether extraction removes the cell
 * axis or leaves it with extent one. `layout` is `auto`, `view` or
 * `materialize`. `profile` selects one of the three CPU registrations. The
 * helpers serialize every default into generated nodes; direct nodes must
 * specify their required values explicitly.
 */
struct ChannelExtractOptions final {
  std::string metadata_mode = "respect";
  std::optional<std::uint32_t> axis;
  std::optional<TensorDescription> metadata_override;
  bool keepdims = false;
  std::string layout = "auto";
  std::string profile = "strict";
};

/** @brief One separately connectable FMT-01A Result output reference.
 *
 * `name` is the stable zero-based handle name `cN`. The output edge is not
 * automatically added to the workflow's exported roots; the caller chooses
 * which handles to connect or publish.
 */
struct ChannelHandle final {
  std::string name;
  WorkflowNodeOutput output;
};

/** @brief Expand FMT-01C into one FMT-01A Result node per channel.
 *
 * `source_metadata` is the inferred metadata for `input` and must describe a
 * Result schema with one tensor member and no fields. For a declared workflow
 * input, the helper checks the declared schema and physical layout. It stages
 * every node and handle before appending; generated A nodes carry canonical
 * schema/layout assertions for compile-time checks, including forward producer
 * references. The helper reads no samples and does not execute the workflow.
 *
 * The channel axis is resolved on the tensor descriptor; Result batch axes
 * remain outside it. The helper returns handles `c0` through `c(C-1)` for the
 * generated nodes' `values` outputs. The caller decides which handles to
 * connect or export. Expansion is limited to 65,536 channels and also obeys
 * workflow node/output limits. Calls mutating the same document must be
 * serialized by the caller.
 *
 * @param document Workflow to extend; unchanged if validation or staging fails.
 * @param input Source Result edge.
 * @param source_metadata Inferred schema, descriptor and physical layout.
 * @param options Static extraction policy and CPU profile.
 * @return One handle per channel in index order, or an error such as
 *         `InvalidArgument`, `ResourceExhausted` or the source metadata status.
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
  if (!source_metadata.result_schema ||
      source_metadata.result_schema->tensors.size() != 1 ||
      !source_metadata.result_schema->fields.empty())
    return Answer(invalid("split requires one Result tensor and no fields"));
  const auto& schema = *source_metadata.result_schema;
  auto valid = schema.validate(true);
  if (!valid.ok())
    return Answer(valid);
  const auto& source = schema.tensors[0];
  if (const auto* declaration = std::get_if<WorkflowInputReference>(&input)) {
    bool found = false;
    for (const auto& candidate : document.inputs)
      if (candidate.id == declaration->input_id) {
        found = true;
        if (!candidate.result_schema ||
            !candidate.result_schema->same_schema(schema) ||
            detail::layout_assertion(
                candidate.result_schema->tensors[0].layout) !=
                detail::layout_assertion(source.layout))
          return Answer(
              invalid("split schema/layout disagrees with input declaration"));
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
  if (options.layout != "auto" && options.layout != "view" &&
      options.layout != "materialize")
    return Answer(invalid("unknown extraction layout"));
  std::optional<TensorDescription> description;
  if (options.metadata_override) {
    description = *options.metadata_override;
  } else if (options.metadata_mode != "raw") {
    for (const auto& facet : source.facets)
      if (facet.key == "photospider.tensor-description") {
        auto decoded = decode_tensor_description(facet);
        if (!decoded.ok())
          return Answer(decoded.status());
        description = decoded.take_value();
      }
  }
  if (description) {
    auto status = validate_tensor_description(*description, source.descriptor);
    if (!status.ok())
      return Answer(status);
  }
  std::optional<std::uint32_t> axis = options.axis;
  if (axis && *axis >= source.descriptor.shape.size())
    return Answer(invalid("split axis is outside tensor rank"));
  if (options.metadata_mode != "raw" && description &&
      description->channel_axis) {
    if (axis && *axis != *description->channel_axis)
      return Answer(invalid("split axis disagrees with description"));
    axis = description->channel_axis;
  }
  if (!axis)
    return Answer(invalid("split requires an explicit or described axis"));
  if (!options.keepdims && source.descriptor.shape.size() == 1)
    return Answer(invalid("rank-one split requires keepdims=true"));
  const auto count = source.descriptor.shape[*axis];
  if (count == 0 || count > 65536 || count > INT64_MAX)
    return Answer(invalid("split exceeds graph expansion capacity"));
  auto ids = numeric::available_workflow_node_ids(
      document, static_cast<unsigned>(count), {input});
  if (!ids.ok())
    return Answer(ids.status());
  const auto layout_assertion = detail::layout_assertion(source.layout);
  if (layout_assertion.size() > 8192)
    return Answer(invalid("split image layout assertion is too large"));
  std::map<std::string, ParameterValue> common{
      {"axis", static_cast<std::int64_t>(*axis)},
      {"expected_channels", static_cast<std::int64_t>(count)},
      {"expected_source_schema", detail::schema_assertion(schema)},
      {"expected_source_layout", layout_assertion},
      {"keepdims", options.keepdims},
      {"layout", options.layout},
      {"metadata_mode", options.metadata_mode}};
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
