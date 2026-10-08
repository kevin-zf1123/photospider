#include <iostream>
#include <string>
#include <vector>

#include "support/result_image_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
void image_split_storage_types() {
  Driver d;
  for (auto type :
       {ElementType::UInt8, ElementType::UInt16, ElementType::Float64}) {
    auto s = schema();
    s.tensors[0].batch_axes = {1, 1};
    s.tensors[0].descriptor = {type, {1, 2, 3}};
    s.tensors[0].facets.clear();
    const auto width = Value::element_size(type);
    std::vector<uint8_t> bytes(6 * width);
    for (uint64_t i = 0; i < 6; ++i) {
      if (type == ElementType::UInt8) {
        bytes[i] = static_cast<uint8_t>(i + 1);
      } else if (type == ElementType::UInt16) {
        const uint16_t value = static_cast<uint16_t>(257 * (i + 1));
        std::memcpy(bytes.data() + i * width, &value, width);
      } else {
        const double value = static_cast<double>(i + 1) + .25;
        std::memcpy(bytes.data() + i * width, &value, width);
      }
    }
    auto builder = take(ResultBuilder::start(d.root, s, "split.dtype"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(d.root, 1, {})))
                .ok(),
            "split dtype source descriptor");
    require(builder
                .publish_tensor(0, Region::whole(s.tensors[0].sample_shape()),
                                ByteView(bytes.data(), bytes.size()),
                                take(ResultRelation::cartesian(d.root, 6, {})),
                                {true, true, true, true})
                .ok(),
            "split dtype source samples");
    auto source = take(builder.seal());
    for (const std::string name : {"full", "left", "right"}) {
      auto output = d.run("image.split_horizontal", {{"image", source}},
                          {{"split_x", int64_t{1}}}, name);
      auto descriptor = take(output.descriptor());
      require(
          descriptor.tensor_coverage(0)
              .visit(
                  [&](const auto& at) {
                    uint8_t actual[8]{};
                    auto status =
                        output.read_tensor(descriptor, 0, at, actual, width);
                    const auto offset =
                        ((at[3] + (name == "right" ? 1 : 0)) * 3 + at[4]) *
                        width;
                    require(status.ok() &&
                                std::memcmp(actual, bytes.data() + offset,
                                            width) == 0,
                            "split preserves the complete dtype byte width");
                    return Status::success();
                  },
                  6)
              .ok(),
          "split dtype traversal");
    }
  }
}
void downsample_oracles() {
  for (bool mask : {false, true})
    with_spatial_oracle(mask, [&](Driver& d, const SchemaTemplate& spec,
                                  const ExecutionBinding& input,
                                  const std::vector<float>& pixels,
                                  const auto& verify) {
      constexpr std::uint64_t height = 5, width = 7;
      const auto channels = mask ? 1U : 4U;
      for (std::uint64_t factor : {1, 2, 4, 16}) {
        const auto rows = (height + factor - 1) / factor,
                   columns = (width + factor - 1) / factor;
        std::vector<double> sums(rows * columns * channels);
        std::vector<unsigned> counts(rows * columns);
        for (std::uint64_t y = 0; y < height; ++y)
          for (std::uint64_t x = 0; x < width; ++x) {
            const auto cell = (y / factor) * columns + x / factor;
            ++counts[cell];
            for (unsigned c = 0; c < channels; ++c)
              sums[cell * channels + c] +=
                  pixels[(y * width + x) * channels + c];
          }
        std::vector<float> expected(sums.size());
        for (std::size_t i = 0; i < sums.size(); ++i)
          expected[i] = static_cast<float>(sums[i] / counts[i / channels]);
        auto prepared =
            d.prepare(mask ? "mask.downsample_box" : "image.downsample_box",
                      {input}, {{"factor", static_cast<std::int64_t>(factor)}});
        auto executed =
            take(d.context->execute(prepared.plan, prepared.bindings));
        auto output = executed.results.at("out");
        const auto shape = output.schema().tensors[0].sample_shape();
        require(shape[2] == rows && shape[3] == columns,
                "ceil downsample dimensions");
        verify(output, Region::whole(shape), expected, columns);
        if (factor == 4) {
          auto changed_dimensions =
              Region::whole(spec.tensors[0].sample_shape()).dimensions();
          changed_dimensions[2] = {3, 2};
          changed_dimensions[3] = {5, 2};
          auto changed = take(Footprint::from_regions(
              spec.tensors[0].sample_shape(), {Region(changed_dimensions)}));
          auto dirty =
              take(executed.dependencies.potential_dirty("source", changed));
          auto expected_dimensions = Region::whole(shape).dimensions();
          expected_dimensions[3] = {1, 1};
          require(dirty.at("out") == take(Footprint::from_regions(
                                         shape, {Region(expected_dimensions)})),
                  "scaled dirty preimage matches clipped cells");
        }
        auto dimensions = Region::whole(shape).dimensions();
        dimensions[2] = {rows - 1, 1};
        dimensions[3] = {columns - 1, 1};
        const Region edge(dimensions);
        auto tile = take(prepared.plan.tile_plan("out", edge));
        verify(
            take(d.context->execute(tile, prepared.bindings)).results.at("out"),
            edge, expected, columns);
      }
      for (std::int64_t factor : {0, 17}) {
        bool rejected = false;
        try {
          d.prepare(mask ? "mask.downsample_box" : "image.downsample_box",
                    {input}, {{"factor", factor}});
        } catch (const std::runtime_error&) {
          rejected = true;
        }
        require(rejected, "out of range shrink factor rejected");
      }
    });
}
}  // namespace
int main() {
  try {
    image_split_storage_types();
    downsample_oracles();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
