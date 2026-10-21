#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/array_profiles.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
struct ArrayMetadata final {
  std::vector<std::uint64_t> source;
  TensorDescriptor output;
  std::array<unsigned, 8> axes{};
};
Result<ArrayMetadata> metadata(
    bool constant, const OperationMetadata& input,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<ArrayMetadata>;
  auto valid = tensor_ops::check_tensor(input);
  if (!valid.ok())
    return Answer(valid);
  ArrayMetadata result;
  const auto& source = input.result_schema->tensors[0];
  result.source = source.sample_shape();
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch, message});
  };
  if (result.source.empty() || result.source.size() > 8)
    return mismatch("array source rank outside 1..8");
  if (constant && result.source != std::vector<std::uint64_t>{1})
    return mismatch("constant requires a scalar [1]");
  auto shape = numeric_ops::parse_array_list(
      std::get<std::string>(parameters.at("shape")), true);
  if (!shape.ok())
    return Answer(shape.status());
  result.output = {source.descriptor.element_type, shape.take_value()};
  if (!constant) {
    auto parsed = numeric_ops::parse_array_list(
        std::get<std::string>(parameters.at("axis_map")), false);
    if (!parsed.ok())
      return Answer(parsed.status());
    if (parsed.value().size() != result.source.size() ||
        result.output.shape.size() < result.source.size())
      return mismatch(
          "broadcast must map every source axis without rank reduction");
    unsigned used = 0;
    for (unsigned axis = 0; axis < result.source.size(); ++axis) {
      auto mapped = parsed.value()[axis];
      if (mapped >= result.output.shape.size() || (used & (1U << mapped)))
        return Answer(numeric_ops::array_parameter_error(
            "broadcast axis map must be injective and in range"));
      used |= 1U << mapped;
      result.axes[axis] = static_cast<unsigned>(mapped);
      if (result.source[axis] != 1 &&
          result.source[axis] != result.output.shape[mapped])
        return mismatch("broadcast mapped extent must match or be one");
    }
  }
  return Answer(std::move(result));
}
Region source_region(bool constant, const ArrayMetadata& description,
                     const Region& output) {
  if (constant)
    return Region::whole({1});
  std::vector<RegionDimension> dimensions;
  for (unsigned axis = 0; axis < description.source.size(); ++axis)
    dimensions.push_back(description.source[axis] == 1
                             ? RegionDimension{0, 1}
                             : output.dimensions()[description.axes[axis]]);
  return Region(std::move(dimensions));
}
Result<OperationProgram> lower_array(bool constant,
                                     const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto parsed = metadata(constant, query.inputs[0], query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  auto output =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  auto input =
      TileGrid::from_schema(*query.inputs[0].result_schema, query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  if (!input.ok())
    return Answer(input.status());
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task(constant ? "constant" : "broadcast",
                                   "array-bitcopy-v1");
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      auto reads = sdk::tiles_for_region(
          budget, input.value(),
          source_region(constant, parsed.value(), region.value()));
      if (!reads.ok())
        return Answer(reads.status());
      for (auto index : reads.value()) {
        auto status = budget.consume({1});
        if (status.ok())
          status = builder.read_input(task.value(), 0, 0, index);
        if (!status.ok())
          return Answer(status);
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
struct PreparedArray final {
  bool constant = false;
  SequenceProfile profile;
  ArrayMetadata description;
  fixed_tensor::PointInputs<1> inputs;
};
void array_invoke(const ps_kernel_invocation_v8* query,
                  ps_kernel_status_v8* status) noexcept {
  const auto& state =
      *static_cast<const PreparedArray*>(query->prepared_context);
  const auto cancelled = [&] {
    return query->cancellation.is_cancelled &&
           query->cancellation.is_cancelled(query->cancellation.context);
  };
  if (cancelled()) {
    *status = kernel_failure(ErrorCode::Cancelled);
    return;
  }
  fixed_tensor::PointReader<1> reader{state.inputs, *query};
  const auto& output = query->outputs[0];
  const auto& layout = *output.layout;
  const auto width =
      TensorSpan::element_size(state.description.output.element_type);
  auto* destination =
      static_cast<std::uint8_t*>(output.allocation) + output.byte_offset;
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    count *= layout.shape[axis];
  if (state.constant) {
    const auto bits = reader.bits(0, {}, width);
    std::memcpy(destination, &bits, width);
    const auto bytes = count * width;
    // Grow a filled prefix; each copy is disjoint and at most 64 KiB.
    for (std::uint64_t offset = width; offset < bytes;) {
      if (cancelled()) {
        *status = kernel_failure(ErrorCode::Cancelled);
        return;
      }
      const auto size = std::min({offset, UINT64_C(65536), bytes - offset});
      std::memcpy(destination + offset, destination,
                  static_cast<std::size_t>(size));
      offset += size;
    }
  } else {
    std::array<std::uint64_t, 8> at{}, source{};
    std::array<std::uint8_t, 32> block{};
    unsigned buffered = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
      if (index % 64 == 0 && cancelled()) {
        *status = kernel_failure(ErrorCode::Cancelled);
        return;
      }
      auto remainder = index;
      for (unsigned axis = layout.rank; axis-- > 0;) {
        at[axis] = layout.origin[axis] + remainder % layout.shape[axis];
        remainder /= layout.shape[axis];
      }
      for (unsigned axis = 0; axis < state.description.source.size(); ++axis)
        source[axis] = state.description.source[axis] == 1
                           ? 0
                           : at[state.description.axes[axis]];
      const auto bits = reader.bits(0, source, width);
      std::memcpy(block.data() + buffered, &bits, width);
      buffered += width;
      if (buffered == block.size() || index + 1 == count) {
        numeric_ops::array_copy_block(destination, block.data(), buffered,
                                      state.profile);
        destination += buffered;
        buffered = 0;
      }
    }
  }
  *status = {};
}
OperationDefinition array_operation(const std::string& key, bool constant,
                                    SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.requires_metadata_specialization = true;
  traits.input_count = 1;
  traits.input_schema.resize(1);
  traits.input_schema[0].element_type_mask = 127;
  traits.parameter_schema = {{"shape", OperationParameterType::String}};
  if (!constant)
    traits.parameter_schema.push_back(
        {"axis_map", OperationParameterType::String});
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [constant, profile](const auto& inputs,
                                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto parsed = metadata(constant, inputs[0], parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = parsed.value().output;
    OperationOutputSpecialization result;
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = [constant](const auto& query) {
    return lower_array(constant, query);
  };
  operation.prepare_kernel =
      [constant, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    const auto invalid = [] {
      return Answer({ErrorCode::TypeMismatch, "invalid array invocation"});
    };
    if (kernel.symbol != (constant ? "constant" : "broadcast") ||
        kernel.contract != "array-bitcopy-v1" || query.inputs.size() != 1 ||
        query.outputs.size() != 1 || query.writes.size() != 1 ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{})
      return invalid();
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer(
          {ErrorCode::BackendUnavailable, "array requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    auto parsed = metadata(constant, query.inputs[0], query.parameters);
    if (!parsed.ok())
      return Answer(parsed.status());
    auto state = std::make_unique<PreparedArray>();
    state->constant = constant;
    state->profile = profile;
    state->description = parsed.take_value();
    const auto& target = query.outputs[0].result_schema->tensors[0];
    if (target.sample_shape() != state->description.output.shape ||
        target.descriptor.element_type !=
            state->description.output.element_type)
      return invalid();
    auto output_grid = TileGrid::from_schema(*query.outputs[0].result_schema,
                                             query.tile_extent);
    if (!output_grid.ok())
      return Answer(output_grid.status());
    auto output_region = output_grid.value().region(query.writes[0].tile_index);
    if (!output_region.ok())
      return Answer(output_region.status());
    const auto input =
        source_region(constant, state->description, output_region.value());
    const std::array<Region, 1> regions{input};
    auto valid =
        fixed_tensor::prepare_point_inputs(query, &state->inputs, &regions);
    if (!valid.ok())
      return Answer(valid);
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.context = state.release();
    result.invoke_cpu = array_invoke;
    result.release = [](const void* context) {
      delete static_cast<const PreparedArray*>(context);
    };
    return Answer(result);
  };
  return operation;
}
}  // namespace
Status register_numeric_arrays(OperationRegistry* registry) {
  for (const auto& variant :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool constant : {true, false}) {
      auto status = registry->register_operation(array_operation(
          std::string(constant ? "numeric.constant" : "numeric.broadcast") +
              variant.first,
          constant, variant.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
