#include "plugin/image_operations.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "rgba32f/image_gpu.h"

namespace ps::plugin_internal {
namespace {
/** @brief Adapts built-in Values to the same pure C GPU operation
 * implementation. */
Result<Value> gpu_image(const OperationInvocation& call, std::uint32_t kind) {
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
  std::vector<ps_operation_value_view_v7> views(count);
  std::vector<std::vector<std::uint64_t>> origins(count), offsets(count),
      extents(count), demands(count), demand_extents(count);
  std::vector<std::vector<ps_operation_facet_view_v7>> facets(count);
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
      facets[i].push_back({sizeof(ps_operation_facet_view_v7), f.key.data(),
                           static_cast<std::uint32_t>(f.key.size()), f.version,
                           f.payload.data(),
                           static_cast<std::uint32_t>(f.payload.size())});
    v.facets = facets[i].data();
    v.facet_count = facets[i].size();
  }
  std::vector<ps_operation_parameter_value_v7> parameters;
  for (const auto& entry : call.parameters) {
    ps_operation_parameter_value_v7 p{};
    p.struct_size = sizeof(p);
    p.key = entry.first.data();
    p.key_size = entry.first.size();
    if (const auto* number = std::get_if<double>(&entry.second)) {
      p.type = PS_OPERATION_PARAMETER_FLOAT64_V7;
      p.float64_value = *number;
    } else {
      p.type = PS_OPERATION_PARAMETER_INT64_V7;
      p.int64_value = std::get<std::int64_t>(entry.second);
    }
    parameters.push_back(p);
  }
  std::vector<std::uint64_t> out_offsets, out_extents;
  for (auto d : call.output_region.dimensions()) {
    out_offsets.push_back(d.offset);
    out_extents.push_back(d.extent);
  }
  ps_operation_output_sink_v7 sink{};
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
                    std::uint32_t, const ps_operation_facet_view_v7*,
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
  if (result == PS_OPERATION_RESULT_BACKEND_UNAVAILABLE_V7)
    return Result<Value>(
        Status::failure(ErrorCode::BackendUnavailable,
                        "Metal FP32 numeric or shape eligibility"));
  if (result == PS_OPERATION_RESULT_CANCELLED_V7)
    return Result<Value>(
        Status::failure(ErrorCode::Cancelled, "native image cancelled"));
  return std::move(output.result);
}
/** @brief Executes one dense CPU execute_image; host enforces the port
 * contract. */
Result<Value> execute_image(const OperationInvocation& invocation,
                            bool opacity) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(invocation, opacity ? 1 : 0);
  input_internal::Float32Environment environment;
  if (!environment.active()) {
    return Result<Value>(Status::failure(
        ErrorCode::OperationFailed,
        "image multiplication requires round-to-nearest ties-to-even"));
  }
  const Value& input = invocation.inputs[0];
  float factor = 0;
  std::memcpy(&factor,
              invocation.inputs[1].bytes().data() +
                  invocation.inputs[1].byte_address({0}).value(),
              sizeof(factor));
  auto allocated = MutableValue::allocate(
      input.descriptor(), invocation.output_region, invocation.allocator);
  if (!allocated.ok())
    return Result<Value>(allocated.status());
  auto output = allocated.take_value();
  const auto yd = invocation.output_region.dimensions()[0];
  const auto xd = invocation.output_region.dimensions()[1];
  std::size_t destination = 0;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "image operation cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      for (std::uint64_t channel = 0; channel < 4; ++channel) {
        const auto offset = input.byte_address({y, x, channel});
        if (!offset.ok())
          return Result<Value>(offset.status());
        float number = 0;
        std::memcpy(&number, input.bytes().data() + offset.value(),
                    sizeof(number));
        if (opacity || channel < 3)
          number *= factor;
        std::memcpy(output.data() + destination, &number, sizeof(number));
        destination += sizeof(number);
      }
    }
  }
  return std::move(output).publish(invocation.inputs[0].facets());
}
// Validated region coverage makes every sample address representable.
float sample(const Value& value, std::uint64_t y, std::uint64_t x,
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
std::uint64_t clamp_axis(std::uint64_t coordinate, int tap,
                         std::uint64_t length) {
  if (tap < 0)
    return coordinate < static_cast<std::uint64_t>(-tap)
               ? 0
               : coordinate - static_cast<std::uint64_t>(-tap);
  const auto room = length - 1 - coordinate;
  return coordinate + std::min(static_cast<std::uint64_t>(tap), room);
}
Result<Value> gaussian(const OperationInvocation& invocation) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(invocation, 2);
  const auto& input = invocation.inputs[0];
  const auto radius = static_cast<int>(
      std::get<std::int64_t>(invocation.parameters.at("radius")));
  const auto sigma = std::get<double>(invocation.parameters.at("sigma"));
  auto made = MutableValue::allocate(
      input.descriptor(), invocation.output_region, invocation.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  auto weights_result = invocation.allocator.allocate((2 * radius + 1) * 8);
  if (!weights_result.ok())
    return Result<Value>(weights_result.status());
  auto weights = weights_result.take_value();
  double total = 0;
  for (int tap = -radius; tap <= radius; ++tap) {
    const double weight =
        std::exp(-static_cast<double>(tap * tap) / (2.0 * sigma * sigma));
    total += weight;
    std::memcpy(weights.data() + (tap + radius) * 8, &weight, 8);
  }
  for (int tap = -radius; tap <= radius; ++tap) {
    double weight = 0;
    std::memcpy(&weight, weights.data() + (tap + radius) * 8, 8);
    weight /= total;
    std::memcpy(weights.data() + (tap + radius) * 8, &weight, 8);
  }
  const auto yd = invocation.output_region.dimensions()[0];
  const auto xd = invocation.output_region.dimensions()[1];
  const auto rows = invocation.input_demands[0].dimensions()[0];
  auto scratch_result =
      invocation.allocator.allocate(rows.extent * xd.extent * 16);
  if (!scratch_result.ok())
    return Result<Value>(scratch_result.status());
  auto scratch = scratch_result.take_value();
  for (std::uint64_t y = rows.offset; y < rows.offset + rows.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "Gaussian cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x)
      for (std::uint64_t c = 0; c < 4; ++c) {
        double sum = 0;
        for (int tap = -radius; tap <= radius; ++tap) {
          double weight = 0;
          std::memcpy(&weight, weights.data() + (tap + radius) * 8, 8);
          const double product =
              sample(input, y, clamp_axis(x, tap, input.descriptor().shape[1]),
                     c) *
              weight;
          sum += product;
        }
        const float rounded = static_cast<float>(sum);
        const auto index =
            ((y - rows.offset) * xd.extent + x - xd.offset) * 4 + c;
        std::memcpy(scratch.data() + index * 4, &rounded, 4);
      }
  }
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "Gaussian cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x)
      for (std::uint64_t c = 0; c < 4; ++c) {
        double sum = 0;
        for (int tap = -radius; tap <= radius; ++tap) {
          double weight = 0;
          std::memcpy(&weight, weights.data() + (tap + radius) * 8, 8);
          const auto row = clamp_axis(y, tap, input.descriptor().shape[0]);
          const auto index =
              ((row - rows.offset) * xd.extent + x - xd.offset) * 4 + c;
          float number = 0;
          std::memcpy(&number, scratch.data() + index * 4, 4);
          const double product = number * weight;
          sum += product;
        }
        const float rounded = static_cast<float>(sum);
        const auto index =
            ((y - yd.offset) * xd.extent + x - xd.offset) * 4 + c;
        std::memcpy(output.data() + index * 4, &rounded, 4);
      }
  }
  return std::move(output).publish(invocation.inputs[0].facets());
}
Result<Value> combine(const OperationInvocation& invocation, bool mask) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(invocation, mask ? 3 : 4);
  const auto& foreground = invocation.inputs[0];
  const auto& other = invocation.inputs[1];
  auto made = MutableValue::allocate(
      foreground.descriptor(), invocation.output_region, invocation.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const auto yd = invocation.output_region.dimensions()[0];
  const auto xd = invocation.output_region.dimensions()[1];
  std::size_t target = 0;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "image combine cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      const float factor =
          mask ? sample(other, y, x) : 1.0F - sample(foreground, y, x, 3);
      for (std::uint64_t c = 0; c < 4; ++c) {
        const float attenuated =
            (mask ? sample(foreground, y, x, c) : sample(other, y, x, c)) *
            factor;
        const float number =
            mask ? attenuated : sample(foreground, y, x, c) + attenuated;
        std::memcpy(output.data() + target, &number, 4);
        target += 4;
      }
    }
  }
  return std::move(output).publish(invocation.inputs[0].facets());
}
/** @brief Averages clipped integer boxes in fixed row/column sample order. */
Result<Value> downsample(const OperationInvocation& invocation) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(
        invocation,
        invocation.inputs[0].descriptor().shape.size() == 3 ? 5 : 6);
  const auto& input = invocation.inputs[0];
  const auto factor = static_cast<std::uint64_t>(
      std::get<std::int64_t>(invocation.parameters.at("factor")));
  auto descriptor = input.descriptor();
  for (std::size_t a = 0; a < 2; ++a)
    descriptor.shape[a] =
        descriptor.shape[a] / factor + (descriptor.shape[a] % factor != 0);
  auto made = MutableValue::allocate(descriptor, invocation.output_region,
                                     invocation.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const auto yd = invocation.output_region.dimensions()[0],
             xd = invocation.output_region.dimensions()[1];
  const std::uint64_t channels = descriptor.shape.size() == 3 ? 4 : 1;
  std::size_t target = 0;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "downsample cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      const auto y0 = y * factor, x0 = x * factor;
      const auto h = std::min(factor, input.descriptor().shape[0] - y0),
                 w = std::min(factor, input.descriptor().shape[1] - x0);
      for (std::uint64_t c = 0; c < channels; ++c) {
        double sum = 0;
        for (std::uint64_t row = y0; row < y0 + h; ++row)
          for (std::uint64_t col = x0; col < x0 + w; ++col)
            sum += sample(input, row, col, c);
        const float number =
            static_cast<float>(sum / static_cast<double>(h * w));
        std::memcpy(output.data() + target, &number, 4);
        target += 4;
      }
    }
  }
  return std::move(output).publish(input.facets());
}
/** @brief Applies one hard circular stamp; dynamic scalar inputs are immutable.
 */
