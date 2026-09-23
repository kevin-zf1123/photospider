#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::execution_internal {
inline bool channel_assembly(const std::string& key) {
  return key.compare(0, 17, "channel.assemble_") == 0 ||
         key.compare(0, 20, "channel.concatenate_") == 0;
}
// The caller owns exact preparation/publication. Rectangles stop at both
// physical tile edges and authorized coverage; padding is never copied.
inline Status copy_channel_piece(
    const Region& region, const DependencyMappedNeed& map,
    const PlanarImageReadWindow* image, const Value* value,
    const PlanarImageWriteWindow& output, const PlanarImageLayout& layout,
    std::uint64_t width, const CancellationToken& cancellation,
    const std::function<bool()>& current = {}) {
  std::vector<std::uint64_t> at, source(map.axes.size());
  for (const auto& dim : region.dimensions())
    at.push_back(dim.offset);
  const auto y = region.dimensions()[layout.height_axis];
  const auto x = region.dimensions()[layout.width_axis];
  const auto channels = region.dimensions()[*layout.channel_axis];
  std::uint64_t since_current_check = 1024;
  std::size_t source_x = 0;
  for (std::size_t d = 0; d < map.axes.size(); ++d)
    if (map.axes[d].observation_axis ==
        static_cast<std::int32_t>(layout.width_axis))
      source_x = d;
  for (auto c = channels.offset; c < channels.offset + channels.extent; ++c) {
    at[*layout.channel_axis] = c;
    for (auto row = y.offset; row < y.offset + y.extent;) {
      at[layout.height_axis] = row;
      auto rows = y.offset + y.extent - row;
      for (auto column = x.offset; column < x.offset + x.extent;) {
        if (cancellation.cancelled())
          return {ErrorCode::Cancelled, "channel assembly cancelled"};
        at[layout.width_axis] = column;
        for (std::size_t d = 0; d < source.size(); ++d) {
          const auto& axis = map.axes[d];
          source[d] =
              axis.observation_axis < 0
                  ? axis.fixed.offset
                  : static_cast<std::uint64_t>(
                        static_cast<__int128>(at[axis.observation_axis]) +
                        axis.translation);
        }
        auto write = output.rectangle_run(at);
        if (!write.ok())
          return write.status();
        const auto& destination = write.value();
        rows = std::min(rows, destination.rows);
        auto samples =
            std::min(x.offset + x.extent - column, destination.row.samples);
        const std::uint8_t* input = nullptr;
        std::uint64_t input_row_stride = 0;
        std::int64_t input_x_stride = width;
        if (image) {
          auto read = image->rectangle_run(source);
          if (!read.ok())
            return read.status();
          rows = std::min(rows, read.value().rows);
          samples = std::min(samples, read.value().row.samples);
          input = read.value().row.data;
          input_row_stride = read.value().row_stride_bytes;
        } else {
          // Generic layouts may have negative/zero strides. One row is enough
          // to preserve bounds while coalescing its truly contiguous X runs.
          rows = 1;
          auto address = value->byte_address(source);
          if (!address.ok())
            return address.status();
          input = value->bytes().data() + address.value();
          input_x_stride = value->layout().byte_strides[source_x];
        }
        for (std::uint64_t dy = 0; dy < rows; ++dy) {
          for (std::uint64_t dx = 0; dx < samples;) {
            if (cancellation.cancelled())
              return {ErrorCode::Cancelled, "channel assembly cancelled"};
            if (current && since_current_check >= 1024) {
              if (!current())
                return {ErrorCode::Stale, "channel assembly plan changed"};
              since_current_check = 0;
            }
            const auto count = std::min<std::uint64_t>(
                1024 - (since_current_check % 1024), samples - dx);
            since_current_check += count;
            auto* to = destination.row.data +
                       dy * destination.row_stride_bytes + dx * width;
            const auto* from = input + dy * input_row_stride +
                               static_cast<std::int64_t>(dx) * input_x_stride;
            if (input_x_stride == static_cast<std::int64_t>(width)) {
              std::memcpy(to, from, count * width);
            } else {
              for (std::uint64_t i = 0; i < count; ++i)
                std::memcpy(
                    to + i * width,
                    from + static_cast<std::int64_t>(i) * input_x_stride,
                    width);
            }
            dx += count;
          }
        }
        column += samples;
      }
      row += rows;
    }
  }
  return Status::success();
}
}  // namespace ps::execution_internal
