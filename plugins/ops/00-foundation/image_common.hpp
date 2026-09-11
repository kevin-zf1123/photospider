#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "00-foundation/numeric_common.hpp"
#include "data/input_validation.hpp"
#include "rgba32f/image_gpu.h"

namespace ps::plugin_internal::image_ops {
/** @brief Adapts built-in Values to the same pure C GPU operation
 * implementation. */
inline Result<Value> gpu_image(const OperationInvocation& call,
                               std::uint32_t kind) {
  struct Output {
    const OperationInvocation* call;
    ValueDescriptor descriptor;
    std::optional<MutableValue> value;
    std::vector<MutableBuffer> scratch;
    Result<Value> result{
        Status::failure(ErrorCode::OperationFailed, "no native output")};
    Status failure;
  } output{&call,
           call.inputs[0].descriptor(),
           {},
           {},
           Result<Value>(
               Status::failure(ErrorCode::OperationFailed, "no native output")),
           {}};
  if (kind == 5 || kind == 6) {
    auto factor = static_cast<std::uint64_t>(
        std::get<std::int64_t>(call.parameters.at("factor")));
    for (int axis = 0; axis < 2; ++axis)
      output.descriptor.shape[axis] =
          output.descriptor.shape[axis] / factor +
          (output.descriptor.shape[axis] % factor != 0);
  }
  const auto count = call.inputs.size();
  std::vector<ps_operation_value_view_v9> views(count);
  std::vector<std::vector<std::uint64_t>> origins(count), offsets(count),
      extents(count), demands(count), demand_extents(count);
  std::vector<std::vector<ps_operation_facet_view_v9>> facets(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& input = call.inputs[i];
    auto& v = views[i];
    v.struct_size = sizeof(v);
    v.element_type =
        static_cast<std::uint32_t>(input.descriptor().element_type);
    v.rank = input.descriptor().shape.size();
    v.shape = input.descriptor().shape.data();
    v.data = input.bytes().data();
    v.byte_size = input.bytes().size();
    v.byte_offset = input.layout().byte_offset;
    v.byte_strides = input.layout().byte_strides.data();
    origins[i] = input.layout().origin;
    if (origins[i].empty())
      origins[i].resize(v.rank, 0);
    v.storage_origin = origins[i].data();
    for (auto d : input.region().dimensions()) {
      offsets[i].push_back(d.offset);
      extents[i].push_back(d.extent);
    }
    for (auto d : call.input_demands[i].dimensions()) {
      demands[i].push_back(d.offset);
      demand_extents[i].push_back(d.extent);
    }
    v.region_offsets = offsets[i].data();
    v.region_extents = extents[i].data();
    v.demand_offsets = demands[i].data();
    v.demand_extents = demand_extents[i].data();
    for (const auto& f : input.facets())
      facets[i].push_back({sizeof(ps_operation_facet_view_v9), f.key.data(),
                           static_cast<std::uint32_t>(f.key.size()), f.version,
                           f.payload.data(),
                           static_cast<std::uint32_t>(f.payload.size())});
    v.facets = facets[i].data();
    v.facet_count = facets[i].size();
  }
  std::vector<ps_operation_parameter_value_v9> parameters;
  for (const auto& entry : call.parameters) {
    ps_operation_parameter_value_v9 p{};
    p.struct_size = sizeof(p);
    p.key = entry.first.data();
    p.key_size = entry.first.size();
    if (const auto* number = std::get_if<double>(&entry.second)) {
      p.type = PS_OPERATION_PARAMETER_FLOAT64_V9;
      p.float64_value = *number;
    } else {
      p.type = PS_OPERATION_PARAMETER_INT64_V9;
      p.int64_value = std::get<std::int64_t>(entry.second);
    }
    parameters.push_back(p);
  }
  std::vector<std::uint64_t> out_offsets, out_extents;
  for (auto d : call.output_region.dimensions()) {
    out_offsets.push_back(d.offset);
    out_extents.push_back(d.extent);
  }
  ps_operation_output_sink_v9 sink{};
  sink.struct_size = sizeof(sink);
  sink.context = &output;
  sink.output_rank = output.descriptor.shape.size();
  sink.output_shape = output.descriptor.shape.data();
  sink.output_offsets = out_offsets.data();
  sink.output_extents = out_extents.data();
  auto elements = call.output_region.element_count();
  if (!elements.ok() || elements.value() > UINT64_MAX / 4)
    return Result<Value>(Status::failure(ErrorCode::ResourceExhausted,
                                         "native output overflow"));
  sink.output_byte_size = elements.value() * 4;
  sink.gpu = call.gpu;
  sink.allocate_output = [](void* context) -> std::uint8_t* {
    auto& state = *static_cast<Output*>(context);
    if (!state.value) {
      auto value = MutableValue::allocate(
          state.descriptor, state.call->output_region, state.call->allocator);
      if (!value.ok()) {
        state.failure = value.status();
        return nullptr;
      }
      state.value.emplace(value.take_value());
    }
    return state.value->data();
  };
  sink.allocate_scratch = [](void* context,
                             std::uint64_t size) -> std::uint8_t* {
    auto& state = *static_cast<Output*>(context);
    auto buffer = state.call->allocator.allocate(size);
    if (!buffer.ok()) {
      state.failure = buffer.status();
      return nullptr;
    }
    state.scratch.push_back(buffer.take_value());
    return state.scratch.back().data();
  };
  sink.publish = [](void* context, std::uint32_t, const std::uint64_t*,
                    std::uint32_t, const ps_operation_facet_view_v9*,
                    std::uint32_t, const std::uint8_t* data, std::uint64_t) {
    auto& state = *static_cast<Output*>(context);
    if (!state.value || state.value->data() != data)
      return 0;
    state.result =
        std::move(*state.value).publish(state.call->inputs[0].facets());
    return state.result.ok() ? 1 : 0;
  };
  auto cancelled = [](void* context) {
    return static_cast<const CancellationToken*>(context)->cancelled() ? 1 : 0;
  };
  const int result = ps_execute_gpu_image(
      kind, views.data(), views.size(), parameters.data(), parameters.size(),
      cancelled, const_cast<CancellationToken*>(&call.cancellation), &sink);
  if (!output.failure.ok())
    return Result<Value>(output.failure);
  if (result == PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V9)
    return Result<Value>(
        Status::failure(ErrorCode::BackendUnavailable,
                        "Metal FP32 numeric or shape eligibility"));
  if (result == PS_OPERATION_RESULT_CANCELLED_V9)
    return Result<Value>(
        Status::failure(ErrorCode::Cancelled, "native image cancelled"));
  return std::move(output.result);
}
/** @brief Executes one dense CPU execute_image; host enforces the port
 * contract. */

// Validated region coverage makes every sample address representable.
inline float sample(const Value& value, std::uint64_t y, std::uint64_t x,
                    std::uint64_t channel = 0) {
  auto coordinate = value.descriptor().shape.size() == 2
                        ? std::vector<std::uint64_t>{y, x}
                        : std::vector<std::uint64_t>{y, x, channel};
  const auto address = value.byte_address(coordinate);
  if (!address.ok())
    throw std::runtime_error(address.status().message);
  float number = 0;
  std::memcpy(&number, value.bytes().data() + address.value(), sizeof(number));
  return number;
}
inline std::uint64_t clamp_axis(std::uint64_t coordinate, int tap,
                                std::uint64_t length) {
  if (tap < 0)
    return coordinate < static_cast<std::uint64_t>(-tap)
               ? 0
               : coordinate - static_cast<std::uint64_t>(-tap);
  const auto room = length - 1 - coordinate;
  return coordinate + std::min(static_cast<std::uint64_t>(tap), room);
}

/** @brief Averages clipped integer boxes in fixed row/column sample order. */

/** @brief Applies one hard circular stamp; dynamic scalar inputs are immutable.
 */

}  // namespace ps::plugin_internal::image_ops