Result<Value> brush_circle(const OperationInvocation& invocation) {
  if (invocation.backend == Backend::Gpu)
    return gpu_image(invocation, 7);
  const auto& input = invocation.inputs[0];
  float args[7];
  for (std::size_t i = 0; i < 7; ++i)
    std::memcpy(&args[i],
                invocation.inputs[i + 1].bytes().data() +
                    invocation.inputs[i + 1].byte_address({0}).value(),
                4);
  const double cx = args[0], cy = args[1], radius = args[2];
  const float alpha = args[6], remaining = 1.0F - alpha;
  auto made = MutableValue::allocate(
      input.descriptor(), invocation.output_region, invocation.allocator);
  if (!made.ok())
    return Result<Value>(made.status());
  auto output = made.take_value();
  const auto yd = invocation.output_region.dimensions()[0],
             xd = invocation.output_region.dimensions()[1];
  std::size_t target = 0;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y) {
    if (invocation.cancellation.cancelled())
      return Result<Value>(
          Status::failure(ErrorCode::Cancelled, "brush cancelled"));
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x) {
      const double dx = static_cast<double>(x) + .5 - cx,
                   dy = static_cast<double>(y) + .5 - cy;
      const bool inside = dx * dx + dy * dy <= radius * radius;
      for (std::uint64_t c = 0; c < 4; ++c) {
        float number = sample(input, y, x, c);
        if (inside) {
          const float source = c == 3 ? alpha : args[3 + c] * alpha;
          const float back = number * remaining;
          number = source + back;
        }
        std::memcpy(output.data() + target, &number, 4);
        target += 4;
      }
    }
  }
  return std::move(output).publish(input.facets());
}
}  // namespace

