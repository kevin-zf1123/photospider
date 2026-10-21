#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_shaper.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
Result<TensorDescriptor> metadata(
    const std::vector<OperationMetadata>& inputs) {
  using Answer = Result<TensorDescriptor>;
  const auto mismatch = [] {
    return Status{ErrorCode::TypeMismatch,
                  "log shaper input/bound shapes and dtypes",
                  FailureReason::None,
                  {FailureOrigin::Schema, FailureScope::Unspecified}};
  };
  if (inputs.size() != 3)
    return Answer(mismatch());
  for (const auto& input : inputs) {
    auto valid = tensor_ops::check_tensor(input);
    if (!valid.ok())
      return Answer(valid);
  }
  const auto& member = inputs[0].result_schema->tensors[0];
  TensorDescriptor descriptor{member.descriptor.element_type,
                              member.sample_shape()};
  if ((descriptor.element_type != ElementType::Float32 &&
       descriptor.element_type != ElementType::Float64) ||
      descriptor.shape.empty() || descriptor.shape.size() > 8)
    return Answer(mismatch());
  for (unsigned port = 1; port < 3; ++port) {
    const auto& bound = inputs[port].result_schema->tensors[0];
    if (bound.descriptor.element_type != descriptor.element_type ||
        bound.sample_shape() != std::vector<std::uint64_t>{1})
      return Answer(mismatch());
  }
  std::uint64_t count = 1;
  for (auto extent : descriptor.shape) {
    if (!extent || extent > (UINT64_C(1) << 40) / count)
      return Answer(mismatch());
    count *= extent;
  }
  return Answer(std::move(descriptor));
}
SchemaTemplate output_schema(const TensorDescriptor& descriptor) {
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = descriptor;
  return schema;
}
std::array<Region, 3> input_regions(const Region& output) {
  return {output, Region({{0, 1}}), Region({{0, 1}})};
}
Result<OperationProgram> lower(bool inverse, const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto descriptor = metadata(query.inputs);
  if (!descriptor.ok())
    return Answer(descriptor.status());
  auto output = TileGrid::from_schema(output_schema(descriptor.value()),
                                      query.tile_extent);
  if (!output.ok())
    return Answer(output.status());
  std::array<TileGrid, 3> inputs;
  for (unsigned port = 0; port < 3; ++port) {
    auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    inputs[port] = grid.take_value();
  }
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task(inverse ? "inverse" : "forward",
                                   "log-shaper-unchecked-v1", {},
                                   {sizeof(numeric_ops::ExactShaper), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      const auto regions = input_regions(region.value());
      for (unsigned port = 0; port < 3; ++port) {
        auto reads = sdk::tiles_for_region(budget, inputs[port], regions[port]);
        if (!reads.ok())
          return Answer(reads.status());
        for (auto index : reads.value()) {
          auto valid = budget.consume({1});
          if (valid.ok())
            valid = builder.read_input(task.value(), port, 0, index);
          if (!valid.ok())
            return Answer(valid);
        }
      }
      auto valid = builder.write_output(task.value(), 0, member.first, tile);
      if (!valid.ok())
        return Answer(valid);
    }
  return builder.finish();
}
struct PreparedShaper final {
  bool inverse, narrow;
  SequenceProfile profile;
  fixed_tensor::PointInputs<3> inputs;
};
void invoke(const ps_kernel_invocation_v8* invocation,
            ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& state =
        *static_cast<const PreparedShaper*>(invocation->prepared_context);
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    const auto accept = [&](const Status& checked) {
      if (!checked.ok())
        *status = kernel_failure(checked.code, checked.message.c_str(),
                                 checked.reason);
      return checked.ok();
    };
    if (!accept(consume(1)))
      return;
    using Arithmetic = numeric_ops::ExactShaper;
    static_assert(alignof(Arithmetic) <= alignof(std::max_align_t));
    auto* math =
        new (invocation->scratch[0].allocation) Arithmetic(state.profile);
    const auto destroy = [](Arithmetic* value) { value->~Arithmetic(); };
    std::unique_ptr<Arithmetic, decltype(destroy)> guard(math, destroy);
    fixed_tensor::PointReader<3> reader{state.inputs, *invocation};
    const unsigned width = state.narrow ? 4 : 8;
    const auto lower = reader.bits(1, {0}, width),
               upper = reader.bits(2, {0}, width);
    const auto& output = invocation->outputs[0];
    std::array<std::uint64_t, 8> at{};
    std::uint64_t samples = 1;
    for (unsigned axis = 0; axis < output.layout->rank; ++axis) {
      at[axis] = output.layout->origin[axis];
      samples *= output.layout->shape[axis];
    }
    for (std::uint64_t sample = 0; sample < samples; ++sample) {
      if (!accept(consume(1)))
        return;
      auto value = math->evaluate(reader.bits(0, at, width), lower, upper,
                                  state.inverse, state.narrow, consume,
                                  [] { return Status::success(); });
      if (!accept(value.status()))
        return;
      const auto bits = value.value();
      std::uint64_t offset = output.byte_offset;
      for (unsigned axis = 0; axis < output.layout->rank; ++axis)
        offset += (at[axis] - output.layout->origin[axis]) *
                  output.layout->strides[axis];
      std::memcpy(static_cast<std::uint8_t*>(output.allocation) + offset, &bits,
                  width);
      for (unsigned axis = output.layout->rank; axis-- > 0;) {
        if (++at[axis] <
            output.layout->origin[axis] + output.layout->shape[axis])
          break;
        at[axis] = output.layout->origin[axis];
      }
    }
    *status = {};
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "log shaper invocation failed");
  }
}
Result<ps_prepared_kernel_v8> prepare(bool inverse, SequenceProfile profile,
                                      const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(
        {ErrorCode::TypeMismatch, "invalid fixed log shaper invocation"});
  };
  if (query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.parameters.empty() || !query.task_parameters.empty() ||
      query.scratch !=
          std::array<std::uint64_t, 3>{sizeof(numeric_ops::ExactShaper), 0, 0})
    return invalid();
  auto descriptor = metadata(query.inputs);
  if (!descriptor.ok())
    return Answer(descriptor.status());
  if (!query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(
          output_schema(descriptor.value())))
    return invalid();
  auto grid =
      TileGrid::from_schema(*query.outputs[0].result_schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto output = grid.value().region(query.writes[0].tile_index);
  if (!output.ok())
    return Answer(output.status());
  auto state = std::make_unique<PreparedShaper>();
  state->inverse = inverse;
  state->profile = profile;
  state->narrow = descriptor.value().element_type == ElementType::Float32;
  const auto regions = input_regions(output.value());
  auto valid =
      fixed_tensor::prepare_point_inputs(query, &state->inputs, &regions);
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = state.release();
  kernel.invoke_cpu = invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedShaper*>(context);
  };
  return Answer(kernel);
}
OperationDefinition operation(const std::string& key, bool inverse,
                              SequenceProfile profile) {
  OperationDefinition result;
  result.key = key;
  auto& traits = result.traits;
  traits.input_count = 3;
  traits.input_schema.resize(3);
  for (auto& input : traits.input_schema)
    input.element_type_mask = 12;
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  traits.requires_metadata_specialization = true;
  result.specialize_metadata = [profile](const auto& inputs, const auto&) {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto descriptor = metadata(inputs);
    if (!descriptor.ok())
      return Answer(descriptor.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization resolved;
    resolved.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        output_schema(descriptor.value()));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(resolved)});
  };
  result.lower = [inverse](const OperationLowering& query) {
    return lower(inverse, query);
  };
  result.prepare_kernel =
      [inverse, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != (inverse ? "inverse" : "forward") ||
        kernel.contract != "log-shaper-unchecked-v1")
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid log shaper kernel"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "log shaper requires CPU Host"});
    return prepare(inverse, profile, query);
  };
  return result;
}
}  // namespace
Status register_log_shapers(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (bool inverse : {false, true}) {
      auto status = registry->register_operation(
          operation(std::string(inverse ? "curve.log2_shaper_inverse"
                                        : "curve.log2_shaper") +
                        profile.first,
                    inverse, profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
