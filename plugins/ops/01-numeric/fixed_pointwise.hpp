#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "common/fixed_tensor.hpp"
#include "common/floating_environment.hpp"
#include "common/tensor_program.hpp"
#include "photospider/plugin/tile_geometry.hpp"

namespace ps::plugin_internal::numeric_ops {
enum class Pointwise {
  Add,
  Subtract,
  Multiply,
  Divide,
  Clamp,
  Abs,
  Minimum,
  Maximum
};
inline unsigned pointwise_inputs(Pointwise kind) {
  return kind == Pointwise::Clamp || kind == Pointwise::Abs ? 1 : 2;
}
struct PointwiseState final {
  double minimum = 0, maximum = 0;
  std::array<TileGrid, 2> grids;
  // Sorted tile IDs map the fixed logical input grid to invocation slots.
  std::array<std::vector<std::pair<TileIndex, std::uint32_t>>, 2> slots;
};
template <class Number, Pointwise Kind>
void pointwise_invoke(const ps_kernel_invocation_v8* invocation,
                      ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  const auto& state =
      *static_cast<const PointwiseState*>(invocation->prepared_context);
  const auto& output = invocation->outputs[0];
  const auto& layout = *output.layout;
  std::array<std::uint64_t, 8> at{};
  std::copy(layout.origin, layout.origin + layout.rank, at.begin());
  std::uint64_t count = 1;
  for (unsigned axis = 0; axis < layout.rank; ++axis)
    count *= layout.shape[axis];
  constexpr unsigned ports =
      Kind == Pointwise::Clamp || Kind == Pointwise::Abs ? 1 : 2;
  std::array<const ps_kernel_const_view_v8*, 2> cached{};
  for (std::uint64_t sample = 0; sample < count; ++sample) {
    if (!(sample & 1023) && invocation->cancellation.is_cancelled &&
        invocation->cancellation.is_cancelled(
            invocation->cancellation.context)) {
      *status = kernel_failure(ErrorCode::Cancelled);
      return;
    }
    Number numbers[2]{};
    for (unsigned port = 0; port < ports; ++port) {
      auto* view = cached[port];
      bool contained = view != nullptr;
      for (unsigned axis = 0; contained && axis < layout.rank; ++axis)
        contained =
            at[axis] >= view->layout->origin[axis] &&
            at[axis] - view->layout->origin[axis] < view->layout->shape[axis];
      if (!contained) {
        const auto tile = fixed_tensor::input_tile(state.grids[port], at);
        const auto& slots = state.slots[port];
        auto found = std::lower_bound(
            slots.begin(), slots.end(), tile,
            [](const auto& item, TileIndex id) { return item.first < id; });
        // Prepare proved complete coverage and immutable slot membership.
        view = cached[port] = &invocation->inputs[found->second];
      }
      __int128 offset = view->byte_offset;
      for (unsigned axis = 0; axis < layout.rank; ++axis)
        offset += static_cast<__int128>(at[axis] - view->layout->origin[axis]) *
                  view->layout->strides[axis];
      std::memcpy(&numbers[port],
                  static_cast<const std::uint8_t*>(view->allocation) +
                      static_cast<std::ptrdiff_t>(offset),
                  sizeof(Number));
    }
    const Number a = numbers[0], b = numbers[1];
    Number value;
    if constexpr (Kind == Pointwise::Add)
      value = a + b;
    else if constexpr (Kind == Pointwise::Subtract)
      value = a - b;
    else if constexpr (Kind == Pointwise::Multiply)
      value = a * b;
    else if constexpr (Kind == Pointwise::Divide)
      value = a / b;
    else if constexpr (Kind == Pointwise::Clamp)
      value = static_cast<Number>(
          std::clamp(static_cast<double>(a), state.minimum, state.maximum));
    else if constexpr (Kind == Pointwise::Abs)
      value = std::abs(a);
    else if constexpr (Kind == Pointwise::Minimum)
      value = a == 0 && b == 0 ? static_cast<Number>(-0.0) : std::min(a, b);
    else
      value = a == 0 && b == 0 ? static_cast<Number>(0.0) : std::max(a, b);
    std::memcpy(static_cast<std::uint8_t*>(output.allocation) +
                    output.byte_offset + sample * sizeof(Number),
                &value, sizeof(Number));
    for (unsigned axis = layout.rank; axis-- > 0;) {
      if (++at[axis] < layout.origin[axis] + layout.shape[axis])
        break;
      at[axis] = layout.origin[axis];
    }
  }
  *status = {};
}
template <class Number>
ps_kernel_cpu_invoke_v8 pointwise_entry(Pointwise kind) {
  switch (kind) {
    case Pointwise::Add:
      return pointwise_invoke<Number, Pointwise::Add>;
    case Pointwise::Subtract:
      return pointwise_invoke<Number, Pointwise::Subtract>;
    case Pointwise::Multiply:
      return pointwise_invoke<Number, Pointwise::Multiply>;
    case Pointwise::Divide:
      return pointwise_invoke<Number, Pointwise::Divide>;
    case Pointwise::Clamp:
      return pointwise_invoke<Number, Pointwise::Clamp>;
    case Pointwise::Abs:
      return pointwise_invoke<Number, Pointwise::Abs>;
    case Pointwise::Minimum:
      return pointwise_invoke<Number, Pointwise::Minimum>;
    case Pointwise::Maximum:
      return pointwise_invoke<Number, Pointwise::Maximum>;
  }
  return nullptr;
}
inline OperationDefinition fixed_pointwise(const char* key, Pointwise kind) {
  OperationDefinition definition;
  definition.key = key;
  auto& traits = definition.traits;
  traits.input_count = pointwise_inputs(kind);
  traits.input_schema.resize(traits.input_count);
  for (auto& port : traits.input_schema)
    port.element_type_mask = 12;
  traits.outputs[0].key = "value";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  traits.requires_metadata_specialization = true;
  if (kind == Pointwise::Clamp) {
    const auto maximum = std::numeric_limits<double>::max();
    traits.parameter_schema = {
        {"min", OperationParameterType::Float64, true, true, -maximum, maximum},
        {"max", OperationParameterType::Float64, true, true, -maximum,
         maximum}};
  }
  definition.specialize_metadata = [kind](const auto& inputs,
                                          const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    using Answer = Result<std::vector<OperationOutputSpecialization>>;
    for (const auto& input : inputs) {
      auto status = tensor_ops::check_tensor(input);
      if (!status.ok())
        return Answer(status);
    }
    const auto& first = inputs[0].result_schema->tensors[0];
    const auto shape = first.sample_shape();
    for (const auto& input : inputs) {
      const auto& spec = input.result_schema->tensors[0];
      if (spec.sample_shape() != shape ||
          spec.descriptor.element_type != first.descriptor.element_type)
        return Answer({ErrorCode::TypeMismatch,
                       "pointwise inputs must share shape and dtype"});
    }
    if (kind == Pointwise::Clamp && std::get<double>(parameters.at("min")) >
                                        std::get<double>(parameters.at("max")))
      return Answer({ErrorCode::InvalidArgument, "clamp min exceeds max"});
    auto schema = tensor_ops::scalar_schema();
    schema.tensors[0].descriptor = {first.descriptor.element_type, shape};
    return Answer(std::vector<OperationOutputSpecialization>{
        {{std::make_shared<const SchemaTemplate>(std::move(schema))}, {}}});
  };
  definition.lower =
      [](const OperationLowering& query) -> Result<OperationProgram> {
    OperationProgramBuilder builder(query);
    auto output = TileGrid::from_schema(*query.outputs[0].result_schema,
                                        query.tile_extent);
    if (!output.ok())
      return Result<OperationProgram>(output.status());
    ResourceLimits limits;
    limits.maximum_work = query.limits.maximum_work;
    ResourceBudget root(limits);
    std::vector<TileGrid> inputs;
    for (const auto& input : query.inputs) {
      auto grid =
          TileGrid::from_schema(*input.result_schema, query.tile_extent);
      if (!grid.ok())
        return Result<OperationProgram>(grid.status());
      inputs.push_back(grid.take_value());
    }
    for (const auto& member : query.requests[0].members) {
      for (auto tile : member.second) {
        auto task = builder.add_task("pointwise", "finite-unchecked-v1");
        if (!task.ok())
          return Result<OperationProgram>(task.status());
        auto region = output.value().region(tile);
        if (!region.ok())
          return Result<OperationProgram>(region.status());
        for (unsigned port = 0; port < query.inputs.size(); ++port) {
          auto reads =
              sdk::tiles_for_region(root, inputs[port], region.value());
          if (!reads.ok())
            return Result<OperationProgram>(reads.status());
          for (auto index : reads.value()) {
            auto status = root.consume({1});
            if (status.ok())
              status = builder.read_input(task.value(), port, 0, index);
            if (!status.ok())
              return Result<OperationProgram>(status);
          }
        }
        auto status = builder.write_output(task.value(), 0, member.first, tile);
        if (!status.ok())
          return Result<OperationProgram>(status);
      }
    }
    return builder.finish();
  };
  definition.prepare_kernel =
      [kind](const physical::KernelDesc& kernel,
             const physical::Placement& placement,
             const KernelPreparation& query, const ResourceSet&,
             const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    using Answer = Result<ps_prepared_kernel_v8>;
    const auto invalid = [] {
      return Answer(
          {ErrorCode::TypeMismatch, "invalid pointwise kernel signature"});
    };
    if (kernel.symbol != "pointwise" ||
        kernel.contract != "finite-unchecked-v1" || query.writes.size() != 1 ||
        query.writes[0].port != 0 || query.writes[0].member != 0 ||
        !query.task_parameters.empty() ||
        query.scratch != std::array<std::uint64_t, 3>{})
      return invalid();
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Answer({ErrorCode::BackendUnavailable,
                     "pointwise requires CPU Host placement"});
    const auto& write = query.writes[0];
    auto state = std::make_unique<PointwiseState>();
    if (kind == Pointwise::Clamp) {
      state->minimum = std::get<double>(query.parameters.at("min"));
      state->maximum = std::get<double>(query.parameters.at("max"));
    }
    const auto region = fixed_tensor::view_region(write.layout);
    std::size_t slot = 0;
    for (unsigned port = 0; port < pointwise_inputs(kind); ++port) {
      auto grid = TileGrid::from_schema(*query.inputs[port].result_schema,
                                        query.tile_extent);
      if (!grid.ok())
        return Answer(grid.status());
      state->grids[port] = grid.take_value();
      auto tiles =
          sdk::tiles_for_region(ResourceBudget{}, state->grids[port], region);
      if (!tiles.ok())
        return Answer(tiles.status());
      if (tiles.value().size() > query.reads.size() - slot)
        return invalid();
      state->slots[port].reserve(tiles.value().size());
      for (auto index : tiles.value()) {
        const auto& read = query.reads[slot];
        if (read.port != port || read.member != 0 || read.tile_index != index ||
            read.domain != physical::Domain::Host ||
            read.layout.rank != write.layout.rank ||
            read.layout.element_type != write.layout.element_type)
          return invalid();
        auto expected = state->grids[port].region(index);
        if (!expected.ok())
          return Answer(expected.status());
        for (unsigned axis = 0; axis < read.layout.rank; ++axis)
          if (read.layout.origin[axis] !=
                  expected.value().dimensions()[axis].offset ||
              read.layout.shape[axis] !=
                  expected.value().dimensions()[axis].extent)
            return invalid();
        state->slots[port].push_back(
            {index, static_cast<std::uint32_t>(slot++)});
      }
    }
    if (slot != query.reads.size())
      return invalid();
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
    result.invoke_cpu = write.layout.element_type == ElementType::Float32
                            ? pointwise_entry<float>(kind)
                            : pointwise_entry<double>(kind);
    result.context = state.release();
    result.release = [](const void* context) {
      delete static_cast<const PointwiseState*>(context);
    };
    return Answer(result);
  };
  return definition;
}
}  // namespace ps::plugin_internal::numeric_ops
