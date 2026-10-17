#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/floating_environment.hpp"
#include "photospider/plugin/tile_helpers.hpp"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "common/image_contract.hpp"
#include "common/result_support.hpp"

namespace ps::plugin_internal::stmap_result {
using namespace numeric_ops;  // NOLINT(build/namespaces)
constexpr std::uint64_t coordinate_limit = UINT64_C(1) << 40;
enum class Boundary { Constant, Clamp, Wrap, Reflect, Mirror };
inline std::optional<std::uint64_t> boundary_index(std::int64_t index,
                                                   std::uint64_t extent,
                                                   Boundary mode) {
  const auto n = static_cast<std::int64_t>(extent);
  if (index >= 0 && index < n)
    return static_cast<std::uint64_t>(index);
  if (mode == Boundary::Constant)
    return {};
  if (mode == Boundary::Clamp)
    return index < 0 ? 0 : extent - 1;
  if (extent == 1)
    return 0;
  const auto period = mode == Boundary::Wrap      ? n
                      : mode == Boundary::Reflect ? 2 * n
                                                  : 2 * n - 2;
  auto position = index % period;
  if (position < 0)
    position += period;
  if (mode != Boundary::Wrap && position >= n)
    position =
        mode == Boundary::Reflect ? period - 1 - position : period - position;
  return static_cast<std::uint64_t>(position);
}
inline Boundary boundary(const std::string& mode) {
  if (mode == "constant")
    return Boundary::Constant;
  if (mode == "clamp")
    return Boundary::Clamp;
  if (mode == "wrap")
    return Boundary::Wrap;
  if (mode == "reflect")
    return Boundary::Reflect;
  if (mode == "mirror")
    return Boundary::Mirror;
  throw Status{ErrorCode::InvalidArgument, "unknown STMap boundary"};
}
struct Program final {
  using Coordinate = std::array<std::uint64_t, 8>;
  struct Tap {
    std::uint64_t y = 0, x = 0;
    double weight = 0;
    bool present = false;
  };
  struct Block {
    Region region;
    ResourceLease lease;
    std::uint64_t first = 0, count = 0, first_row = 0;
  };
  using MapValue = std::array<double, 2>;
  struct MapRow {
    std::uint64_t first = 0, count = 0;
    std::array<const std::uint8_t*, 2> data{};
    std::array<std::int64_t, 2> stride{};
    ResultTensorReadWindow window;
    ResourceVector<MapValue> copied;
  };
  struct Span {
    Coordinate at;
    std::uint64_t count = 0;
    std::array<const std::uint8_t*, 2> map_data{};
    std::array<std::int64_t, 2> map_stride{};
    std::array<std::uint8_t*, 4> data{};
    std::array<std::int64_t, 4> stride{};
  };
  struct TileWork {
    const Program& program;
    const ResultTensorSpec& source;
    const ResourceVector<Span>& spans;
    const CancellationToken& cancellation;
    std::array<Status, 4> failures{};
  };
  Boundary mode;
  std::size_t rank = 0, height_axis = 0, width_axis = 0;
  unsigned stage = 0;
  Footprint outputs;
  ResourceVector<Coordinate> source_coordinates;
  ResourceVector<Block> blocks;
  ResourceVector<MapValue> uv;
  ResourceVector<MapRow> map_rows;
  std::uint64_t pixel_count = 0, row_count = 0;
  bool use_map_rows = false;
  ResourceVector<std::array<float, 4>> source_pixels;
  std::optional<ResultBuilder> builder;
  explicit Program(Boundary selected) : mode(selected) {}

