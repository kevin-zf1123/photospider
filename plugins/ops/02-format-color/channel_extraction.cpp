#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "01-numeric/sequence_profiles.hpp"
#include "02-format-color/result_mapping.hpp"
#include "common/registry.hpp"
#include "photospider/core/content_digest.hpp"
#include "photospider/core/status_helpers.hpp"
#include "photospider/core/utf8_validation.hpp"
#include "photospider/ops/format/channel.hpp"
#include "photospider/ops/tensor_description.hpp"
#include "photospider/plugin/tile_helpers.hpp"

namespace ps::format::detail {
std::string schema_assertion(const SchemaTemplate& schema) {
  const auto canonical = schema.canonical();
  content_internal::Sha256 digest;
  digest.text("photospider.fmt.result-schema.v1");
  digest.text(std::string_view(canonical));
  return digest.finish();
}
}  // namespace ps::format::detail

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
using Answer = Result<std::vector<OperationOutputSpecialization>>;

enum class Layout { Auto, View, Materialize };
struct Selection final {
  std::uint32_t axis = 0;
  std::uint64_t index = 0;
  bool keepdims = false;
  Layout layout = Layout::Auto;
};
struct Preparation final {
  Selection selection;
};
bool supported(ElementType type) {
  switch (type) {
    case ElementType::UInt8:
    case ElementType::UInt16:
    case ElementType::Int8:
    case ElementType::Int16:
    case ElementType::Int64:
    case ElementType::Float32:
    case ElementType::Float64:
      return true;
  }
  return false;
}
Result<OperationPreparation> prepare_extraction(
    const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters, bool named,
    SequenceProfile profile) {
  using Prepared = Result<OperationPreparation>;
  const auto available = numeric_ops::sequence_profile_available(profile);
  if (!available.ok())
    return Prepared(available);
  auto valid = tensor_ops::check_tensor(inputs[0]);
  if (!valid.ok())
    return Prepared(valid);
  const auto& input_schema = *inputs[0].result_schema;
  const auto& input = input_schema.tensors[0];
  const auto& shape = input.descriptor.shape;
  if (!supported(input.descriptor.element_type) || shape.empty() ||
      shape.size() > 8)
    return Prepared(Status{ErrorCode::TypeMismatch,
                           "channel extraction requires a supported tensor"});
  const auto expected_schema = parameters.find("expected_source_schema");
  const auto expected_layout = parameters.find("expected_source_layout");
  if ((expected_schema != parameters.end()) !=
      (expected_layout != parameters.end()))
    return Prepared(core_internal::invalid_schema_domain(
        "source metadata assertions must be complete"));
  if (expected_schema != parameters.end() &&
      (std::get<std::string>(expected_schema->second) !=
           format::detail::schema_assertion(input_schema) ||
       std::get<std::string>(expected_layout->second) !=
           format::detail::layout_assertion(input.layout)))
    return Prepared(core_internal::invalid_schema_domain(
        "source schema assertion disagrees with input"));
  std::optional<TensorDescription> description;
  if (parameters.count("input_description")) {
    auto parsed = tensor_description_from_parameter(
        std::get<std::string>(parameters.at("input_description")));
    if (!parsed.ok())
      return Prepared(parsed.status());
    auto valid = validate_tensor_description(parsed.value(), input.descriptor);
    if (!valid.ok())
      return Prepared(valid);
    description = parsed.take_value();
  }
  if (named && !description)
    return Prepared(core_internal::invalid_schema_domain(
        "named extraction requires input_description"));
  Selection selected;
  const auto axis_it = parameters.find("axis");
  if (axis_it != parameters.end()) {
    const auto axis = std::get<std::int64_t>(axis_it->second);
    if (axis < 0 || static_cast<std::uint64_t>(axis) >= shape.size())
      return Prepared(core_internal::invalid_schema_domain(
          "channel axis is outside tensor rank"));
    selected.axis = static_cast<std::uint32_t>(axis);
    if (description && description->channel_axis &&
        selected.axis != *description->channel_axis)
      return Prepared(core_internal::invalid_schema_domain(
          "axis assertion disagrees with metadata"));
  } else if (description && description->channel_axis) {
    selected.axis = *description->channel_axis;
  } else {
    return Prepared(core_internal::invalid_schema_domain(
        "channel axis must be explicit or described"));
  }
  const auto expected = parameters.find("expected_channels");
  if (expected != parameters.end() &&
      (std::get<std::int64_t>(expected->second) <= 0 ||
       static_cast<std::uint64_t>(std::get<std::int64_t>(expected->second)) !=
           shape[selected.axis]))
    return Prepared(core_internal::invalid_schema_domain(
        "channel count assertion disagrees with input"));
  selected.keepdims = std::get<bool>(parameters.at("keepdims"));
  if (!selected.keepdims && shape.size() == 1)
    return Prepared(core_internal::invalid_schema_domain(
        "rank-one extraction requires keepdims=true"));
  const auto& layout = std::get<std::string>(parameters.at("layout"));
  if (layout == "auto")
    selected.layout = Layout::Auto;
  else if (layout == "view")
    selected.layout = Layout::View;
  else if (layout == "materialize")
    selected.layout = Layout::Materialize;
  else
    return Prepared(core_internal::invalid_schema_domain(
        "layout must be auto, view or materialize"));
  if (named) {
    const auto& match = std::get<std::string>(parameters.at("match"));
    const auto& selector = std::get<std::string>(parameters.at("selector"));
    if (match != "name" && match != "role")
      return Prepared(
          core_internal::invalid_schema_domain("match must be name or role"));
    if (selector.empty() || selector.size() > 128 ||
        !core_internal::valid_utf8_key(selector))
      return Prepared(core_internal::invalid_schema_domain(
          "selector must be strict UTF-8 of at most 128 bytes"));
    if (!description || !description->channel_axis ||
        *description->channel_axis != selected.axis ||
        description->channels.size() != shape[selected.axis])
      return Prepared(core_internal::invalid_schema_domain(
          "named extraction requires a complete channel table"));
    bool found = false;
    for (std::uint64_t index = 0; index < description->channels.size();
         ++index) {
      const auto& entry = description->channels[index];
      const auto& field = match == "name" ? entry.name : entry.role;
      if (field != selector)
        continue;
      if (found)
        return Prepared(core_internal::invalid_schema_domain(
            "named channel selector is ambiguous"));
      selected.index = index;
      found = true;
    }
    if (!found)
      return Prepared(core_internal::invalid_schema_domain(
          "named channel selector is absent"));
  } else {
    const auto index = std::get<std::int64_t>(parameters.at("index"));
    if (index < 0 || static_cast<std::uint64_t>(index) >= shape[selected.axis])
      return Prepared(core_internal::invalid_schema_domain(
          "channel index is outside selected axis"));
    selected.index = static_cast<std::uint64_t>(index);
  }
  if (parameters.count("expected_inputs") &&
      format_result::text(parameters, "expected_inputs") !=
          format_result::source_assertion(inputs))
    return Prepared(core_internal::invalid_schema_domain(
        "FMT-05B source metadata disagrees with inference"));
  OperationOutputSpecialization output;
  auto schema = input_schema;
  auto& output_tensor = schema.tensors[0];
  output_tensor.layout = {};
  if (!selected.keepdims &&
      selected.axis >= shape.size() - input.atomic_trailing_axes)
    --output_tensor.atomic_trailing_axes;
  if (selected.keepdims)
    output_tensor.descriptor.shape[selected.axis] = 1;
  else
    output_tensor.descriptor.shape.erase(
        output_tensor.descriptor.shape.begin() + selected.axis);
  if (input.layout.spatial) {
    const auto& original = input.layout;
    const bool structural =
        original.channel_axis && selected.axis == *original.channel_axis;
    if (structural || selected.keepdims) {
      auto layout = original;
      if (structural && !selected.keepdims) {
        if (layout.height_axis > selected.axis)
          --layout.height_axis;
        if (layout.width_axis > selected.axis)
          --layout.width_axis;
        layout.channel_axis.reset();
      }
      output_tensor.layout = std::move(layout);
    }
  }
  output.metadata.result_schema =
      std::make_shared<const SchemaTemplate>(std::move(schema));
  OperationPreparation prepared;
  prepared.outputs.push_back(std::move(output));
  prepared.state = std::make_shared<Preparation>(Preparation{selected});
  return Prepared(std::move(prepared));
}

