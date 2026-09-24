#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "photospider/plugin/operation_registry.hpp"

namespace ps::execution_internal {
inline bool planar_mapped_copy(const std::string& key) {
  return key.compare(0, 17, "channel.assemble_") == 0 ||
         key.compare(0, 20, "channel.concatenate_") == 0 ||
         key.compare(0, 16, "metadata.assign_") == 0;
}
// The caller owns exact preparation/publication. Rectangles stop at both
// physical tile edges and authorized coverage; padding is never copied.
inline Status copy_spatial_channel_piece(
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
  const auto channels = layout.channel_axis
                            ? region.dimensions()[*layout.channel_axis]
                            : RegionDimension{0, 1};
  std::uint64_t since_current_check = 1024;
  std::size_t source_x = 0;
  for (std::size_t d = 0; d < map.axes.size(); ++d)
    if (map.axes[d].observation_axis ==
        static_cast<std::int32_t>(layout.width_axis))
      source_x = d;
  for (auto c = channels.offset; c < channels.offset + channels.extent; ++c) {
    if (layout.channel_axis)
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
        // A full-width rectangle with matching row strides has no padding or
        // tile gap. Copy across its row boundaries in bounded blocks, retaining
        // the same 1024-sample stop/currentness interval as the row path.
        if (layout.order == ImagePlaneOrder::Tiled && rows > 1 &&
            input_x_stride == static_cast<std::int64_t>(width) &&
            input_row_stride == samples * width &&
            destination.row_stride_bytes == samples * width) {
          const auto total = rows * samples;
          for (std::uint64_t offset = 0; offset < total;) {
            if (cancellation.cancelled())
              return {ErrorCode::Cancelled, "channel copy cancelled"};
            if (current && since_current_check >= 1024) {
              if (!current())
                return {ErrorCode::Stale, "channel copy plan changed"};
              since_current_check = 0;
            }
            const auto count = std::min<std::uint64_t>(
                1024 - (since_current_check % 1024), total - offset);
            since_current_check += count;
            std::memcpy(destination.row.data + offset * width,
                        input + offset * width, count * width);
            offset += count;
          }
        } else {
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
        }
        column += samples;
      }
      row += rows;
    }
  }
  return Status::success();
}

// Scalar broadcast has no spatial source stride. Keep its fill traversal
// separate so spatial copies retain the established FMT-02 hot loop.
inline Status fill_scalar_channel_piece(
    const Region& region, const DependencyMappedNeed& map, const Value& value,
    const PlanarImageWriteWindow& output, const PlanarImageLayout& layout,
    std::uint64_t width, const CancellationToken& cancellation,
    const std::function<bool()>& current) {
  auto address = value.byte_address({map.axes[0].fixed.offset});
  if (!address.ok())
    return address.status();
  const auto* scalar = value.bytes().data() + address.value();
  std::vector<std::uint64_t> at;
  for (const auto& dim : region.dimensions())
    at.push_back(dim.offset);
  const auto y = region.dimensions()[layout.height_axis];
  const auto x = region.dimensions()[layout.width_axis];
  const auto channels = layout.channel_axis
                            ? region.dimensions()[*layout.channel_axis]
                            : RegionDimension{0, 1};
  std::uint64_t since_check = 1024;
  for (auto c = channels.offset; c < channels.offset + channels.extent; ++c) {
    if (layout.channel_axis)
      at[*layout.channel_axis] = c;
    for (auto row = y.offset; row < y.offset + y.extent;) {
      at[layout.height_axis] = row;
      auto rows = y.offset + y.extent - row;
      for (auto column = x.offset; column < x.offset + x.extent;) {
        if (cancellation.cancelled())
          return {ErrorCode::Cancelled, "channel fill cancelled"};
        at[layout.width_axis] = column;
        auto write = output.rectangle_run(at);
        if (!write.ok())
          return write.status();
        const auto& rectangle = write.value();
        rows = std::min(rows, rectangle.rows);
        const auto samples =
            std::min(x.offset + x.extent - column, rectangle.row.samples);
        for (std::uint64_t dy = 0; dy < rows; ++dy) {
          for (std::uint64_t dx = 0; dx < samples;) {
            if (cancellation.cancelled())
              return {ErrorCode::Cancelled, "channel fill cancelled"};
            if (since_check >= 1024) {
              if (current && !current())
                return {ErrorCode::Stale, "channel fill plan changed"};
              since_check = 0;
            }
            const auto count = std::min(1024 - since_check, samples - dx);
            since_check += count;
            auto* to = rectangle.row.data + dy * rectangle.row_stride_bytes +
                       dx * width;
            std::memcpy(to, scalar, width);
            for (std::uint64_t filled = 1; filled < count;) {
              const auto more = std::min(filled, count - filled);
              std::memcpy(to + filled * width, to, more * width);
              filled += more;
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
inline Status copy_channel_piece(
    const Region& region, const DependencyMappedNeed& map,
    const PlanarImageReadWindow* image, const Value* value,
    const PlanarImageWriteWindow& output, const PlanarImageLayout& layout,
    std::uint64_t width, const CancellationToken& cancellation,
    const std::function<bool()>& current = {}) {
  if (!image && value && map.axes.size() == 1 &&
      map.axes[0].observation_axis < 0 && map.axes[0].fixed.extent == 1)
    return fill_scalar_channel_piece(region, map, *value, output, layout, width,
                                     cancellation, current);
  return copy_spatial_channel_piece(region, map, image, value, output, layout,
                                    width, cancellation, current);
}
}  // namespace ps::execution_internal
