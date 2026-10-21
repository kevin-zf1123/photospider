#pragma once

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
#include <vector>

#include "01-numeric/array_parameters.hpp"
#include "01-numeric/exact_curve.hpp"
#include "common/fixed_point_inputs.hpp"
#include "common/floating_environment.hpp"
#include "common/tensor_program.hpp"

namespace ps::plugin_internal::fixed_curve {
using numeric_ops::BinaryParts;
using numeric_ops::SequenceProfile;
inline Status shape_error(const char* message) {
  return {ErrorCode::TypeMismatch,
          message,
          FailureReason::None,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline Result<TensorDescriptor> metadata(
    bool multi, bool inverse, const std::vector<OperationMetadata>& inputs,
    const std::map<std::string, ParameterValue>& parameters) {
  using Answer = Result<TensorDescriptor>;
  if (inputs.size() != 3)
    return Answer(shape_error("curve requires x, y and query inputs"));
  for (const auto& input : inputs) {
    auto valid = tensor_ops::check_tensor(input);
    if (!valid.ok())
      return Answer(valid);
  }
  std::array<TensorDescriptor, 3> descriptors;
  for (unsigned i = 0; i < 3; ++i) {
    const auto& tensor = inputs[i].result_schema->tensors[0];
    descriptors[i] = {tensor.descriptor.element_type, tensor.sample_shape()};
    const auto& d = descriptors[i];
    if ((d.element_type != ElementType::Float32 &&
         d.element_type != ElementType::Float64) ||
        d.shape.size() != (multi && i == 1 ? 2U : 1U))
      return Answer(shape_error(
          "curve inputs require independent Float32/64 x,y,query ranks"));
    for (auto extent : d.shape)
      if (!extent || extent > (UINT64_C(1) << 40))
        return Answer(shape_error("curve extents require 1..2^40"));
  }
  if (inverse &&
      (descriptors[0].shape[0] < 2 || descriptors[0].shape[0] > 65536))
    return Answer(
        numeric_ops::array_parameter_error("inverse requires K=2..65536"));
  const auto knots = descriptors[0].shape[0];
  const auto count = descriptors[2].shape[0];
  const auto columns = multi ? descriptors[1].shape[1] : 1;
  if (knots < 2 || knots > 65536 || descriptors[1].shape[0] != knots ||
      columns > (UINT64_C(1) << 40) / knots ||
      columns > (UINT64_C(1) << 40) / count)
    return Answer(
        shape_error("curve requires matching K=2..65536 and products <=2^40"));
  const auto& dtype = std::get<std::string>(parameters.at("dtype"));
  const auto& policy = std::get<std::string>(parameters.at("out_of_domain"));
  if ((dtype != "float32" && dtype != "float64") ||
      (policy != "reject" && policy != "clamp" &&
       (inverse || policy != "linear_extrapolate")))
    return Answer(numeric_ops::array_parameter_error(
        "invalid curve dtype/domain policy"));
  std::vector<std::uint64_t> shape{count};
  if (multi)
    shape.push_back(columns);
  return Answer(TensorDescriptor{
      dtype == "float32" ? ElementType::Float32 : ElementType::Float64,
      std::move(shape)});
}
inline SchemaTemplate output_schema(const TensorDescriptor& descriptor) {
  auto schema = tensor_ops::scalar_schema();
  schema.tensors[0].descriptor = descriptor;
  return schema;
}
inline std::array<Region, 3> input_regions(bool multi, std::uint64_t knots,
                                           const Region& output) {
  const auto rows = output.dimensions()[0];
  std::array<Region, 3> regions;
  regions[0] = Region({{0, knots}});
  regions[1] =
      multi ? Region({{0, knots}, output.dimensions()[1]}) : regions[0];
  regions[2] = Region({rows});
  return regions;
}
inline Result<OperationProgram> lower_curve(bool multi, bool inverse,
                                            const OperationLowering& query) {
  using Answer = Result<OperationProgram>;
  auto descriptor = metadata(multi, inverse, query.inputs, query.parameters);
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
  const auto knots = inputs[0].shape()[0];
  ResourceLimits limits;
  limits.maximum_work = query.limits.maximum_work;
  ResourceBudget budget(limits);
  OperationProgramBuilder builder(query);
  for (const auto& member : query.requests[0].members)
    for (auto tile : member.second) {
      auto task = builder.add_task(inverse ? "invert" : "interpolate",
                                   inverse ? "exact-inverse-curve-unchecked-v1"
                                           : "exact-curve-unchecked-v1",
                                   {}, {sizeof(numeric_ops::ExactCurve), 0, 0});
      if (!task.ok())
        return Answer(task.status());
      auto region = output.value().region(tile);
      if (!region.ok())
        return Answer(region.status());
      const auto regions = input_regions(multi, knots, region.value());
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
struct CurveRow final {
  std::uint64_t bits = 0;
  unsigned first = 0, count = 0, segment = 0;
  int selected = -1;
};
struct PreparedCurve final {
  bool pchip, multi, inverse, narrow;
  SequenceProfile profile;
  unsigned knots, policy;
  std::array<bool, 3> input_narrow{};
  fixed_tensor::PointInputs<3> inputs;
};
inline void curve_invoke(const ps_kernel_invocation_v8* invocation,
                         ps_kernel_status_v8* status) noexcept {
  FloatingEnvironment environment;
  if (!environment.active()) {
    *status = kernel_failure(ErrorCode::OperationFailed,
                             "numeric environment unavailable");
    return;
  }
  try {
    const auto& state =
        *static_cast<const PreparedCurve*>(invocation->prepared_context);
    const std::function<Status(std::uint64_t)> consume =
        [invocation](std::uint64_t) {
          return invocation->cancellation.is_cancelled &&
                         invocation->cancellation.is_cancelled(
                             invocation->cancellation.context)
                     ? Status{ErrorCode::Cancelled, {}}
                     : Status::success();
        };
    const auto accept = [&](const Status& result) {
      if (!result.ok())
        *status =
            kernel_failure(result.code, result.message.c_str(), result.reason);
      return result.ok();
    };
    if (!accept(consume(1)))
      return;
    fixed_tensor::PointReader<3> reader{state.inputs, *invocation};
    const auto read = [&](unsigned port, std::uint64_t row,
                          std::uint64_t column = 0) {
      auto bits =
          reader.bits(port, {row, column}, state.input_narrow[port] ? 4 : 8);
      if (state.input_narrow[port]) {
        const auto raw = static_cast<std::uint32_t>(bits);
        float number;
        std::memcpy(&number, &raw, 4);
        const double wide = number;
        std::memcpy(&bits, &wide, 8);
      }
      return bits;
    };
    using Arithmetic = numeric_ops::ExactCurve;
    static_assert(alignof(Arithmetic) <= alignof(std::max_align_t));
    auto* arithmetic =
        new (invocation->scratch[0].allocation) Arithmetic(state.profile);
    const auto destroy = [](Arithmetic* value) { value->~Arithmetic(); };
    std::unique_ptr<Arithmetic, decltype(destroy)> guard(arithmetic, destroy);
    const auto& output = invocation->outputs[0];
    const auto begin = output.layout->origin[0];
    const auto end = begin + output.layout->shape[0];
    const auto column_begin = state.multi ? output.layout->origin[1] : 0;
    const auto column_end =
        state.multi ? column_begin + output.layout->shape[1] : 1;
    const unsigned search_port = state.inverse ? 1 : 0;
    const bool increasing =
        !state.inverse ||
        BinaryParts::decode(read(1, 0), false).order_key() <
            BinaryParts::decode(read(1, 1), false).order_key();
    const auto ordered = [increasing](std::uint64_t bits) {
      const auto key = BinaryParts::decode(bits, false).order_key();
      return increasing ? key : UINT64_MAX - key;
    };
    std::array<std::uint64_t, 4> x{}, y{}, replicas{};
    for (std::uint64_t index = begin; index < end; ++index) {
      if (!accept(consume(1)))
        return;
      CurveRow row;
      row.bits = read(2, index);
      const auto key = ordered(row.bits);
      unsigned lo = 0, hi = state.knots;
      while (lo < hi) {
        if (!accept(consume(1)))
          return;
        const auto mid = lo + (hi - lo) / 2;
        if (ordered(read(search_port, mid)) < key)
          lo = mid + 1;
        else
          hi = mid;
      }
      if (lo < state.knots && ordered(read(search_port, lo)) == key) {
        row.selected = static_cast<int>(lo);
      } else if (!lo || lo == state.knots) {
        // The reject domain is a caller precondition. Both endpoint segments
        // remain bounded even for an out-of-contract query.
        if (state.inverse || state.policy == 1)
          row.selected = lo ? static_cast<int>(state.knots - 1) : 0;
        row.segment = lo ? state.knots - 2 : 0;
      } else {
        row.segment = lo - 1;
      }
      if (row.selected >= 0) {
        row.first = static_cast<unsigned>(row.selected);
        row.count = 1;
      } else if (!state.pchip) {
        row.first = row.segment;
        row.count = 2;
      } else {
        row.first = row.segment ? row.segment - 1 : 0;
        row.count = std::min(state.knots, row.segment + 3) - row.first;
      }
      for (unsigned j = 0; j < row.count; ++j)
        x[j] = read(0, row.first + j);
      for (std::uint64_t column = column_begin; column < column_end; ++column) {
        if (!accept(consume(1)))
          return;
        for (unsigned j = 0; j < row.count; ++j)
          y[j] = read(1, row.first + j, column);
        auto value = state.inverse
                         ? arithmetic->inverse(
                               state.pchip, state.knots, row.first, row.count,
                               row.segment, row.selected, row.bits, x, y,
                               state.narrow, consume, {}, true)
                         : arithmetic->evaluate(
                               state.pchip, state.knots, row.first, row.count,
                               row.segment, row.selected, row.bits, x, y,
                               state.narrow, consume, {}, true);
        if (!accept(value.status()))
          return;
        numeric_ops::select_words(replicas.data(), value.value(), value.value(),
                                  1, state.profile);
        const auto offset =
            output.byte_offset + (index - begin) * output.layout->strides[0] +
            (state.multi ? (column - column_begin) * output.layout->strides[1]
                         : 0);
        std::memcpy(static_cast<std::uint8_t*>(output.allocation) + offset,
                    replicas.data(), state.narrow ? 4 : 8);
      }
    }
    *status = {};
  } catch (const std::bad_alloc&) {
    *status = kernel_failure(ErrorCode::ResourceExhausted);
  } catch (...) {
    *status =
        kernel_failure(ErrorCode::OperationFailed, "curve invocation failed");
  }
}
inline Result<ps_prepared_kernel_v8> prepare_curve(
    bool pchip, bool multi, bool inverse, SequenceProfile profile,
    const KernelPreparation& query) {
  using Answer = Result<ps_prepared_kernel_v8>;
  const auto invalid = [] {
    return Answer({ErrorCode::TypeMismatch, "invalid fixed curve invocation"});
  };
  if (query.outputs.size() != 1 || query.writes.size() != 1 ||
      !query.task_parameters.empty() ||
      query.scratch !=
          std::array<std::uint64_t, 3>{sizeof(numeric_ops::ExactCurve), 0, 0})
    return invalid();
  auto descriptor = metadata(multi, inverse, query.inputs, query.parameters);
  if (!descriptor.ok())
    return Answer(descriptor.status());
  const auto schema = output_schema(descriptor.value());
  if (!query.outputs[0].result_schema ||
      !query.outputs[0].result_schema->same_schema(schema))
    return invalid();
  auto grid = TileGrid::from_schema(schema, query.tile_extent);
  if (!grid.ok())
    return Answer(grid.status());
  auto output = grid.value().region(query.writes[0].tile_index);
  if (!output.ok())
    return Answer(output.status());
  auto state = std::make_unique<PreparedCurve>();
  state->pchip = pchip;
  state->multi = multi;
  state->inverse = inverse;
  state->profile = profile;
  state->narrow = descriptor.value().element_type == ElementType::Float32;
  state->knots = static_cast<unsigned>(
      query.inputs[0].result_schema->tensors[0].sample_shape()[0]);
  const auto& policy =
      std::get<std::string>(query.parameters.at("out_of_domain"));
  state->policy = policy == "reject" ? 0 : policy == "clamp" ? 1 : 2;
  for (unsigned port = 0; port < 3; ++port)
    state->input_narrow[port] =
        query.inputs[port].result_schema->tensors[0].descriptor.element_type ==
        ElementType::Float32;
  auto regions = input_regions(multi, state->knots, output.value());
  auto valid =
      fixed_tensor::prepare_point_inputs(query, &state->inputs, &regions);
  if (!valid.ok())
    return Answer(valid);
  ps_prepared_kernel_v8 kernel{};
  kernel.struct_size = sizeof(kernel);
  kernel.abi_version = PS_FIXED_INVOCATION_ABI_VERSION_8;
  kernel.context = state.release();
  kernel.invoke_cpu = curve_invoke;
  kernel.release = [](const void* context) {
    delete static_cast<const PreparedCurve*>(context);
  };
  return Answer(kernel);
}
inline OperationDefinition curve_operation(const std::string& key, bool pchip,
                                           bool multi, bool inverse,
                                           SequenceProfile profile) {
  OperationDefinition operation;
  operation.key = key;
  auto& traits = operation.traits;
  traits.parameter_schema = {{"dtype", OperationParameterType::String},
                             {"out_of_domain", OperationParameterType::String}};
  traits.input_count = 3;
  traits.input_schema.resize(3);
  traits.requires_metadata_specialization = true;
  for (auto& port : traits.input_schema) {
    port.element_type_mask = 12;
  }
  traits.outputs[0].key = "values";
  traits.outputs[0].output_schema = tensor_ops::port();
  traits.outputs[0].result_schema = tensor_ops::scalar_schema();
  operation.specialize_metadata =
      [multi, inverse, profile](const auto& inputs, const auto& parameters) {
        using Answer = Result<std::vector<OperationOutputSpecialization>>;
        auto descriptor = metadata(multi, inverse, inputs, parameters);
        if (!descriptor.ok())
          return Answer(descriptor.status());
        auto available = numeric_ops::sequence_profile_available(profile);
        if (!available.ok())
          return Answer(available);
        OperationOutputSpecialization resolved;
        resolved.metadata.result_schema =
            std::make_shared<const SchemaTemplate>(
                output_schema(descriptor.value()));
        return Answer(
            std::vector<OperationOutputSpecialization>{std::move(resolved)});
      };
  operation.lower = [multi, inverse](const OperationLowering& query) {
    return lower_curve(multi, inverse, query);
  };
  operation.prepare_kernel =
      [pchip, multi, inverse, profile](
          const physical::KernelDesc& kernel,
          const physical::Placement& placement, const KernelPreparation& query,
          const ResourceSet&,
          const ps_native_gpu_service_v2*) -> Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != (inverse ? "invert" : "interpolate") ||
        kernel.contract != (inverse ? "exact-inverse-curve-unchecked-v1"
                                    : "exact-curve-unchecked-v1"))
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::TypeMismatch, "invalid curve kernel contract"});
    if (placement.backend != physical::Backend::Cpu ||
        placement.domain != physical::Domain::Host)
      return Result<ps_prepared_kernel_v8>(
          {ErrorCode::BackendUnavailable, "curve requires CPU Host"});
    return prepare_curve(pchip, multi, inverse, profile, query);
  };
  return operation;
}
}  // namespace ps::plugin_internal::fixed_curve
