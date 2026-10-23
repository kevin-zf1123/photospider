#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "03-generation/paths.hpp"
#include "common/registry.hpp"

namespace ps::plugin_internal::generation {
namespace {
std::uint64_t periodic(__int128 value, std::uint64_t extent) {
  auto result = value % static_cast<__int128>(extent);
  return static_cast<std::uint64_t>(result < 0 ? result + extent : result);
}
std::vector<RegionDimension> periodic_support(__int128 first,
                                              std::uint64_t count,
                                              std::uint64_t extent) {
  if (count >= extent)
    return {{0, extent}};
  const auto start = periodic(first, extent);
  const auto tail = std::min(count, extent - start);
  std::vector<RegionDimension> result{{start, tail}};
  if (tail < count)
    result.push_back({0, count - tail});
  return result;
}
// The quotient is in [2^-41,1). Numerator and denominator are exact integers.
// Scaling to the target significand fits 94 bits, so one integer division and
// a remainder comparison implement RN32/RN64 without double rounding.
std::uint64_t rank_midpoint(std::uint64_t rank, std::uint64_t count,
                            bool narrow) {
  const std::uint64_t numerator = 2 * rank + 1, denominator = 2 * count;
  int exponent = __builtin_clzll(denominator) - __builtin_clzll(numerator);
  if ((static_cast<unsigned __int128>(numerator) << -exponent) < denominator)
    --exponent;
  const unsigned precision = narrow ? 24 : 53;
  const auto scaled = static_cast<unsigned __int128>(numerator)
                      << (precision - 1 - exponent);
  std::uint64_t significand = static_cast<std::uint64_t>(scaled / denominator);
  const auto remainder = scaled % denominator;
  if (2 * remainder > denominator ||
      (2 * remainder == denominator && (significand & 1)))
    ++significand;
  if (significand == (UINT64_C(1) << precision)) {
    significand >>= 1;
    ++exponent;
  }
  const auto bias = narrow ? 127 : 1023;
  auto result =
      (static_cast<std::uint64_t>(exponent + bias) << (precision - 1)) |
      (significand - (UINT64_C(1) << (precision - 1)));
  const auto one = narrow ? UINT64_C(0x3f800000) : UINT64_C(0x3ff0000000000000);
  if (result == one)
    --result;
  return result ? result : 1;
}
enum class RankKind { Tile, Points, Volume };
struct RankNoise final : Program {
  RankKind kind;
  Canvas canvas;
  std::uint64_t height, width, frames = 1, count, threshold = 0, capacity = 0;
  std::int64_t offset_x, offset_y, offset_t, frame;
  DenseLayout layout;
  RankNoise(RankKind selected, const std::vector<OperationMetadata>& in,
            const Parameters& p)
      : kind(selected),
        canvas(p),
        offset_x(integer(p, "offset_x")),
        offset_y(integer(p, "offset_y")),
        offset_t(integer(p, "offset_t")),
        frame(integer(p, "frame")),
        layout(take(ps::generation::dynamic_points_schema(1))) {
    const auto& field = tensor(in[0]);
    const auto dimensions = field.sample_shape();
    require(field.descriptor.element_type == ElementType::Int64 &&
                dimensions.size() == (kind == RankKind::Volume ? 3U : 2U),
            "rank lookup requires an Int64 tile or volume");
    height = dimensions[dimensions.size() - 2];
    width = dimensions.back();
    count = size_product(dimensions);
    if (kind == RankKind::Volume)
      frames = dimensions[0];
    const auto hash = text(p, "resource_sha256");
    require(hash.size() == 64 && std::all_of(hash.begin(), hash.end(),
                                             [](char c) {
                                               return (c >= '0' && c <= '9') ||
                                                      (c >= 'a' && c <= 'f');
                                             }),
            "rank resource requires a canonical SHA-256 descriptor");
    require(integer(p, "resource_byte_length") ==
                    static_cast<std::int64_t>(8 * count) &&
                text(p, "resource_encoding", "little_endian_int64") ==
                    "little_endian_int64",
            "rank resource descriptor structure");
    // Content, permutation and the identity binding are caller premises.
    // The immutable input Result owns and accounts for all demanded rank tiles.
    if (kind == RankKind::Points) {
      complete = true;
      threshold = integer(p, "threshold_count");
      capacity = integer(p, "max_count", 1048576);
      const auto schema = take(ps::generation::dynamic_points_schema(capacity));
      layout = DenseLayout(schema);
      scratch_bytes = layout.bytes;
      add_output(this, schema);
    } else {
      add_output(this, tensor_schema(canvas.type, {canvas.height, canvas.width},
                                     true));
    }
  }
  std::vector<ReadRegion> support(unsigned,
                                  const Region& region) const override {
    if (kind == RankKind::Points && !threshold)
      return {};
    const auto y = kind == RankKind::Points ? RegionDimension{0, canvas.height}
                                            : region.dimensions()[0];
    const auto x = kind == RankKind::Points ? RegionDimension{0, canvas.width}
                                            : region.dimensions()[1];
    std::vector<ReadRegion> result;
    for (const auto row : periodic_support(
             static_cast<__int128>(canvas.origin_y) + offset_y + y.offset,
             y.extent, height))
      for (const auto column : periodic_support(
               static_cast<__int128>(canvas.origin_x) + offset_x + x.offset,
               x.extent, width)) {
        std::vector<RegionDimension> dimensions;
        if (kind == RankKind::Volume)
          dimensions.push_back(
              {periodic(static_cast<__int128>(frame) + offset_t, frames), 1});
        dimensions.push_back(row);
        dimensions.push_back(column);
        result.push_back({0, 0, Region(std::move(dimensions))});
      }
    return result;
  }
  std::uint64_t lookup(const Reader& reader, std::uint64_t x,
                       std::uint64_t y) const {
    Coordinate at{};
    const unsigned first = kind == RankKind::Volume ? 1 : 0;
    if (first)
      at[0] = periodic(static_cast<__int128>(frame) + offset_t, frames);
    at[first] =
        periodic(static_cast<__int128>(canvas.origin_y) + offset_y + y, height);
    at[first + 1] =
        periodic(static_cast<__int128>(canvas.origin_x) + offset_x + x, width);
    return reader.bits(0, 0, at);
  }
  void run(const Kernel& kernel,
           const ps_kernel_invocation_v8& invocation) const override {
    const Work work{invocation, work_limit};
    const Reader reader{kernel, invocation};
    if (kind != RankKind::Points) {
      samples(invocation.outputs[0], [&](const Coordinate& at, std::uint64_t) {
        work.poll(8);
        store(invocation.outputs[0], at,
              rank_midpoint(lookup(reader, at[1], at[0]), count,
                            canvas.type == ElementType::Float32));
      });
      return;
    }
    const Dense points{
        layout, static_cast<std::uint8_t*>(invocation.scratch[0].allocation),
        work};
    points.clear();
    std::uint64_t selected = 0;
    if (threshold) {
      for (std::uint64_t y = 0; y < canvas.height; ++y)
        for (std::uint64_t x = 0; x < canvas.width; ++x) {
          work.poll(8);
          if (lookup(reader, x, y) >= threshold)
            continue;
          if (selected == capacity)
            throw Status{ErrorCode::ResourceExhausted,
                         "rank point output capacity exhausted",
                         FailureReason::CapacityLimit};
          const auto center = [](std::int64_t origin, std::uint64_t index) {
            const __int128 twice =
                2 * static_cast<__int128>(origin) + 2 * index + 1;
            return bits(static_cast<double>(twice) * .5);
          };
          points.set(0, 2 * selected, center(canvas.origin_x, x));
          points.set(0, 2 * selected + 1, center(canvas.origin_y, y));
          points.set(1, selected, selected);
          ++selected;
        }
    }
    points.set(2, 0, selected);
    points.publish(kernel, invocation);
  }
};
}  // namespace
}  // namespace ps::plugin_internal::generation

