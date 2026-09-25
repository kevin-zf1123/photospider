#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "photospider/data/planar_image.hpp"
#include "support/test_support.hpp"

namespace {
int layouts() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (const auto type :
       {ElementType::UInt8, ElementType::Int8, ElementType::UInt16,
        ElementType::Int16, ElementType::Float32, ElementType::Int64,
        ElementType::Float64}) {
    const auto size = Value::element_size(type);
    std::vector<unsigned> axes{0, 1, 2};
    do {
      // H/W/C axes vary independently of physical source layout.
      const std::uint64_t height = 5, width = 133, channels = 3;
      ValueDescriptor descriptor{type, {1, 1, 1}};
      descriptor.shape[axes[0]] = height;
      descriptor.shape[axes[1]] = width;
      descriptor.shape[axes[2]] = channels;
      const auto whole = Region::whole(descriptor.shape);
      for (unsigned mode = 0; mode < 6; ++mode) {
        // Interleaved, reversed X, broadcast X, contiguous rows, reversed Y/C.
        const std::int64_t xs = mode == 3 ? size : channels * size;
        const std::int64_t ys = width * xs + 7;
        const std::int64_t cs = mode == 3 ? height * ys + 11 : size;
        std::vector<std::uint8_t> bytes(height * ys + channels * cs + 19);
        for (std::size_t i = 0; i < bytes.size(); ++i)
          bytes[i] = static_cast<std::uint8_t>((i * 37 + 11) & 255);
        StridedLayout source;
        source.byte_strides.resize(3);
        source.origin.assign(3, 0);
        source.origin[axes[0]] = 2;
        source.byte_offset = 3 + (mode == 4 ? height - 3 : 2) * ys +
                             (mode == 1 ? (width - 1) * xs : 0) +
                             (mode == 5 ? (channels - 1) * cs : 0);
        source.byte_strides[axes[0]] = mode == 4 ? -ys : ys;
        source.byte_strides[axes[1]] = mode == 1 ? -xs : mode == 2 ? 0 : xs;
        source.byte_strides[axes[2]] = mode == 5 ? -cs : cs;
        auto input = Value::create(descriptor, whole, source, bytes);
        PS_CHECK(input.ok());
        for (const auto order :
             {ImagePlaneOrder::Continuous, ImagePlaneOrder::Tiled}) {
          PlanarImageConfig config;
          config.order = order;
          config.height_axis = axes[0];
          config.width_axis = axes[1];
          config.channel_axis = axes[2];
          config.tile_height = 2;
          config.tile_width = 128;
          if (order == ImagePlaneOrder::Continuous)
            config.row_pitch_bytes = width * size + 32;
          auto imported = PlanarImage::import_value(input.value(), config);
          PS_CHECK(imported.ok());
          auto window = imported.value().acquire(whole).take_value();
          std::vector<std::uint64_t> at(3);
          for (std::uint64_t c = 0; c < channels; ++c)
            for (std::uint64_t y = 0; y < height; ++y)
              for (std::uint64_t x = 0; x < width;) {
                at[axes[0]] = y;
                at[axes[1]] = x;
                at[axes[2]] = c;
                auto run = window.row_run(at).take_value();
                for (std::uint64_t dx = 0; dx < run.samples; ++dx) {
                  const auto logical_x = x + dx;
                  const auto sx = mode == 1   ? width - 1 - logical_x
                                  : mode == 2 ? 0
                                              : logical_x;
                  const auto sy = mode == 4 ? height - 1 - y : y;
                  const auto sc = mode == 5 ? channels - 1 - c : c;
                  const auto offset = 3 + sy * ys + sx * xs + sc * cs;
                  PS_CHECK(std::memcmp(run.data + dx * size,
                                       bytes.data() + offset, size) == 0);
                }
                x += run.samples;
              }
        }
      }
    } while (std::next_permutation(axes.begin(), axes.end()));
  }
  return 0;
}
int limits() {
  using namespace ps;  // NOLINT(build/namespaces)
  const ValueDescriptor descriptor{ElementType::UInt8, {1, 5001}};
  auto input = Value::create(descriptor, Region::whole(descriptor.shape),
                             {0, {std::numeric_limits<std::int64_t>::min(), 1}},
                             std::vector<std::uint8_t>(5001, 173))
                   .take_value();
  PlanarImageConfig config;
  config.channel_axis.reset();
  config.tile_width = 8192;
  auto image = PlanarImage::import_value(input, config);
  PS_CHECK(image.ok());
  std::vector<std::uint8_t> result(5001);
  PS_CHECK(
      image.value().read(input.region(), result.data(), result.size()).ok());
  PS_CHECK(result == input.copy_bytes());
  config.aggregate_budget = std::make_shared<PlanarPageBudget>(1ULL << 24);
  config.maximum_backed_bytes = 0;
  PS_CHECK(PlanarImage::import_value(input, config).status().code ==
           ErrorCode::ResourceExhausted);
  PS_CHECK(config.aggregate_budget->live_bytes() == 0);
  config.maximum_backed_bytes = 1ULL << 24;
  CancellationSource stop;
  stop.cancel();
  PS_CHECK(
      PlanarImage::import_value(input, config, stop.token()).status().code ==
      ErrorCode::Cancelled);
  PS_CHECK(config.aggregate_budget->live_bytes() == 0);
  return 0;
}
}  // namespace

int main() {
  return layouts() || limits();
}