using format_result::require;
using format_result::take;
using Poll = Result<ResultProgramPoll>;
ResultBuilder builder(const ResultProgramPhase& phase, bool /*empty*/) {
  auto result = take(ResultBuilder::start(
      phase.resources, *phase.query.output.result_schema,
      phase.query.semantic_key, {},
      phase.query.tile_extent));

  return result;
}
Poll empty_result(const ResultProgramPhase& phase) try {
  auto result = builder(phase, true);
  return Poll(ResultPublication{take(result.seal()), true});
} catch (const Status& status) {
  return Poll(status);
}
struct ChannelState final {
  Selection selected;
  bool requested = false;
  explicit ChannelState(Selection value) : selected(value) {}
  Poll poll(const ResultProgramPhase& phase) try {
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(8192, 8192)));
    const auto& output_spec = phase.query.output.result_schema->tensors[0];
    const auto& input_spec = phase.query.inputs[0].result_schema->tensors[0];
    const auto output_shape = output_spec.sample_shape();
    const auto input_shape = input_spec.sample_shape();
    const auto output = phase.query.tensor_outputs
                            ? *phase.query.tensor_outputs
                            : take(Footprint::all(output_shape));
    const auto axes = format_result::extraction_axes(
        input_shape.size(), input_spec.batch_axes.size() + selected.axis,
        selected.index, selected.keepdims);

    if (!requested) {
      requested = true;
      auto support = take(Footprint::none(input_shape));
      for (const auto& box : output.boxes())
        support = take(support.unite(take(Footprint::from_regions(
            input_shape, {format_result::mapped_region(box, axes)}))));
      ResultProgramNeed need;
      {
        auto tile_need_0 = sdk::tile_need_for_samples(
            phase.resources, phase.query, 0, 0, std::move(support));
        if (!tile_need_0.ok())
          return Result<ResultProgramPoll>(tile_need_0.status());
        need.tiles.push_back(tile_need_0.take_value());
      }
      return Poll(std::move(need));
    }
    auto result = builder(phase, false);
    const auto policy = selected.layout == Layout::View          ? "view"
                        : selected.layout == Layout::Materialize ? "materialize"
                                                                 : "auto";
    require(format_result::planes(output_spec, output, [&](const Region& box) {
      auto window = phase.tensors->at({0, 0}).acquire(
          format_result::mapped_region(box, axes), phase.query.cancellation);
      if (!window.ok())
        return window.status();
      return format_result::publish(phase, &result, box, window.value(), axes,
                                    policy);
    }));
    return Poll(ResultPublication{take(result.seal()), true});
  } catch (const Status& status) {
    return Poll(status);
  }
};