Status register_image_operations(OperationRegistry* registry) {
  for (bool opacity : {false, true}) {
    OperationDefinition operation;
    operation.traits.output_semantic_rule =
        OperationSemanticRule::PreserveInput;
    operation.traits.supports_gpu = operation.traits.allows_cpu_fallback = true;
    operation.key = opacity ? "image.opacity" : "image.exposure_gain";
    operation.traits.input_count = 2;
    operation.traits.output_element_type = ElementType::Float32;
    operation.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
    operation.traits.region_rule = OperationRegionRule::Elementwise;
    operation.traits.input_schema = {
        {OperationPortKind::RgbaFloat32, 0, 0},
        {OperationPortKind::Float32Scalar, 0, opacity ? 1.0F : 16.0F}};
    operation.traits.output_schema = operation.traits.input_schema.front();
    operation.callback = [opacity](const OperationInvocation& invocation) {
      return execute_image(invocation, opacity);
    };
    const auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  for (int kind = 0; kind < 3; ++kind) {
    OperationDefinition operation;
    operation.traits.output_semantic_rule =
        OperationSemanticRule::PreserveInput;
    operation.traits.supports_gpu = operation.traits.allows_cpu_fallback = true;
    operation.key = kind == 0   ? "image.gaussian_blur"
                    : kind == 1 ? "image.mask"
                                : "image.source_over";
    operation.traits.input_count = kind == 0 ? 1 : 2;
    operation.traits.output_element_type = ElementType::Float32;
    operation.traits.shape_rule = kind == 2
                                      ? OperationShapeRule::MatchAllInputs
                                      : OperationShapeRule::PreserveFirstInput;
    operation.traits.region_rule = kind == 0 ? OperationRegionRule::Halo
                                             : OperationRegionRule::Elementwise;
    operation.traits.input_schema = {{OperationPortKind::RgbaFloat32, 0, 0}};
    operation.traits.output_schema = operation.traits.input_schema.front();
    if (kind == 0) {
      operation.traits.parameter_schema = {
          {"radius", OperationParameterType::Int64, true, true, 1, 64},
          {"sigma", OperationParameterType::Float64, true, true, 0.1, 64}};
      operation.traits.halo_radius_parameter = "radius";
      operation.traits.workspace_bytes = 129 * 8;
      operation.traits.workspace_input_multiplier = 1;
      operation.callback = gaussian;
    } else {
      operation.traits.input_schema.push_back(
          {kind == 1 ? OperationPortKind::Float32Mask
                     : OperationPortKind::RgbaFloat32,
           0, 0});
      operation.callback = [kind](const OperationInvocation& invocation) {
        return combine(invocation, kind == 1);
      };
    }
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  for (bool mask : {false, true}) {
    OperationDefinition operation;
    operation.traits.output_semantic_rule =
        OperationSemanticRule::PreserveInput;
    operation.traits.supports_gpu = operation.traits.allows_cpu_fallback = true;
    operation.key = mask ? "mask.downsample_box" : "image.downsample_box";
    auto& traits = operation.traits;
    traits.input_count = 1;
    traits.output_element_type = ElementType::Float32;
    traits.shape_rule = OperationShapeRule::Shrink;
    traits.region_rule = OperationRegionRule::Shrink;
    traits.spatial_factor_parameter = "factor";
    traits.parameter_schema = {
        {"factor", OperationParameterType::Int64, true, true, 1, 16}};
    traits.output_schema.kind =
        mask ? OperationPortKind::Float32Mask : OperationPortKind::RgbaFloat32;
    traits.input_schema = {traits.output_schema};
    operation.callback = downsample;
    auto status = registry->register_operation(std::move(operation));
    if (!status.ok())
      return status;
  }
  OperationDefinition brush;
  brush.key = "image.brush_circle";
  brush.traits.output_semantic_rule = OperationSemanticRule::PreserveInput;
  brush.callback = brush_circle;
  auto& traits = brush.traits;
  traits.supports_gpu = traits.allows_cpu_fallback = true;
  traits.workspace_input_multiplier = 1;
  traits.input_count = 8;
  traits.output_element_type = ElementType::Float32;
  traits.shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.region_rule = OperationRegionRule::Elementwise;
  traits.output_schema.kind = OperationPortKind::RgbaFloat32;
  const float maximum = std::numeric_limits<float>::max();
  traits.input_schema = {traits.output_schema,
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar,
                          std::numeric_limits<float>::min(), maximum},
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar, -maximum, maximum},
                         {OperationPortKind::Float32Scalar, 0, 1}};
  auto status = registry->register_operation(std::move(brush));
  if (!status.ok())
    return status;
  return Status::success();
}
}  // namespace ps::plugin_internal