namespace ps::plugin_internal {
Status register_rank_noise(OperationRegistry* registry) {
  using generation::canvas_parameters;
  using generation::check;
  using generation::operation;
  using generation::RankKind;
  using generation::RankNoise;
  using generation::take;
  using generation::tensor_schema;
  try {
    for (auto kind : {RankKind::Tile, RankKind::Points, RankKind::Volume}) {
      auto parameters = canvas_parameters(kind != RankKind::Points);
      parameters.push_back({"offset_x", OperationParameterType::Int64, false});
      parameters.push_back({"offset_y", OperationParameterType::Int64, false});
      parameters.push_back({"resource_sha256", OperationParameterType::String});
      parameters.push_back(
          {"resource_byte_length", OperationParameterType::Int64});
      parameters.push_back(
          {"resource_encoding", OperationParameterType::String, false});
      if (kind == RankKind::Points) {
        parameters.push_back(
            {"threshold_count", OperationParameterType::Int64});
        parameters.push_back(
            {"max_count", OperationParameterType::Int64, false});
      }
      if (kind == RankKind::Volume) {
        parameters.push_back({"frame", OperationParameterType::Int64});
        parameters.push_back(
            {"offset_t", OperationParameterType::Int64, false});
      }
      const auto name = kind == RankKind::Tile     ? "rank_tile"
                        : kind == RankKind::Points ? "rank_threshold_points"
                                                   : "rank_volume";
      const auto schema = kind == RankKind::Points
                              ? take(ps::generation::dynamic_points_schema(1))
                              : tensor_schema(ElementType::Float64, {1, 1});
      check(registry->register_operation(
          operation(std::string("noise.") + name + "_v1_strict", 1, parameters,
                    {{kind == RankKind::Points ? "points" : "values", schema}},
                    [kind](const auto& in, const auto& p) {
                      return std::make_shared<RankNoise>(kind, in, p);
                    })));
    }
    return Status::success();
  } catch (const Status& status) {
    return status;
  }
}
}  // namespace ps::plugin_internal
