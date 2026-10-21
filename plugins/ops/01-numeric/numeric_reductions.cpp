#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_moments.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/registry.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal {
namespace {
using numeric_ops::SequenceProfile;
enum class ReductionKind { Sum, Minimum, Maximum, Mean, Count, Variance, Std };
struct ReductionMetadata {
  unsigned mask = 0;
  std::uint64_t count = 1, ddof = 0;
  TensorDescriptor output;
};
Result<ReductionMetadata> metadata(
    ReductionKind kind, const ResultTensorSpec& input,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<ReductionMetadata>;
  const auto mismatch = [](const char* message) {
    return Answer(Status{ErrorCode::TypeMismatch,
                         message,
                         FailureReason::None,
                         {FailureOrigin::Schema, FailureScope::Unspecified}});
  };
  const TensorDescriptor descriptor{input.descriptor.element_type,
                                    input.sample_shape()};
  if (descriptor.shape.empty() || descriptor.shape.size() > 8)
    return mismatch("reduction requires rank 1..8");
  std::uint64_t elements = 1;
  for (auto size : descriptor.shape) {
    if (!size || size > (UINT64_C(1) << 40) / elements)
      return mismatch("reduction input exceeds 2^40 elements");
    elements *= size;
  }
  auto axes = numeric_ops::parse_array_list(
      std::get<std::string>(parameters.at("axes")), false);
  if (!axes.ok())
    return Answer(axes.status());
  ReductionMetadata result;
  result.output = descriptor;
  for (auto axis : axes.value()) {
    if (axis >= descriptor.shape.size() || (result.mask & (1U << axis)))
      return Answer(numeric_ops::array_parameter_error(
          "invalid or duplicate reduction axis"));
    result.mask |= 1U << axis;
    result.count *= descriptor.shape[axis];
    result.output.shape[axis] = 1;
  }
  if (kind == ReductionKind::Count)
    result.output.element_type = ElementType::Int64;
  if (kind == ReductionKind::Sum || kind == ReductionKind::Mean ||
      kind == ReductionKind::Variance || kind == ReductionKind::Std) {
    const auto& dtype = std::get<std::string>(parameters.at("dtype"));
    if (dtype == "uint8")
      result.output.element_type = ElementType::UInt8;
    else if (dtype == "int64")
      result.output.element_type = ElementType::Int64;
    else if (dtype == "float32")
      result.output.element_type = ElementType::Float32;
    else if (dtype == "float64")
      result.output.element_type = ElementType::Float64;
    else
      return Answer(
          numeric_ops::array_parameter_error("unsupported reduction dtype"));
    const bool source_float = descriptor.element_type == ElementType::Float32 ||
                              descriptor.element_type == ElementType::Float64;
    const bool target_float =
        result.output.element_type == ElementType::Float32 ||
        result.output.element_type == ElementType::Float64;
    if (kind == ReductionKind::Sum ? source_float != target_float
                                   : !target_float)
      return mismatch("reduction source/destination numeric domains differ");
  }
  if (kind == ReductionKind::Variance || kind == ReductionKind::Std) {
    const auto ddof = std::get<std::int64_t>(parameters.at("ddof"));
    result.ddof = static_cast<std::uint64_t>(ddof);
  }
  return Answer(std::move(result));
}
const char* operation_name(ReductionKind kind) {
  switch (kind) {
    case ReductionKind::Sum:
      return "reduce_sum";
    case ReductionKind::Minimum:
      return "reduce_minimum";
    case ReductionKind::Maximum:
      return "reduce_maximum";
    case ReductionKind::Mean:
      return "reduce_mean";
    case ReductionKind::Count:
      return "reduce_count";
    case ReductionKind::Variance:
      return "reduce_variance";
    case ReductionKind::Std:
      return "reduce_std";
  }
  return "invalid";
}
struct ReductionState final {
  std::variant<numeric_ops::ExactAggregate, numeric_ops::ExactMoments>
      arithmetic;
  ReductionState(ReductionKind kind, SequenceProfile profile, ElementType type)
      : arithmetic(std::in_place_type<numeric_ops::ExactAggregate>, profile,
                   kind == ReductionKind::Minimum
                       ? numeric_ops::AggregateKind::Minimum
                   : kind == ReductionKind::Maximum
                       ? numeric_ops::AggregateKind::Maximum
                       : numeric_ops::AggregateKind::Sum,
                   type) {
    if (kind == ReductionKind::Variance || kind == ReductionKind::Std)
      arithmetic.emplace<numeric_ops::ExactMoments>(profile, type);
  }
};
struct PreparedReduction final {
  ReductionKind kind;
  SequenceProfile profile;
  ReductionMetadata description;
  fixed_tensor::PointInputs<1> inputs;
  ElementType source_type = ElementType::UInt8;
};
Region reduction_region(const Region& output, unsigned mask,
                        const std::vector<std::uint64_t>& input_shape) {
  auto dimensions = output.dimensions();
  for (unsigned axis = 0; axis < dimensions.size(); ++axis)
    if (mask & (1U << axis))
      dimensions[axis] = {0, input_shape[axis]};
  return Region(std::move(dimensions));
}
template <bool Count>
void reduction_invoke(const ps_kernel_invocation_v8* invocation,
                      ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  const auto& state =
      *static_cast<const PreparedReduction*>(invocation->prepared_context);
  const auto& description = state.description;
  static_assert(alignof(ReductionState) <= alignof(std::max_align_t));
  try {
    const auto destroy = [](ReductionState* value) {
      value->~ReductionState();
    };
    std::unique_ptr<ReductionState, decltype(destroy)> math(nullptr, destroy);
    if constexpr (!Count)
      math.reset(new (invocation->scratch[0].allocation) ReductionState(
          state.kind, state.profile, state.source_type));
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    fixed_tensor::PointReader<1> reader{state.inputs, *invocation};
    const auto& output = invocation->outputs[0];
    const auto& layout = *output.layout;
    const auto out_width =
        TensorSpan::element_size(description.output.element_type);
    const auto in_width = TensorSpan::element_size(state.source_type);
    std::array<std::uint64_t, 8> at{};
    std::copy(layout.origin, layout.origin + layout.rank, at.begin());
    std::uint64_t groups = 1;
    for (unsigned axis = 0; axis < layout.rank; ++axis)
      groups *= layout.shape[axis];
    for (std::uint64_t group = 0; group < groups; ++group) {
      auto checked = consume(1);
      if (!checked.ok()) {
        *status = kernel_failure(checked.code);
        return;
      }
      std::uint64_t bits = description.count;
      if constexpr (!Count) {
        std::visit([](auto& arithmetic) { arithmetic.reset(); },
                   math->arithmetic);
        auto source = at;
        for (std::uint64_t i = 0; i < description.count; ++i) {
          const auto value = reader.bits(0, source, in_width);
          auto added = std::visit(
              [&](auto& arithmetic) { return arithmetic.add(value, consume); },
              math->arithmetic);
          if (!added.ok()) {
            *status =
                kernel_failure(added.code, added.message.c_str(), added.reason);
            return;
          }
          for (unsigned axis = layout.rank; axis-- > 0;)
            if (description.mask & (1U << axis)) {
              if (++source[axis] < state.inputs.grids[0].shape()[axis])
                break;
              source[axis] = 0;
            }
        }
        Result<std::uint64_t> value(Status{ErrorCode::Internal, {}});
        if (auto* moments =
                std::get_if<numeric_ops::ExactMoments>(&math->arithmetic))
          value = moments->finish(description.output.element_type,
                                  description.count, description.ddof,
                                  state.kind == ReductionKind::Std, consume);
        else
          value =
              std::get<numeric_ops::ExactAggregate>(math->arithmetic)
                  .finish_as(
                      description.output.element_type,
                      state.kind == ReductionKind::Mean ? description.count : 1,
                      consume);
        if (!value.ok()) {
          *status = kernel_failure(value.status().code,
                                   value.status().message.c_str(),
                                   value.status().reason);
          return;
        }
        bits = value.value();
      }
      std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                      output.byte_offset + group * out_width,
                  &bits, out_width);
      for (unsigned axis = layout.rank; axis-- > 0;) {
        if (++at[axis] < layout.origin[axis] + layout.shape[axis])
          break;
        at[axis] = layout.origin[axis];
      }
    }
    *status = {};
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "reduction arithmetic failed");
  }
}
Result<OperationProgram> lower_reduction(ReductionKind kind,
                                         const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto parsed = metadata(kind, query.inputs[0].result_schema->tensors[0],
                         query.parameters);
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
      auto task = builder.add_task(
          operation_name(kind), "exact-reduction-unchecked-v2", {},
          {kind == ReductionKind::Count ? 0 : sizeof(ReductionState), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      if (kind == ReductionKind::Count) {
        auto status = builder.read_input_schema(task.value(), 0);
        if (!status.ok())
          return Answer(status);
      } else {
        auto reads = sdk::tiles_for_region(
            budget, input.value(),
            reduction_region(
                region.value(), parsed.value().mask,
                query.inputs[0].result_schema->tensors[0].sample_shape()));
        if (!reads.ok())
          return Answer(reads.status());
        for (auto index : reads.value()) {
          auto status = budget.consume({1});
          if (status.ok())
            status = builder.read_input(task.value(), 0, 0, index);
          if (!status.ok())
            return Answer(status);
        }
      }
      auto status = builder.write_output(task.value(), 0, member.first, tile);
      if (!status.ok())
        return Answer(status);
    }
  return builder.finish();
}
Result<ps_prepared_kernel_v8> prepare_reduction(
    ReductionKind kind, SequenceProfile profile,
    const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer(
        {ErrorCode::TypeMismatch, "invalid reduction tile signature"});
  };
  if (query.inputs.size() != 1 || query.outputs.size() != 1 ||
      query.writes.size() != 1)
    return invalid();
  auto parsed = metadata(kind, query.inputs[0].result_schema->tensors[0],
                         query.parameters);
  if (!parsed.ok())
    return Answer(parsed.status());
  auto state = std::make_unique<PreparedReduction>();
  state->kind = kind;
  state->profile = profile;
  state->source_type =
      query.inputs[0].result_schema->tensors[0].descriptor.element_type;
  state->description = parsed.take_value();
  const auto& write = query.writes[0];
  if (query.outputs[0].result_schema->tensors[0].descriptor.shape !=
          state->description.output.shape ||
      write.layout.element_type != state->description.output.element_type)
    return invalid();
  if (kind == ReductionKind::Count) {
    auto grid = TileGrid::from_schema(*query.outputs[0].result_schema,
                                      query.tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    auto region = grid.value().region(write.tile_index);
    if (!region.ok())
      return Answer(region.status());
    if (!query.reads.empty() || write.port || write.member ||
        write.domain != physical::Domain::Host ||
        !fixed_tensor::matches_region(write.layout, region.value()))
      return invalid();
  } else {
    const std::array<Region, 1> regions = {reduction_region(
        fixed_tensor::view_region(write.layout), state->description.mask,
        query.inputs[0].result_schema->tensors[0].sample_shape())};
    auto valid =
        fixed_tensor::prepare_point_inputs(query, &state->inputs, &regions);
    if (!valid.ok())
      return Answer(valid);
  }
  ps_prepared_kernel_v8 result{};
  result.struct_size = sizeof(result);
  result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  result.invoke_cpu = kind == ReductionKind::Count ? reduction_invoke<true>
                                                   : reduction_invoke<false>;
  result.context = state.release();
  result.release = [](const void* context) {
    delete static_cast<const PreparedReduction*>(context);
  };
  return Answer(result);
}
OperationDefinition reduction_operation(const std::string& key,
                                        ReductionKind kind,
                                        SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.input_count = 1;
  traits.input_schema.resize(1);

  traits.input_schema[0].element_type_mask = 15;
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {{"axes", OperationParameterType::String}};
  if (kind == ReductionKind::Sum || kind == ReductionKind::Mean ||
      kind == ReductionKind::Variance || kind == ReductionKind::Std)
    traits.parameter_schema.push_back(
        {"dtype", OperationParameterType::String});
  if (kind == ReductionKind::Variance || kind == ReductionKind::Std)
    traits.parameter_schema.insert(traits.parameter_schema.begin() + 1,
                                   {"ddof", OperationParameterType::Int64});
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata = [kind, profile](const auto& inputs,
                                                  const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto valid = tensor_ops::check_tensor(inputs[0]);
    if (!valid.ok())
      return Answer(valid);
    auto resolved =
        metadata(kind, inputs[0].result_schema->tensors[0], parameters);
    if (!resolved.ok())
      return Answer(resolved.status());
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    OperationOutputSpecialization result;
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = resolved.value().output;
    result.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
    return Answer(
        std::vector<OperationOutputSpecialization>{std::move(result)});
  };
  operation.lower = [kind](const OperationLowering& query) {
    return lower_reduction(kind, query);
  };
  operation.prepare_kernel =
      [kind, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    if (kernel.symbol != operation_name(kind) ||
        kernel.contract != "exact-reduction-unchecked-v2" ||
        !query.task_parameters.empty() ||
        query.scratch !=
            std::array<std::uint64_t, 3>{
                kind == ReductionKind::Count ? 0 : sizeof(ReductionState), 0,
                0})
      return Answer(
          {ErrorCode::TypeMismatch, "invalid reduction kernel signature"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "reduction requires CPU Host placement"});
    auto available = numeric_ops::sequence_profile_available(profile);
    if (!available.ok())
      return Answer(available);
    return prepare_reduction(kind, profile, query);
  };
  return operation;
}
}  // namespace
Status register_numeric_reductions(OperationRegistry* registry) {
  for (const auto& profile :
       {std::make_pair("_strict", SequenceProfile::Strict),
        std::make_pair("_accelerated_apple_silicon",
                       SequenceProfile::AppleSilicon),
        std::make_pair("_accelerated_x86_64", SequenceProfile::X86Avx2)})
    for (auto kind :
         {ReductionKind::Sum, ReductionKind::Minimum, ReductionKind::Maximum,
          ReductionKind::Mean, ReductionKind::Count, ReductionKind::Variance,
          ReductionKind::Std}) {
      auto status = registry->register_operation(reduction_operation(
          std::string("numeric.") + operation_name(kind) + profile.first, kind,
          profile.second));
      if (!status.ok())
        return status;
    }
  return Status::success();
}
}  // namespace ps::plugin_internal
