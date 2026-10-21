#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "common/fixed_tensor.hpp"
#include "common/floating_environment.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class Ordered { Mean, Variance, Scan };
inline const char* ordered_symbol(Ordered kind) {
  return kind == Ordered::Mean       ? "mean"
         : kind == Ordered::Variance ? "variance"
                                     : "scan";
}
struct OrderedState final {
  TileGrid input;
  std::uint64_t total = 0, block = 64;
};
inline bool matches_region(const physical::Layout& layout,
                           const Region& region) {
  if (layout.rank != region.rank())
    return false;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    if (layout.origin[axis] != region.dimensions()[axis].offset ||
        layout.shape[axis] != region.dimensions()[axis].extent)
      return false;
  return true;
}
// Logical traversal is independent of the input's spatial and atomic axes.
struct OrderedReader final {
  const OrderedState& state;
  const ps_kernel_invocation_v8& invocation;
  const ps_kernel_const_view_v8* cached = nullptr;
  std::array<std::uint64_t, 8> at{};
  template <class Number>
  double next() noexcept {
    bool contains = cached != nullptr;
    for (unsigned axis = 0; contains && axis < state.input.rank(); ++axis)
      contains =
          at[axis] >= cached->layout->origin[axis] &&
          at[axis] - cached->layout->origin[axis] < cached->layout->shape[axis];
    if (!contains)
      cached = &invocation.inputs[fixed_tensor::input_tile(state.input, at)];
    __int128 offset = cached->byte_offset;
    for (unsigned axis = 0; axis < state.input.rank(); ++axis)
      offset += static_cast<__int128>(at[axis] - cached->layout->origin[axis]) *
                cached->layout->strides[axis];
    Number number;
    std::memcpy(&number,
                static_cast<const std::uint8_t*>(cached->allocation) +
                    static_cast<std::ptrdiff_t>(offset),
                sizeof(Number));
    for (unsigned axis = state.input.rank(); axis-- > 0;) {
      if (++at[axis] < state.input.shape()[axis])
        break;
      at[axis] = 0;
    }
    return number;
  }
};
template <class Number, Ordered Kind>
void ordered_invoke(const ps_kernel_invocation_v8* invocation,
                    ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  const auto& state =
      *static_cast<const OrderedState*>(invocation->prepared_context);
  double sum = 0, mean = 0;
  constexpr unsigned passes = Kind == Ordered::Variance ? 2 : 1;
  for (unsigned pass = 0; pass < passes; ++pass) {
    OrderedReader reader{state, *invocation};
    sum = 0;
    std::uint64_t checkpoint = 0;
    unsigned output_slot = 0;
    for (std::uint64_t index = 0; index < state.total; ++index) {
      if (!checkpoint) {
        if (invocation->cancellation.is_cancelled &&
            invocation->cancellation.is_cancelled(
                invocation->cancellation.context)) {
          *status = kernel_failure(ErrorCode::Cancelled);
          return;
        }
        checkpoint = state.block;
      }
      --checkpoint;
      const double number = reader.next<Number>();
      if (pass == 0) {
        sum += number;
      } else {
        const double difference = number - mean;
        const double square = difference * difference;
        sum += square;
      }
      if constexpr (Kind == Ordered::Scan) {
        while (output_slot < invocation->output_count &&
               index >= invocation->outputs[output_slot].layout->origin[0] +
                            invocation->outputs[output_slot].layout->shape[0])
          ++output_slot;
        if (output_slot < invocation->output_count) {
          const auto& output = invocation->outputs[output_slot];
          if (index >= output.layout->origin[0])
            std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                            output.byte_offset +
                            (index - output.layout->origin[0]) * sizeof(double),
                        &sum, sizeof(sum));
        }
      }
    }
    if (pass == 0)
      mean = sum / static_cast<double>(state.total);
  }
  if constexpr (Kind != Ordered::Scan) {
    const double result = sum / static_cast<double>(state.total);
    const auto& output = invocation->outputs[0];
    std::memcpy(
        static_cast<std::uint8_t*>(output.allocation) + output.byte_offset,
        &result, sizeof(result));
  }
  *status = {};
}
inline OperationDefinition fixed_ordered(const char* key, Ordered kind) {
  OperationDefinition operation;
  operation.key = key;
  operation.traits = tensor_ops::traits(1);
  auto& traits = operation.traits;
  traits.input_schema[0] = {};
  if (kind == Ordered::Scan) {
    traits.input_schema[0].element_type =
        static_cast<std::uint32_t>(ElementType::Float64);
    traits.input_schema[0].rank = 1;
  } else {
    traits.input_schema[0].element_type_mask = 12;
  }
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"block_size", OperationParameterType::Int64, false, true, 1, 65536}};
  operation.specialize_metadata = [kind](const auto& inputs, const auto&)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    auto valid = tensor_ops::check_tensor(inputs[0]);
    if (!valid.ok())
      return Answer(valid);
    const auto& input = inputs[0].result_schema->tensors[0];
    const auto shape = input.sample_shape();
    if (kind == Ordered::Scan && shape.size() != 1)
      return Answer(
          {ErrorCode::TypeMismatch, "ordered scan requires rank one"});
    auto count = input.sample_count();
    if (!count.ok())
      return Answer(count.status());
    auto schema = tensor_ops::scalar_schema();
    if (kind == Ordered::Scan)
      schema.tensors[0].descriptor.shape = shape;
    return Answer(std::vector<OperationOutputSpecialization>{
        {{std::make_shared<const SchemaTemplate>(std::move(schema))}, {}}});
  };
  operation.lower =
      [kind](const OperationLowering& query) -> Result<OperationProgram> {
    OperationProgramBuilder builder(query);
    if (!query.requests[0].empty()) {
      auto input = TileGrid::from_schema(*query.inputs[0].result_schema,
                                         query.tile_extent);
      if (!input.ok())
        return Result<OperationProgram>(input.status());
      if (input.value().tile_count() > query.limits.maximum_records ||
          input.value().tile_count() > query.limits.maximum_edges ||
          input.value().tile_count() > query.limits.maximum_work)
        return Result<OperationProgram>(
            {ErrorCode::ResourceExhausted, "ordered input tile limit"});
      auto task =
          builder.add_task(ordered_symbol(kind), "row-major-unchecked-f64-v1");
      if (!task.ok())
        return Result<OperationProgram>(task.status());
      for (TileIndex tile = 0; tile < input.value().tile_count(); ++tile) {
        auto status = builder.read_input(task.value(), 0, 0, tile);
        if (!status.ok())
          return Result<OperationProgram>(status);
      }
      for (const auto& member : query.requests[0].members)
        for (auto tile : member.second) {
          auto status =
              builder.write_output(task.value(), 0, member.first, tile);
          if (!status.ok())
            return Result<OperationProgram>(status);
        }
    }
    return builder.finish();
  };
  operation.prepare_kernel =
      [kind](const physical::KernelDesc& kernel,
             const physical::Placement& placement,
             const KernelPreparation& query, const ResourceSet&,
             const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    const auto invalid = [] {
      return Answer(
          {ErrorCode::TypeMismatch, "invalid ordered kernel signature"});
    };
    if (kernel.symbol != ordered_symbol(kind) ||
        kernel.contract != "row-major-unchecked-f64-v1" ||
        query.inputs.size() != 1 || query.outputs.size() != 1 ||
        query.writes.empty() || !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{})
      return invalid();
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "ordered kernel requires CPU Host placement"});
    auto state = std::make_unique<OrderedState>();
    auto input = TileGrid::from_schema(*query.inputs[0].result_schema,
                                       query.tile_extent);
    auto output = TileGrid::from_schema(*query.outputs[0].result_schema,
                                        query.tile_extent);
    if (!input.ok())
      return Answer(input.status());
    if (!output.ok())
      return Answer(output.status());
    state->input = input.take_value();
    auto count = query.inputs[0].result_schema->tensors[0].sample_count();
    if (!count.ok())
      return Answer(count.status());
    state->total = count.value();
    auto block = query.parameters.find("block_size");
    if (block != query.parameters.end())
      state->block =
          static_cast<std::uint64_t>(std::get<std::int64_t>(block->second));
    if (query.reads.size() != state->input.tile_count())
      return invalid();
    const auto dtype =
        query.inputs[0].result_schema->tensors[0].descriptor.element_type;
    for (std::size_t slot = 0; slot < query.reads.size(); ++slot) {
      const auto& read = query.reads[slot];
      auto region = state->input.region(slot);
      if (!region.ok())
        return Answer(region.status());
      if (read.port != 0 || read.member != 0 || read.tile_index != slot ||
          read.domain != physical::Domain::Host ||
          read.layout.element_type != dtype ||
          !matches_region(read.layout, region.value()))
        return invalid();
    }
    for (std::size_t slot = 0; slot < query.writes.size(); ++slot) {
      const auto& write = query.writes[slot];
      auto region = output.value().region(write.tile_index);
      if (!region.ok())
        return Answer(region.status());
      if (write.port != 0 || write.member != 0 ||
          write.domain != physical::Domain::Host ||
          write.layout.element_type != ElementType::Float64 ||
          !matches_region(write.layout, region.value()) ||
          (slot && query.writes[slot - 1].tile_index >= write.tile_index))
        return invalid();
    }
    if (kind != Ordered::Scan &&
        (query.writes.size() != 1 || query.writes[0].tile_index != 0))
      return invalid();
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    if (kind == Ordered::Scan)
      result.invoke_cpu = ordered_invoke<double, Ordered::Scan>;
    else if (kind == Ordered::Mean)
      result.invoke_cpu = dtype == ElementType::Float32
                              ? ordered_invoke<float, Ordered::Mean>
                              : ordered_invoke<double, Ordered::Mean>;
    else
      result.invoke_cpu = dtype == ElementType::Float32
                              ? ordered_invoke<float, Ordered::Variance>
                              : ordered_invoke<double, Ordered::Variance>;
    result.context = state.release();
    result.release = [](const void* context) {
      delete static_cast<const OrderedState*>(context);
    };
    return Answer(result);
  };
  return operation;
}
}  // namespace ps::plugin_internal::numeric_ops