OperationDefinition extraction(const std::string& key, bool named,
                               SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = 1;
  OperationPortConstraint port;

  port.element_type_mask = 127;
  traits.input_schema = {port};
  traits.cacheable = false;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"axis", OperationParameterType::Int64, false},
      {"expected_channels", OperationParameterType::Int64, false},
      {"expected_source_layout", OperationParameterType::String, false},
      {"expected_source_schema", OperationParameterType::String, false},
      {"keepdims", OperationParameterType::Bool},
      {"layout", OperationParameterType::String},
      {"input_description", OperationParameterType::String, false},
      {"expected_inputs", OperationParameterType::String, false},
      {"authoring_member", OperationParameterType::String, false}};
  if (named) {
    traits.parameter_schema.push_back(
        {"match", OperationParameterType::String});
    traits.parameter_schema.push_back(
        {"selector", OperationParameterType::String});
  } else {
    traits.parameter_schema.push_back({"index", OperationParameterType::Int64});
  }
  auto& output = traits.outputs[0];
  output.key = "values";
  output.output_schema = port;
  output.result_schema = tensor_ops::scalar_schema();
  output.continuation_bytes = sizeof(ChannelState);
  output.maximum_dependency_stages = 2;
  definition.prepare_static = [named, profile](const auto& inputs,
                                               const auto& parameters) {
    return prepare_extraction(inputs, parameters, named, profile);
  };
  definition.start_result = [](const ResultProgramQuery& query,
                               const BufferAllocator& allocator) {
    if (query.tensor_outputs && query.tensor_outputs->empty())
      return ResultContinuation::stateless<empty_result>();
    const auto* prepared =
        static_cast<const Preparation*>(query.prepared->state());
    return ResultContinuation::make<ChannelState>(allocator,
                                                  prepared->selection);
  };
  return definition;
}
}  // namespace

Result<OperationPreparation> prepare_alpha_extraction(
    const std::vector<OperationMetadata>& inputs,
    const format_result::Params& p, numeric_ops::SequenceProfile profile) {
  return prepare_extraction(inputs, p, false, profile);
}
Status register_channel_extraction(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)}) {
    for (const auto& member : {std::make_pair("channel.extract_index", false),
                               std::make_pair("channel.extract_named", true)}) {
      auto status = registry->register_operation(
          extraction(std::string(member.first) + profile.first, member.second,
                     profile.second));
      if (!status.ok())
        return status;
    }
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
