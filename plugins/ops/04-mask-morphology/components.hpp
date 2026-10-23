#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "04-mask-morphology/spatial.hpp"

namespace ps::plugin_internal::mask {
constexpr std::uint64_t kUnvisitedComponent = UINT64_MAX;
struct ComponentBounds final {
  std::uint64_t first, area, y0, x0, y1, x1;
};
inline bool component_diagonal(const Parameters& p, const char* key) {
  const auto value = integer(p, key);
  parameter(value == 4 || value == 8, "component connectivity");
  return value == 8;
}
inline const ResultTensorSpec& label_image(const OperationMetadata& input) {
  const auto& result = tensor(input);
  require(result.descriptor.element_type == ElementType::Int64 &&
              result.sample_shape().size() == 2,
          "component labels require Int64[H,W]");
  spatial_axes(result, 2);
  return result;
}
// Resolve the fixed source view once per bounded row span, including strided
// layouts. Each callback accesses at most 256 samples inside that view.
template <class Function>
void component_read_rows(const Reader& reader, unsigned port, unsigned field,
                         std::uint64_t height, std::uint64_t width,
                         const Work& work, Function function) {
  for (std::uint64_t y = 0; y < height; ++y) {
    for (std::uint64_t x = 0; x < width;) {
      const Coordinate at{y, x};
      const auto& view = reader.view(port, field, at);
      const auto count =
          std::min({UINT64_C(256), width - x,
                    view.layout->origin[1] + view.layout->shape[1] - x});
      work.poll(count);
      const auto* data = static_cast<const std::uint8_t*>(view.allocation) +
                         offset(*view.layout, view.byte_offset, at);
      function(data, view.layout->strides[1], y, x, count);
      x += count;
    }
  }
}
template <class T>
void component_binary_span(const void* source, std::ptrdiff_t stride,
                           std::uint64_t* target, std::uint64_t count,
                           bool foreground) {
  std::uint64_t i = 0;
#if defined(__aarch64__) && defined(__ARM_NEON)
  if (stride == sizeof(T)) {
    const auto* data = static_cast<const T*>(source);
    if constexpr (std::is_same_v<T, float>) {
      for (; i + 4 <= count; i += 4) {
        auto zero = vceqq_f32(vld1q_f32(data + i), vdupq_n_f32(0));
        if (foreground)
          zero = vmvnq_u32(zero);
        vst1q_u64(target + i, vreinterpretq_u64_s64(vmovl_s32(
                                  vreinterpret_s32_u32(vget_low_u32(zero)))));
        vst1q_u64(target + i + 2,
                  vreinterpretq_u64_s64(
                      vmovl_s32(vreinterpret_s32_u32(vget_high_u32(zero)))));
      }
    } else {
      for (; i + 2 <= count; i += 2) {
        auto zero = vceqq_f64(vld1q_f64(data + i), vdupq_n_f64(0));
        if (foreground)
          zero = veorq_u64(zero, vdupq_n_u64(UINT64_MAX));
        vst1q_u64(target + i, zero);
      }
    }
  }
#endif
  for (; i < count; ++i)
    target[i] = ((load<T>(source, stride, i) != 0) == foreground)
                    ? kUnvisitedComponent
                    : 0;
}
inline void component_input(const Reader& reader, std::uint64_t* labels,
                            std::uint64_t height, std::uint64_t width,
                            bool foreground, Visits* visits) {
  const bool narrow = reader.kernel.groups[reader.kernel.input_base[0]].type ==
                      ElementType::Float32;
  component_read_rows(
      reader, 0, 0, height, width, visits->work,
      [&](const void* data, std::ptrdiff_t stride, std::uint64_t y,
          std::uint64_t x, std::uint64_t count) {
        auto* target = labels + y * width + x;
        if (narrow)
          component_binary_span<float>(data, stride, target, count, foreground);
        else
          component_binary_span<double>(data, stride, target, count,
                                        foreground);
      });
}
template <class T, class Function>
void component_write_image(const ps_kernel_mutable_view_v8& out,
                           std::uint64_t width, const Work& work,
                           Function function) {
  const auto y0 = out.layout->origin[0], x0 = out.layout->origin[1];
  const auto height = out.layout->shape[0], columns = out.layout->shape[1];
  for (std::uint64_t y = y0; y < y0 + height; ++y) {
    for (std::uint64_t x = x0; x < x0 + columns;) {
      const auto count = std::min(UINT64_C(256), x0 + columns - x);
      work.poll(count);
      auto* data = static_cast<std::uint8_t*>(out.allocation) +
                   offset(*out.layout, out.byte_offset, {y, x});
      for (std::uint64_t j = 0; j < count; ++j)
        save<T>(data, out.layout->strides[1], j,
                static_cast<T>(function(y * width + x + j)));
      x += count;
    }
  }
}
// Labels and the queue are admitted scratch. Unvisited markers are produced
// by component_input, never interpreted as an input-domain validation pass.
template <class Function>
std::uint64_t component_scan(std::uint64_t* labels, std::uint64_t* queue,
                             std::uint64_t height, std::uint64_t width,
                             bool diagonal, bool compact,
                             std::uint64_t maximum_count, Visits* visits,
                             Function finished) {
  const auto count = height * width;
  std::uint64_t components = 0;
  for (std::uint64_t first = 0; first < count; ++first) {
    visits->step();
    if (labels[first] != kUnvisitedComponent)
      continue;
    if (components == maximum_count)
      throw Status{ErrorCode::ResourceExhausted, "component output capacity",
                   FailureReason::CapacityLimit};
    const auto label = compact ? components + 1 : first + 1;
    ComponentBounds bounds{first, 0, first / width, first % width, 0, 0};
    std::uint64_t head = 0, tail = 1;
    queue[0] = first;
    labels[first] = label;
    const auto enqueue = [&](std::uint64_t target) {
      visits->step();
      if (labels[target] == kUnvisitedComponent) {
        labels[target] = label;
        queue[tail++] = target;
      }
    };
    while (head < tail) {
      const auto pixel = queue[head++];
      const auto y = pixel / width, x = pixel % width;
      bounds.y0 = std::min(bounds.y0, y);
      bounds.x0 = std::min(bounds.x0, x);
      bounds.y1 = std::max(bounds.y1, y + 1);
      bounds.x1 = std::max(bounds.x1, x + 1);
      visits->step();
      if (x)
        enqueue(pixel - 1);
      if (x + 1 < width)
        enqueue(pixel + 1);
      if (y) {
        enqueue(pixel - width);
        if (diagonal && x)
          enqueue(pixel - width - 1);
        if (diagonal && x + 1 < width)
          enqueue(pixel - width + 1);
      }
      if (y + 1 < height) {
        enqueue(pixel + width);
        if (diagonal && x)
          enqueue(pixel + width - 1);
        if (diagonal && x + 1 < width)
          enqueue(pixel + width + 1);
      }
    }
    bounds.area = tail;
    finished(components, label, bounds, queue);
    ++components;
  }
  return components;
}
}  // namespace ps::plugin_internal::mask
