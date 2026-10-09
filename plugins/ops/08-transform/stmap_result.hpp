#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "00-foundation/image_program.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "data/footprint_index.hpp"
#include "plugin/port_validation.hpp"

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
  using Coordinate = std::array<std::uint64_t, 4>;
  struct Tap {
    std::uint64_t y = 0, x = 0;
    double weight = 0;
    bool present = false;
  };
  struct Block {
    Region region;
    ResourceLease lease;
    std::uint64_t first = 0, count = 0;
  };
  struct Span {
    Coordinate at;
    std::uint64_t first = 0, count = 0;
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
  unsigned stage = 0;
  Footprint outputs, source_samples;
  ResultRelation witness;
  ResourceVector<Block> blocks;
  ResourceVector<std::array<double, 2>> uv;
  ResourceVector<std::array<float, 4>> source_pixels;
  ResourceVector<std::uint64_t> source_first;
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
    for (std::size_t axis = 0; axis < 4; ++axis)
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
      dims.erase(dims.begin(), dims.begin() + 2);
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
  static void each_row(const Region& region, Callback callback) {
    const auto& d = region.dimensions();
    for (std::uint64_t f = d[0].offset; f < d[0].offset + d[0].extent; ++f)
      for (std::uint64_t l = d[1].offset; l < d[1].offset + d[1].extent; ++l)
        for (std::uint64_t y = d[2].offset; y < d[2].offset + d[2].extent; ++y)
          callback(Coordinate{f, l, y, d[3].offset}, d[3].extent);
  }
  void read_map(const ResultProgramPhase& phase, const Block& block) {
    each_row(block.region, [&](Coordinate at, std::uint64_t count) {
      math_require(phase.consume_work(count * 2 + 1));
      Region row({{at[0], 1}, {at[1], 1}, {at[2], 1}, {at[3], count}, {0, 4}});
      const auto region = map_region(phase.query, row);
      auto window = math_take(
          phase.tensors->at({1, 0}).acquire(region, phase.query.cancellation));
      std::vector<std::uint64_t> coordinate;
      for (const auto d : region.dimensions())
        coordinate.push_back(d.offset);
      const auto width_axis = coordinate.size() - 2;
      auto index = block.first + local_index(block.region, at);
      std::uint64_t done = 0;
      while (done < count) {
        math_require(phase.consume_work(0));
        coordinate[width_axis] = at[3] + done;
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
              std::memcpy(&uv[index + done + i][0], first, 8);
              std::memcpy(&uv[index + done + i][1],
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
            std::memcpy(&uv[index + done + i][c],
                        runs[c].data + static_cast<std::int64_t>(i) *
                                           runs[c].sample_stride_bytes,
                        8);
        done += n;
      }
      for (std::uint64_t i = 0; i < count; ++i) {
        if (!(i & 255U))
          math_require(phase.consume_work(0));
        for (const auto value : uv[index + i])
          if (!std::isfinite(value) || std::abs(value) > coordinate_limit)
            throw Status{ErrorCode::OperationFailed,
                         "STMap coordinate must be finite and within +/-2^40"};
      }
    });
  }
  ResultRelation map_relation(const ResultProgramPhase& phase,
                              const Region& region) const {
    const auto& map = phase.query.inputs[1].result_schema->tensors[0];
    const auto shape = map.sample_shape();
    std::vector<ResultMappedAxis> axes(shape.size());
    for (std::size_t axis = 0; axis + 1 < shape.size(); ++axis)
      axes[axis].output_axis = axis + (map.batch_axes.empty() ? 2 : 0);
    axes.back().extent = 2;
    const auto grouped = std::max<std::uint32_t>(1, map.atomic_trailing_axes);
    auto control = math_take(ResultRelation::mapped(
        phase.resources, outputs.shape(), region, shape, axes,
        {1, grouped == 1 ? 6U : 2U, 0, 0, ResultSupportTarget::Tensor, 0}));
    if (grouped == 1)
      return control;
    for (std::size_t axis = shape.size() - grouped; axis < shape.size();
         ++axis) {
      axes[axis].output_axis = -1;
      axes[axis].source_origin = 0;
      axes[axis].extent = shape[axis];
    }
    auto validation = math_take(ResultRelation::mapped(
        phase.resources, outputs.shape(), region, shape, axes,
        {1, 4, 0, 0, ResultSupportTarget::Tensor, 0}));
    return math_take(
        ResultRelation::unite(phase.resources, {control, validation}));
  }
  void prepare_relations(const ResultProgramPhase& phase) {
    const auto& source = phase.query.inputs[0].result_schema->tensors[0];
    const auto limits = phase_sets(phase);
    auto bounding = blocks[0].region.dimensions();
    for (const auto& block : blocks) {
      read_map(phase, block);
      for (std::size_t axis = 0; axis < bounding.size(); ++axis) {
        const auto d = block.region.dimensions()[axis];
        const auto last = std::max(
            bounding[axis].offset + bounding[axis].extent, d.offset + d.extent);
        bounding[axis].offset = std::min(bounding[axis].offset, d.offset);
        bounding[axis].extent = last - bounding[axis].offset;
      }
    }
    std::vector<ResultMappedAxis> axes(5);
    axes[0].output_axis = 0;
    axes[1].output_axis = 1;
    axes[4].extent = 4;
    std::uint64_t previous = UINT64_MAX;
    std::array<Tap, 4> taps{};
    auto gather = math_take(ResultRelation::gather(
        phase.resources, outputs.shape(), outputs, 1, source.sample_shape(),
        axes, 12, 4,
        [&](std::uint64_t row,
            std::uint32_t tap) -> Result<ResultGatherSample> {
          if (row != previous) {
            if (!(row & 255U))
              math_require(phase.consume_work(
                  std::min<std::uint64_t>(256, uv.size() - row) * 4));
            previous = row;
            taps = taps_for(uv[row], source);
          }
          ResultGatherSample value;
          value.present = taps[tap].present;
          value.coordinates[2] = taps[tap].y;
          value.coordinates[3] = taps[tap].x;
          return Result<ResultGatherSample>(value);
        },
        {0, 1, 0, 0, ResultSupportTarget::Tensor, 0},
        std::max<std::uint32_t>(1, source.atomic_trailing_axes), limits));
    source_samples = math_take(Footprint::none(source.sample_shape(), limits));
    math_require(gather.project(
        outputs,
        [&](ResultSupport support, const Footprint* samples) {
          if ((support.roles & 1U) && samples)
            source_samples = *samples;
          return Status::success();
        },
        limits));
    witness = math_take(ResultRelation::unite(
        phase.resources,
        {gather, map_relation(phase, Region(std::move(bounding)))}));
  }
  void read_source(const ResultProgramPhase& phase) {
    std::uint64_t count = 0;
    source_first.reserve(source_samples.boxes().size());
    for (const auto& box : source_samples.boxes()) {
      source_first.push_back(count);
      const auto n = math_take(box.element_count()) / 4;
      if (n > UINT64_MAX - count)
        throw Status{ErrorCode::ResourceExhausted, "STMap source count"};
      count += n;
    }
    source_pixels.resize(count);
    for (std::size_t b = 0; b < source_samples.boxes().size(); ++b) {
      const auto& box = source_samples.boxes()[b];
      each_row(box, [&](Coordinate at, std::uint64_t width) {
        math_require(phase.consume_work(width * 4));
        const auto first = source_first[b] + local_index(box, at);
        Region region(
            {{at[0], 1}, {at[1], 1}, {at[2], 1}, {at[3], width}, {0, 4}});
        auto window = math_take(phase.tensors->at({0, 0}).acquire(
            region, phase.query.cancellation));
        std::vector<std::uint64_t> coordinate{at[0], at[1], at[2], at[3], 0};
        for (std::uint64_t c = 0; c < 4; ++c) {
          coordinate[4] = c;
          for (std::uint64_t i = 0; i < width;) {
            math_require(phase.consume_work(0));
            coordinate[3] = at[3] + i;
            const auto run = math_take(window.row_run(coordinate));
            const auto n =
                std::min<std::uint64_t>({width - i, run.samples, 256});
            for (std::uint64_t j = 0; j < n; ++j)
              std::memcpy(&source_pixels[first + i + j][c],
                          run.data + static_cast<std::int64_t>(j) *
                                         run.sample_stride_bytes,
                          4);
            i += n;
          }
        }
      });
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
      const Coordinate key{at[0], at[1], taps[i].y, taps[i].x};
      const auto box = source_samples.boxes().size() == 1
                           ? 0
                           : footprint_internal::containing(
                                 source_samples.boxes(), key.data(), 4);
      if (box == source_samples.boxes().size())
        throw Status{ErrorCode::OperationFailed,
                     "STMap source index is incomplete"};
      values[i] = source_pixels[source_first[box] +
                                local_index(source_samples.boxes()[box], key)];
    }
    std::array<float, 4> result{};
#if defined(__aarch64__)
    // Vectorize channels, keeping each lane's TL/TR/BL/BR fold and separate
    // Float64 multiply/add. The operation TU disables contraction/fast math.
    auto low = vdupq_n_f64(0), high = vdupq_n_f64(0);
    for (std::size_t i = 0; i < 4; ++i) {
      const auto rgba = vld1q_f32(values[i].data());
      const auto weight = vdupq_n_f64(taps[i].weight);
      low = vaddq_f64(low, vmulq_f64(vcvt_f64_f32(vget_low_f32(rgba)), weight));
      high =
          vaddq_f64(high, vmulq_f64(vcvt_f64_f32(vget_high_f32(rgba)), weight));
    }
    vst1q_f32(result.data(),
              vcombine_f32(vcvt_f32_f64(low), vcvt_f32_f64(high)));
#else
    std::array<double, 4> sums{};
    for (std::size_t i = 0; i < 4; ++i)
      for (std::size_t c = 0; c < 4; ++c)
        sums[c] += static_cast<double>(values[i][c]) * taps[i].weight;
    for (std::size_t c = 0; c < 4; ++c)
      result[c] = static_cast<float>(sums[c]);
#endif
    return result;
  }
  void render_span(const Span& span, const ResultTensorSpec& source,
                   const CancellationToken& cancellation) const {
    if (cancellation.cancelled())
      throw Status{ErrorCode::Cancelled, {}};
    auto at = span.at;
    for (std::uint64_t i = 0; i < span.count; ++i) {
      const auto value = sample(at, uv[span.first + i], source);
      for (std::uint64_t c = 0; c < 4; ++c)
        std::memcpy(
            span.data[c] + static_cast<std::int64_t>(i) * span.stride[c],
            &value[c], 4);
      ++at[3];
    }
  }
  static int tile(void* raw, const ps_cpu_tile_v1* range) noexcept {
    auto& work = *static_cast<TileWork*>(raw);
    auto& failure = work.failures[range->slot];
    try {
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
    for (auto n = source_samples.boxes().size(); n; n >>= 1)
      ++levels;
    const bool tiled = phase.cpu_tiles && uv.size() >= 65536;
    const auto per_pixel = 24 + 32 * levels + (tiled ? 16 : 0);
    if (uv.size() > UINT64_MAX / per_pixel)
      throw Status{ErrorCode::ResourceExhausted, "STMap interpolation work"};
    math_require(builder->publish_tensor_kernel(
        0, outputs,
        [&](const auto& writers) {
          return math_callback(phase, [&] {
            // Window services and accounting belong to this coordinator. The
            // synchronous stage receives only immutable inputs and disjoint
            // raw spans, each no longer than 256 pixels.
            math_require(phase.consume_work(uv.size() * per_pixel));
            ResourceVector<Span> spans{
                ResourceAllocator<Span>(phase.resources)};
            if (tiled)
              spans.reserve(uv.size() / 256 + (uv.size() % 256 != 0));
            for (const auto& writer : writers)
              each_row(writer.region(), [&](Coordinate at,
                                            std::uint64_t count) {
                math_require(phase.consume_work(0));
                const auto b = blocks.size() == 1
                                   ? 0
                                   : footprint_internal::containing(
                                         outputs.boxes(), at.data(), 4);
                if (b == blocks.size())
                  throw Status{ErrorCode::OperationFailed,
                               "STMap output index is incomplete"};
                const auto& block = blocks[b];
                std::vector<std::uint64_t> coordinate(at.begin(), at.end());
                coordinate.push_back(0);
                for (std::uint64_t done = 0; done < count;) {
                  if (phase.query.cancellation.cancelled())
                    throw Status{ErrorCode::Cancelled, {}};
                  std::array<ResultTensorMutableRun, 4> runs;
                  auto n = std::min<std::uint64_t>(256, count - done);
                  coordinate[3] = at[3];
                  for (std::uint64_t c = 0; c < 4; ++c) {
                    coordinate[4] = c;
                    runs[c] = math_take(writer.row_run(coordinate));
                    n = std::min(n, runs[c].samples);
                  }
                  Span span{at, block.first + local_index(block.region, at), n};
                  for (std::uint64_t c = 0; c < 4; ++c) {
                    span.data[c] = runs[c].data;
                    span.stride[c] = runs[c].sample_stride_bytes;
                  }
                  if (tiled)
                    spans.push_back(span);
                  else
                    render_span(span, source, phase.query.cancellation);
                  at[3] += n;
                  done += n;
                }
              });
            if (tiled) {
              TileWork work{*this, source, spans, phase.query.cancellation};
              // Geometry is independent of the worker grant. At most 64
              // claims bound service overhead even for very large images.
              const auto grain = spans.size() / 64 + (spans.size() % 64 != 0);
              const auto grant = std::min(4U, phase.cpu_tiles->maximum_workers);
              const ps_cpu_tile_stage_v1 tile_stage{
                  sizeof(ps_cpu_tile_stage_v1),
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
        witness, {true, true, true, true}, phase.query.cancellation));
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto scratch =
        math_take(phase.resources.reserve(ResourceCapacity::host(8192, 8192)));
    math_require(phase.consume_work(1));
    input_internal::Float32Environment environment;
    if (!environment.active())
      throw Status{ErrorCode::OperationFailed,
                   "STMap numeric environment unavailable"};
    const auto& output = *phase.query.output.result_schema;
    const auto limits = phase_sets(phase);
    if (stage == 0) {
      const auto shape = output.tensors[0].sample_shape();
      outputs = math_take(output.tensors[0].close_samples(
          phase.query.tensor_outputs ? *phase.query.tensor_outputs
                                     : math_take(Footprint::all(shape, limits)),
          limits));
      if (outputs.empty()) {
        auto empty = math_take(ResultBuilder::start(phase.resources, output,
                                                    phase.query.semantic_key));
        math_require(empty.bind_descriptor_relation(
            math_take(ResultRelation::cartesian(phase.resources, 1, {}))));
        return Result<ResultProgramPoll>(
            ResultPublication{math_take(empty.seal()), true});
      }
      blocks = ResourceVector<Block>(ResourceAllocator<Block>(phase.resources));
      uv = ResourceVector<std::array<double, 2>>(
          ResourceAllocator<std::array<double, 2>>(phase.resources));
      source_pixels = ResourceVector<std::array<float, 4>>(
          ResourceAllocator<std::array<float, 4>>(phase.resources));
      source_first = ResourceVector<std::uint64_t>(
          ResourceAllocator<std::uint64_t>(phase.resources));
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
        blocks.push_back({box, std::move(lease), count, pixels});
        count += pixels;
        map_boxes.push_back(map_region(phase.query, box));
      }
      uv.resize(count);
      const auto& source = phase.query.inputs[0].result_schema->tensors[0];
      const auto& map = phase.query.inputs[1].result_schema->tensors[0];
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0, math_take(Footprint::none(source.sample_shape(), limits)), 8});
      need.tensors.push_back({1, 0,
                              math_take(Footprint::from_regions(
                                  map.sample_shape(), map_boxes, limits)),
                              14});
      stage = 1;
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (stage == 1) {
      builder.emplace(math_take(ResultBuilder::start(
          phase.resources, output, phase.query.semantic_key, {},
          phase.association
              ? std::vector<std::uint64_t>(phase.association->begin(),
                                           phase.association->end())
              : std::vector<std::uint64_t>{},
          phase.query.tile_height, phase.query.tile_width,
          phase.query.resources)));
      std::vector<ResultRelation> descriptors;
      for (std::uint32_t port = 0; port < 2; ++port)
        descriptors.push_back(math_take(ResultRelation::cartesian(
            phase.resources, 1,
            {port, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
      math_require(builder->bind_descriptor_relation(
          math_take(ResultRelation::unite(phase.resources, descriptors))));
      prepare_relations(phase);
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0, source_samples, source_samples.empty() ? 8U : 13U});
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
  for (auto& port : traits.input_schema)
    port.kind = OperationPortKind::Result;
  traits.input_schema[0].result_schema_id = "photospider.image";
  traits.input_schema[0].result_schema_version = 1;
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  set_whole_tensor_output(traits, ElementType::Float32, sizeof(Program));
  traits.outputs[0].key = "value";
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
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
      math_require(
          image_ops::check_image(inputs[0], image_ops::ImageKind::Rgba));
      if (!inputs[1].result_schema ||
          !inputs[1].result_schema->fields.empty() ||
          inputs[1].result_schema->tensors.size() != 1)
        throw Status{ErrorCode::TypeMismatch,
                     "STMap requires one map tensor and no fields"};
      const auto& source = inputs[0].result_schema->tensors[0];
      const auto& map = inputs[1].result_schema->tensors[0];
      if (map.descriptor.shape.size() != 3 || map.descriptor.shape[2] != 2 ||
          !map.facets.empty() ||
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