  static FootprintLimits phase_sets(const ResultProgramPhase& phase) {
    FootprintLimits limits;
    // The host's cumulative Actor/Run and Root ledgers bound this operation;
    // do not replace those configured limits with the standalone set defaults.
    limits.maximum_work = UINT64_MAX;
    limits.maximum_boxes = UINT64_MAX;
    limits.cancellation = phase.query.cancellation;
    limits.consume_work = phase.consume_work;
    return limits;
  }
  static std::uint64_t local_index(const Region& region, const Coordinate& at) {
    std::uint64_t index = 0;
    for (std::size_t axis = 0; axis + 1 < region.rank(); ++axis)
      index = index * region.dimensions()[axis].extent + at[axis] -
              region.dimensions()[axis].offset;
    return index;
  }
  static Region map_region(const ResultProgramQuery& query,
                           const Region& region) {
    const auto& map = query.inputs[1].result_schema->tensors[0];
    auto dims = region.dimensions();
    dims.back() = {0, 2};
    if (map.batch_axes.empty())
      dims.erase(
          dims.begin(),
          dims.begin() +
              query.inputs[0].result_schema->tensors[0].batch_axes.size());
    return Region(std::move(dims));
  }
  std::array<Tap, 4> taps_for(const std::array<double, 2>& value,
                              const ResultTensorSpec& source) const {
    const double u = value[0], v = value[1];
    const auto left = static_cast<std::int64_t>(std::floor(u - .5));
    const auto top = static_cast<std::int64_t>(std::floor(v - .5));
    const double fx = (u - .5) - static_cast<double>(left);
    const double fy = (v - .5) - static_cast<double>(top);
    std::array<Tap, 4> taps{};
    for (unsigned dy = 0; dy < 2; ++dy)
      for (unsigned dx = 0; dx < 2; ++dx) {
        auto& tap = taps[dy * 2 + dx];
        const auto y =
            boundary_index(top + dy, source.descriptor.shape[0], mode);
        const auto x =
            boundary_index(left + dx, source.descriptor.shape[1], mode);
        tap.weight = (dy ? fy : 1 - fy) * (dx ? fx : 1 - fx);
        tap.present = y.has_value() && x.has_value();
        if (tap.present) {
          tap.y = *y;
          tap.x = *x;
        }
      }
    return taps;
  }
  template <class Callback>
  void each_row(const Region& region, Callback callback) const {
    const auto& dims = region.dimensions();
    Coordinate at{};
    for (std::size_t axis = 0; axis < rank - 1; ++axis)
      at[axis] = dims[axis].offset;
    for (;;) {
      callback(at, dims[width_axis].extent);
      std::size_t axis = width_axis;
      while (axis) {
        --axis;
        if (++at[axis] < dims[axis].offset + dims[axis].extent)
          break;
        at[axis] = dims[axis].offset;
      }
      if (axis == 0 && at[0] == dims[0].offset)
        break;
    }
  }
  Region pixel_row(const Coordinate& at, std::uint64_t count) const {
    std::vector<RegionDimension> dims(rank);
    for (std::size_t axis = 0; axis < rank - 1; ++axis)
      dims[axis] = {at[axis], 1};
    dims[width_axis].extent = count;
    dims.back() = {0, 4};
    return Region(std::move(dims));
  }
  static std::ptrdiff_t byte_offset(std::uint64_t index, std::int64_t stride) {
    return static_cast<std::ptrdiff_t>(static_cast<__int128>(index) * stride);
  }
  static MapValue map_value(const std::array<const std::uint8_t*, 2>& data,
                            const std::array<std::int64_t, 2>& stride,
                            std::uint64_t index) {
    MapValue value;
    for (std::size_t c = 0; c < 2; ++c)
      std::memcpy(&value[c], data[c] + byte_offset(index, stride[c]), 8);
    return value;
  }
  static bool raw_map_row(const ResultTensorReadWindow& window,
                          std::vector<std::uint64_t> coordinate, MapRow* row) {
    const auto width_axis = coordinate.size() - 2;
    coordinate.back() = 0;
    if (window.sample_axis() == coordinate.size() - 1 &&
        window.row_axis() == width_axis) {
      const auto rectangle = math_take(window.rectangle_run(coordinate));
      if (rectangle.row.samples >= 2 && rectangle.rows >= row->count) {
        row->data = {rectangle.row.data,
                     rectangle.row.data + rectangle.row.sample_stride_bytes};
        row->stride.fill(rectangle.row_stride_bytes);
        return true;
      }
    } else if (window.sample_axis() == width_axis) {
      for (std::uint64_t c = 0; c < 2; ++c) {
        coordinate.back() = c;
        const auto run = math_take(window.row_run(coordinate));
        if (run.samples < row->count)
          return false;
        row->data[c] = run.data;
        row->stride[c] = run.sample_stride_bytes;
      }
      return true;
    }
    return false;
  }
  void read_map(const ResultProgramPhase& phase, const Block& block) {
    each_row(block.region, [&](Coordinate at, std::uint64_t count) {
      math_require(phase.consume_work(count * 2 + 1));
      const auto row = pixel_row(at, count);
      const auto region = map_region(phase.query, row);
      auto window = math_take(
          phase.tensors->at({1, 0}).acquire(region, phase.query.cancellation));
      std::vector<std::uint64_t> coordinate;
      for (const auto d : region.dimensions())
        coordinate.push_back(d.offset);
      const auto width_axis = coordinate.size() - 2;
      const auto index = block.first + local_index(block.region, at);
      MapRow cached;
      cached.first = index;
      cached.count = count;
      const bool raw = use_map_rows && raw_map_row(window, coordinate, &cached);
      MapValue* target = nullptr;
      if (!raw) {
        if (use_map_rows) {
          cached.copied = ResourceVector<MapValue>(
              ResourceAllocator<MapValue>(phase.resources));
          if (count > cached.copied.max_size())
            throw Status{ErrorCode::ResourceExhausted, "STMap map row size"};
          cached.copied.resize(count);
          target = cached.copied.data();
        } else {
          target = uv.data() + index;
        }
        for (std::size_t c = 0; c < 2; ++c)
          cached.data[c] = reinterpret_cast<const std::uint8_t*>(&target[0][c]);
        cached.stride.fill(sizeof(MapValue));
      }
      std::uint64_t done = 0;
      while (!raw && done < count) {
        math_require(phase.consume_work(0));
        coordinate[width_axis] = at[this->width_axis] + done;
        coordinate.back() = 0;
        if (window.sample_axis() == coordinate.size() - 1 &&
            window.row_axis() == width_axis) {
          auto rectangle = math_take(window.rectangle_run(coordinate));
          if (rectangle.row.samples >= 2) {
            const auto n =
                std::min<std::uint64_t>({count - done, rectangle.rows, 256});
            for (std::uint64_t i = 0; i < n; ++i) {
              const auto* first =
                  rectangle.row.data +
                  static_cast<std::int64_t>(i) * rectangle.row_stride_bytes;
              std::memcpy(&target[done + i][0], first, 8);
              std::memcpy(&target[done + i][1],
                          first + rectangle.row.sample_stride_bytes, 8);
            }
            done += n;
            continue;
          }
        }
        // A generic map may itself be planar, tiled, or have signed strides.
        std::array<ResultTensorRun, 2> runs;
        std::uint64_t n = std::min<std::uint64_t>(256, count - done);
        for (std::uint64_t c = 0; c < 2; ++c) {
          coordinate.back() = c;
          runs[c] = math_take(window.row_run(coordinate));
          n = std::min(
              n, window.sample_axis() == width_axis ? runs[c].samples : 1);
        }
        for (std::uint64_t i = 0; i < n; ++i)
          for (std::uint64_t c = 0; c < 2; ++c)
            std::memcpy(&target[done + i][c],
                        runs[c].data + static_cast<std::int64_t>(i) *
                                           runs[c].sample_stride_bytes,
                        8);
        done += n;
      }
      for (std::uint64_t i = 0; i < count; ++i) {
        if (!(i & 255U))
          math_require(phase.consume_work(0));
        for (const auto value : map_value(cached.data, cached.stride, i))
          if (!std::isfinite(value) || std::abs(value) > coordinate_limit)
            throw Status{ErrorCode::OperationFailed,
                         "STMap coordinate must be finite and within +/-2^40"};
      }
      if (use_map_rows) {
        if (raw)
          cached.window = std::move(window);
        map_rows.push_back(std::move(cached));
      }
    });
  }
  TileNeed prepare_source(const ResultProgramPhase& phase) {
    const auto& source = phase.query.inputs[0].result_schema->tensors[0];
    // Retain at most one owner/directory entry per long logical row. Sparse,
    // partial and multi-Result maps keep the compact per-pixel copy. A raw
    // window owns its immutable bytes independently of the current poll.
    use_map_rows = pixel_count >= 65536 && blocks.size() == 1 &&
                   blocks[0].region.dimensions()[width_axis].extent >= 256 &&
                   phase.tensors->at({1, 0}).object_id();
    if (use_map_rows) {
      const auto map = map_region(phase.query, blocks[0].region);
      const auto shape =
          phase.query.inputs[1].result_schema->tensors[0].sample_shape();
      for (std::size_t axis = 0; axis < shape.size(); ++axis)
        use_map_rows &= !map.dimensions()[axis].offset &&
                        map.dimensions()[axis].extent == shape[axis];
    }
    if (use_map_rows) {
      math_require(phase.consume_work(row_count * 6));
      if (row_count > map_rows.max_size())
        throw Status{ErrorCode::ResourceExhausted, "STMap map row count"};
      map_rows.reserve(row_count);
    } else {
      if (pixel_count > uv.max_size())
        throw Status{ErrorCode::ResourceExhausted, "STMap map size"};
      uv.resize(pixel_count);
    }
    for (const auto& block : blocks)
      read_map(phase, block);
    if (pixel_count > source_coordinates.max_size() / 4)
      throw Status{ErrorCode::ResourceExhausted, "STMap tap coordinate count"};
    source_coordinates.reserve(pixel_count * 4);
    for (const auto& block : blocks) {
      each_row(block.region, [&](Coordinate at, std::uint64_t count) {
        math_require(phase.consume_work(count * 4));
        for (std::uint64_t x = 0; x < count; ++x) {
          if (!(x & 255U))
            math_require(phase.consume_work(0));
          const auto local = local_index(block.region, at);
          MapValue value;
          if (use_map_rows) {
            const auto width = block.region.dimensions()[width_axis].extent;
            const auto& row = map_rows[block.first_row + local / width];
            value = map_value(row.data, row.stride, local % width);
          } else {
            value = uv[block.first + local];
          }
          for (const auto& tap : taps_for(value, source))
            if (tap.present) {
              auto point = at;
              point[height_axis] = tap.y;
              point[width_axis] = tap.x;
              source_coordinates.push_back(point);
            }
          ++at[width_axis];
        }
      });
    }
    std::uint64_t levels = 1;
    for (auto n = source_coordinates.size(); n; n >>= 1)
      ++levels;
    if (source_coordinates.size() > UINT64_MAX / (8 * levels))
      throw Status{ErrorCode::ResourceExhausted, "STMap tap sorting work"};
    math_require(phase.consume_work(source_coordinates.size() * 8 * levels));
    std::uint64_t comparisons = 0;
    std::sort(
        source_coordinates.begin(), source_coordinates.end(),
        [&](const Coordinate& a, const Coordinate& b) {
          if (!(++comparisons & 255U) && phase.query.cancellation.cancelled())
            throw Status{ErrorCode::Cancelled, {}};
          return a < b;
        });
    source_coordinates.erase(
        std::unique(source_coordinates.begin(), source_coordinates.end()),
        source_coordinates.end());
    math_require(phase.consume_work(source_coordinates.size() * 6));
    if (source_coordinates.empty())
      return TileNeed{0, 0, true, {}};
    const auto grid = phase.query.input_tile_grids.empty()
                          ? math_take(TileGrid::from_schema(
                                *phase.query.inputs[0].result_schema,
                                phase.query.tile_extent))
                          : phase.query.input_tile_grids.at(0).at(0);
    ResourceVector<TileIndex> tiles{
        ResourceAllocator<TileIndex>(phase.resources)};
    tiles.reserve(source_coordinates.size());
    for (const auto& at : source_coordinates) {
      tiles.push_back(
          math_take(sdk::tile_for_coordinate(grid, at.data(), rank)));
    }
    return TileNeed{0, 0, false,
                    math_take(TileSet::from_indices(
                        phase.resources, tiles.data(), tiles.size()))};
  }
  void read_source(const ResultProgramPhase& phase) {
    source_pixels.resize(source_coordinates.size());
    for (std::size_t first = 0; first < source_coordinates.size();) {
      const auto& at = source_coordinates[first];
      std::size_t end = first + 1;
      while (end < source_coordinates.size()) {
        auto expected = at;
        expected[width_axis] += end - first;
        if (source_coordinates[end] != expected)
          break;
        ++end;
      }
      math_require(phase.consume_work((end - first) * 4));
      const auto row = pixel_row(at, end - first);
      auto window = math_take(
          phase.tensors->at({0, 0}).acquire(row, phase.query.cancellation));
      for (std::uint64_t c = 0; c < 4; ++c) {
        std::vector<std::uint64_t> position(at.begin(), at.begin() + rank);
        position.back() = c;
        for (auto i = first; i < end;) {
          math_require(phase.consume_work(0));
          position[width_axis] = at[width_axis] + i - first;
          const auto run = math_take(window.row_run(position));
          const auto count = std::min<std::uint64_t>(
              {end - i, window.sample_axis() == width_axis ? run.samples : 1,
               256});
          for (std::uint64_t j = 0; j < count; ++j)
            std::memcpy(&source_pixels[i + j][c],
                        run.data + byte_offset(j, run.sample_stride_bytes), 4);
          i += count;
        }
      }
      first = end;
    }
  }
  std::array<float, 4> sample(const Coordinate& at,
                              const std::array<double, 2>& map,
                              const ResultTensorSpec& source) const {
    const auto taps = taps_for(map, source);
    std::array<std::array<float, 4>, 4> values{};
    for (std::size_t i = 0; i < taps.size(); ++i) {
      if (!taps[i].present)
        continue;
      auto key = at;
      key[height_axis] = taps[i].y;
      key[width_axis] = taps[i].x;
      const auto found = std::lower_bound(source_coordinates.begin(),
                                          source_coordinates.end(), key);
      if (found == source_coordinates.end() || *found != key)
        throw Status{ErrorCode::OperationFailed,
                     "STMap source index is incomplete"};
      values[i] = source_pixels[found - source_coordinates.begin()];
    }
    std::array<float, 4> result{};
    std::array<double, 4> sums{};
    for (std::size_t i = 0; i < 4; ++i) {
      const double alpha_weight =
          static_cast<double>(values[i][3]) * taps[i].weight;
      sums[3] += alpha_weight;
      if (alpha_weight == 0)
        continue;
      for (std::size_t c = 0; c < 3; ++c)
        sums[c] += static_cast<double>(values[i][c]) * alpha_weight;
    }
    result[3] = static_cast<float>(sums[3]);
    for (std::size_t c = 0; c < 3; ++c)
      result[c] = sums[3] == 0 ? 0.0F : static_cast<float>(sums[c] / sums[3]);

    return result;
  }
  void render_span(const Span& span, const ResultTensorSpec& source,
                   const CancellationToken& cancellation) const {
    if (cancellation.cancelled())
      throw Status{ErrorCode::Cancelled, {}};
    auto at = span.at;
    for (std::uint64_t i = 0; i < span.count; ++i) {
      const auto value =
          sample(at, map_value(span.map_data, span.map_stride, i), source);
      for (std::uint64_t c = 0; c < 4; ++c)
        std::memcpy(
            span.data[c] + static_cast<std::int64_t>(i) * span.stride[c],
            &value[c], 4);
      ++at[width_axis];
    }
  }
  static int tile(void* raw, const ps_cpu_tile_v2* range) noexcept {
    auto& work = *static_cast<TileWork*>(raw);
    auto& failure = work.failures[range->slot];
    try {
      FloatingEnvironment environment;
      if (!environment.active())
        throw Status{ErrorCode::OperationFailed,
                     "cannot set numerical environment"};
      if (failure.ok()) {
        for (auto i = range->begin[0]; i < range->end[0]; ++i)
          work.program.render_span(work.spans[i], work.source,
                                   work.cancellation);
      }
    } catch (Status& status) {
      failure = std::move(status);
    } catch (const std::bad_alloc&) {
      failure.code = ErrorCode::ResourceExhausted;
    } catch (...) {
      failure.code = ErrorCode::OperationFailed;
    }
    return failure.ok()                                   ? 0
           : failure.code == ErrorCode::Cancelled         ? 2
           : failure.code == ErrorCode::ResourceExhausted ? 4
                                                          : 1;
  }
  void publish(const ResultProgramPhase& phase) {
    const auto& source = phase.query.inputs[0].result_schema->tensors[0];
    std::uint64_t levels = 1;
    for (auto n = source_coordinates.size(); n; n >>= 1)
      ++levels;
    const bool tiled = phase.cpu_tiles && pixel_count >= 65536;
    const auto per_pixel = 24 + 32 * levels + (tiled ? 16 : 0);
    if (pixel_count > UINT64_MAX / per_pixel)
      throw Status{ErrorCode::ResourceExhausted, "STMap interpolation work"};
    math_require(builder->publish_tensor_kernel(
        0, outputs,
        [&](const auto& writers) {
          return math_callback(phase, [&] {
            // Window services and accounting belong to this coordinator. The
            // synchronous stage receives only immutable inputs and disjoint
            // raw spans, each no longer than 256 pixels.
            math_require(phase.consume_work(pixel_count * per_pixel));
            ResourceVector<Span> spans{
                ResourceAllocator<Span>(phase.resources)};
            if (tiled)
              spans.reserve(pixel_count / 256 + (pixel_count % 256 != 0));
            for (const auto& writer : writers)
              each_row(writer.region(), [&](Coordinate at,
                                            std::uint64_t count) {
                math_require(phase.consume_work(0));
                std::size_t b = 0;
                for (; b < blocks.size(); ++b) {
                  const auto& dimensions = blocks[b].region.dimensions();
                  bool contains = true;
                  for (std::size_t axis = 0; axis < rank - 1; ++axis)
                    contains &= at[axis] >= dimensions[axis].offset &&
                                at[axis] - dimensions[axis].offset <
                                    dimensions[axis].extent;
                  if (contains)
                    break;
                }
                if (b == blocks.size())
                  throw Status{ErrorCode::OperationFailed,
                               "STMap output index is incomplete"};
                const auto& block = blocks[b];
                std::vector<std::uint64_t> coordinate(at.begin(),
                                                      at.begin() + rank);
                for (std::uint64_t done = 0; done < count;) {
                  if (phase.query.cancellation.cancelled())
                    throw Status{ErrorCode::Cancelled, {}};
                  std::array<ResultTensorMutableRun, 4> runs;
                  auto n = std::min<std::uint64_t>(256, count - done);
                  coordinate[width_axis] = at[width_axis];
                  for (std::uint64_t c = 0; c < 4; ++c) {
                    coordinate[rank - 1] = c;
                    runs[c] = math_take(writer.row_run(coordinate));
                    n = std::min(n, runs[c].samples);
                  }
                  Span span;
                  span.at = at;
                  span.count = n;
                  const auto local = local_index(block.region, at);
                  if (use_map_rows) {
                    const auto width =
                        block.region.dimensions()[width_axis].extent;
                    const auto& selected =
                        map_rows[block.first_row + local / width];
                    n = std::min(n, selected.count - local % width);
                    span.count = n;
                    for (std::size_t c = 0; c < 2; ++c) {
                      span.map_data[c] =
                          selected.data[c] +
                          byte_offset(local % width, selected.stride[c]);
                      span.map_stride[c] = selected.stride[c];
                    }
                  } else {
                    for (std::size_t c = 0; c < 2; ++c)
                      span.map_data[c] = reinterpret_cast<const std::uint8_t*>(
                          &uv[block.first + local][c]);
                    span.map_stride.fill(sizeof(MapValue));
                  }
                  for (std::uint64_t c = 0; c < 4; ++c) {
                    span.data[c] = runs[c].data;
                    span.stride[c] = runs[c].sample_stride_bytes;
                  }
                  if (tiled)
                    spans.push_back(span);
                  else
                    render_span(span, source, phase.query.cancellation);
                  at[width_axis] += n;
                  done += n;
                }
              });
            if (tiled) {
              TileWork work{*this, source, spans, phase.query.cancellation};
              // Geometry is independent of the worker grant. At most 64
              // claims bound service overhead even for very large images.
              const auto grain = spans.size() / 64 + (spans.size() % 64 != 0);
              const auto grant = std::min(4U, phase.cpu_tiles->maximum_workers);
              const ps_cpu_tile_stage_v2 tile_stage{
                  sizeof(ps_cpu_tile_stage_v2),
                  {spans.size(), 1, 1},
                  {grain, 1, 1},
                  grant};
              const int code = phase.cpu_tiles->run(phase.cpu_tiles->context,
                                                    &tile_stage, tile, &work);
              for (const auto& failure : work.failures)
                math_require(failure);
              if (code)
                throw Status{code == 2   ? ErrorCode::Cancelled
                             : code == 4 ? ErrorCode::ResourceExhausted
                                         : ErrorCode::OperationFailed,
                             "STMap CPU tile stage failed"};
            }
            return phase.consume_work(0);
          });
        },
        phase.query.cancellation));
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto scratch =
        math_take(phase.resources.reserve(ResourceCapacity::host(8192, 8192)));
    math_require(phase.consume_work(1));
    FloatingEnvironment environment;
    if (!environment.active())
      throw Status{ErrorCode::OperationFailed,
                   "STMap numeric environment unavailable"};
    const auto& output = *phase.query.output.result_schema;
    const auto limits = phase_sets(phase);
    if (stage == 0) {
      const auto shape = output.tensors[0].sample_shape();
      rank = shape.size();
      height_axis = rank - 3;
      width_axis = rank - 2;
      outputs = math_take(output.tensors[0].close_samples(
          phase.query.tensor_outputs ? *phase.query.tensor_outputs
                                     : math_take(Footprint::all(shape, limits)),
          limits));
      if (outputs.empty()) {
        auto empty = math_take(ResultBuilder::start(phase.resources, output,
                                                    phase.query.semantic_key));
        return Result<ResultProgramPoll>(
            ResultPublication{math_take(empty.seal()), true});
      }
      blocks = ResourceVector<Block>(ResourceAllocator<Block>(phase.resources));
      uv = ResourceVector<MapValue>(
          ResourceAllocator<MapValue>(phase.resources));
      map_rows =
          ResourceVector<MapRow>(ResourceAllocator<MapRow>(phase.resources));
      source_pixels = ResourceVector<std::array<float, 4>>(
          ResourceAllocator<std::array<float, 4>>(phase.resources));
      source_coordinates = ResourceVector<Coordinate>(
          ResourceAllocator<Coordinate>(phase.resources));
      std::vector<Region> map_boxes;
      std::uint64_t count = 0;
      for (const auto& box : outputs.boxes()) {
        const auto pixels = math_take(box.element_count()) / 4;
        if (pixels > UINT64_MAX - count)
          throw Status{ErrorCode::ResourceExhausted,
                       "STMap requested pixel count"};
        const auto bytes = box.rank() * sizeof(RegionDimension);
        auto lease = math_take(
            phase.resources.reserve(ResourceCapacity::host(bytes, bytes)));
        blocks.push_back({box, std::move(lease), count, pixels, row_count});
        row_count += pixels / box.dimensions()[width_axis].extent;
        count += pixels;
        map_boxes.push_back(map_region(phase.query, box));
      }
      pixel_count = count;
      const auto& source = phase.query.inputs[0].result_schema->tensors[0];
      const auto& map = phase.query.inputs[1].result_schema->tensors[0];
      ResultProgramNeed need;
      {
        auto tile_need_0 = sdk::tile_need_for_samples(
            phase.resources, phase.query, 0, 0,
            math_take(Footprint::none(source.sample_shape(), limits)), true);
        if (!tile_need_0.ok())
          return Result<ResultProgramPoll>(tile_need_0.status());
        need.tiles.push_back(tile_need_0.take_value());
      }
      {
        auto tile_need_1 = sdk::tile_need_for_samples(
            phase.resources, phase.query, 1, 0,
            math_take(Footprint::from_regions(map.sample_shape(), map_boxes,
                                              limits)));
        if (!tile_need_1.ok())
          return Result<ResultProgramPoll>(tile_need_1.status());
        need.tiles.push_back(tile_need_1.take_value());
      }
      stage = 1;
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (stage == 1) {
      builder.emplace(math_take(ResultBuilder::start(
          phase.resources, output, phase.query.semantic_key, {},
          phase.query.tile_extent)));
      ResultProgramNeed need;
      need.tiles.push_back(prepare_source(phase));
      stage = 2;
      return Result<ResultProgramPoll>(std::move(need));
    }
    read_source(phase);
    publish(phase);
    return Result<ResultProgramPoll>(
        ResultPublication{math_take(builder->seal()), true});
  } catch (const Status& status) {
    math_record_failure(phase, status);
    return Result<ResultProgramPoll>(status);
  }
};
inline OperationDefinition operation() {
  OperationDefinition definition;
  definition.key = "image.stmap";
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  traits.input_schema[0].element_type =
      static_cast<std::uint32_t>(ElementType::Float32);
  traits.input_schema[0].rank = 3;
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  set_whole_tensor_output(traits, ElementType::Float32, sizeof(Program));
  traits.outputs[0].key = "value";
  traits.outputs[0].maximum_dependency_stages = 1048576;
  traits.outputs[0].output_schema.result_schema_id = "photospider.image";
  traits.outputs[0].output_schema.tensor_key = "pixels";
  traits.outputs[0].result_schema = image_ops::image_schema();
  traits.requires_metadata_specialization = true;
  traits.cpu_staged_tiles = true;
  traits.parameter_schema = {
      {"boundary", OperationParameterType::String, true}};
  definition.specialize_metadata = [](const auto& inputs,
                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    try {
      math_require(image_ops::check_image(inputs[0]));
      if (!inputs[1].result_schema ||
          inputs[1].result_schema->tensors.size() != 1)
        throw Status{ErrorCode::TypeMismatch, "STMap requires one map tensor"};
      const auto& source = inputs[0].result_schema->tensors[0];
      const auto& map = inputs[1].result_schema->tensors[0];
      if (map.descriptor.shape.size() != 3 || map.descriptor.shape[2] != 2 ||
          (!map.batch_axes.empty() && map.batch_axes != source.batch_axes) ||
          source.descriptor.shape[0] > coordinate_limit ||
          source.descriptor.shape[1] > coordinate_limit)
        throw Status{ErrorCode::TypeMismatch,
                     "STMap requires a generic two-component map and source "
                     "axes <=2^40"};
      boundary(std::get<std::string>(parameters.at("boundary")));
      auto schema = image_ops::image_schema();
      schema.tensors[0].batch_axes = source.batch_axes;
      schema.tensors[0].descriptor.shape = {map.descriptor.shape[0],
                                            map.descriptor.shape[1], 4};
      OperationOutputSpecialization output;
      output.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(schema));
      return Result<std::vector<OperationOutputSpecialization>>(
          std::vector<OperationOutputSpecialization>{std::move(output)});
    } catch (const Status& status) {
      return Result<std::vector<OperationOutputSpecialization>>(status);
    }
  };
  definition.start_result = [](const ResultProgramQuery& query,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<Program>(
        allocator,
        boundary(std::get<std::string>(query.parameters.at("boundary"))));
  };
  return definition;
}
}  // namespace ps::plugin_internal::stmap_result
