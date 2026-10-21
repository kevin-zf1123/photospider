#include <algorithm>
#include <array>
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
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "photospider/core/content_digest.hpp"
#include "photospider/core/status_helpers.hpp"
#include "photospider/core/utf8_validation.hpp"
#include "photospider/ops/format/channel.hpp"
#include "photospider/ops/tensor_description.hpp"
#include "photospider/plugin/tile_geometry.hpp"

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

Region source_region(const Region& output, const Selection& selected,
                     std::size_t batch_rank) {
  auto dimensions = output.dimensions();
  const auto axis = batch_rank + selected.axis;
  if (selected.keepdims)
    dimensions[axis] = {selected.index, 1};
  else
    dimensions.insert(dimensions.begin() + axis, {selected.index, 1});
  return Region(std::move(dimensions));
}
Result<OperationProgram> lower(const OperationLowering& query) {
  using ResultProgram = Result<OperationProgram>;
  const auto& selected =
      static_cast<const Preparation*>(query.static_state)->selection;
  const auto& source = query.inputs[0].result_schema->tensors[0];
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!input.ok())
    return ResultProgram(input.status());
  if (!output.ok())
    return ResultProgram(output.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task("extract", "channel-copy-v1");
      if (!task.ok())
        return ResultProgram(task.status());
      if (selected.layout == Layout::View) {
        // Fixed produced tiles own distinct storage. A forced view task reports
        // the unsupported policy without asking an upstream producer for data.
        auto status = builder.read_input_schema(task.value(), 0);
        if (!status.ok())
          return ResultProgram(status);
      } else {
        auto region = output.value().region(tile);
        if (!region.ok())
          return ResultProgram(region.status());
        auto reads = sdk::tiles_for_region(
            budget, input.value(),
            source_region(region.value(), selected, source.batch_axes.size()));
        if (!reads.ok())
          return ResultProgram(reads.status());
        for (auto index : reads.value()) {
          auto status = budget.consume({1});
          if (status.ok())
            status = builder.read_input(task.value(), 0, 0, index);
          if (!status.ok())
            return ResultProgram(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return ResultProgram(status);
    }
  return builder.finish();
}
struct PreparedExtraction final {
  Selection selected;
  unsigned axis = 0, source_rank = 0, width = 0;
  fixed_tensor::PointInputs<1> inputs;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  const auto cancelled = [&] {
    return invocation->cancellation.is_cancelled &&
           invocation->cancellation.is_cancelled(
               invocation->cancellation.context);
  };
  if (cancelled()) {
    *status = kernel_failure(ErrorCode::Cancelled);
    return;
  }
  const auto& state =
      *static_cast<const PreparedExtraction*>(invocation->prepared_context);
  if (state.selected.layout == Layout::View) {
    *status = kernel_failure(ErrorCode::InvalidArgument,
                             "ViewUnavailable: fixed extraction requires copy",
                             FailureReason::InvalidDomain);
    return;
  }
  fixed_tensor::PointReader<1> reader{state.inputs, *invocation};
  const auto& output = invocation->outputs[0];
  std::array<std::uint64_t, 8> at{}, source{};
  std::uint64_t samples = 1;
  for (unsigned axis = 0; axis < output.layout->rank; ++axis) {
    at[axis] = output.layout->origin[axis];
    samples *= output.layout->shape[axis];
  }
  auto* destination =
      static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
  for (std::uint64_t sample = 0; sample < samples; ++sample) {
    if (sample % 256 == 0 && cancelled()) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    for (unsigned axis = 0; axis < state.source_rank; ++axis)
      source[axis] =
          axis == state.axis
              ? state.selected.index
              : at[axis - (!state.selected.keepdims && axis > state.axis)];
    const auto bits = reader.bits(0, source, state.width);
    std::memcpy(destination + sample * state.width, &bits, state.width);
    for (unsigned axis = output.layout->rank; axis-- > 0;) {
      if (++at[axis] < output.layout->origin[axis] + output.layout->shape[axis])
        break;
      at[axis] = output.layout->origin[axis];
    }
  }
  *status = {};
}
Result<ps_prepared_kernel_v8> prepare_kernel(const KernelPreparation& query) {
  using ResultKernel = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return ResultKernel(
        {ErrorCode::TypeMismatch, "invalid channel extraction signature"});
  };
  if (!query.static_state || query.inputs.size() != 1 ||
      query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty() ||
      query.scratch != std::array<std::uint64_t, 3>{})
    return invalid();
  const auto& selected =
      static_cast<const Preparation*>(query.static_state)->selection;
  const auto& source = query.inputs[0].result_schema->tensors[0];
  const auto& write = query.writes[0];
  auto grid =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!grid.ok())
    return ResultKernel(grid.status());
  auto region = grid.value().region(write.tile_index);
  if (!region.ok())
    return ResultKernel(region.status());
  if (write.port || write.member || write.domain != physical::Domain::Host ||
      write.layout.element_type != source.descriptor.element_type ||
      !fixed_tensor::matches_region(write.layout, region.value()))
    return invalid();
  auto state = std::make_unique<PreparedExtraction>();
  state->selected = selected;
  state->axis = source.batch_axes.size() + selected.axis;
  state->source_rank = source.sample_shape().size();
  state->width = TensorSpan::element_size(source.descriptor.element_type);
  if (selected.layout == Layout::View) {
    if (!query.reads.empty())
      return invalid();
  } else {
    const std::array<Region, 1> regions{
        source_region(region.value(), selected, source.batch_axes.size())};
    auto status =
        fixed_tensor::prepare_input_regions(query, &state->inputs, regions);
    if (!status.ok())
      return ResultKernel(status);
  }
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = state.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedExtraction*>(context);
  };
  return ResultKernel(kernel);
}

OperationDefinition extraction(const std::string& key, bool named,
                               SequenceProfile profile) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = 1;
  OperationPortConstraint port;

  port.element_type_mask = 127;
  traits.input_schema = {port};
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
  definition.prepare_static = [named, profile](const auto& inputs,
                                               const auto& parameters) {
    return prepare_extraction(inputs, parameters, named, profile);
  };
  definition.lower = lower;
  definition.prepare_kernel =
      [](const physical::KernelDesc& kernel,
         const physical::Placement& placement, const KernelPreparation& query,
         const ResourceSet&,
         const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "extract" || kernel.contract != "channel-copy-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "unknown channel extraction kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "extraction requires CPU Host"});
    return prepare_kernel(query);
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
