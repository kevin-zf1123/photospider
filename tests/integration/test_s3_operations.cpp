#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
ps::Value scalar(float x) {
  std::vector<std::uint8_t> bytes(4);
  std::memcpy(bytes.data(), &x, 4);
  return ps::Value::create({ps::ElementType::Float32, {1}},
                           ps::Region::whole({1}), {0, {4}}, std::move(bytes))
      .take_value();
}
bool close(const ps::Value& value, const std::vector<float>& expected) {
  const auto& shape = value.descriptor().shape;
  const auto& r = value.region().dimensions();
  const std::uint64_t channels = shape.size() == 3 ? 4 : 1;
  for (std::uint64_t y = r[0].offset; y < r[0].offset + r[0].extent; ++y)
    for (std::uint64_t x = r[1].offset; x < r[1].offset + r[1].extent; ++x)
      for (std::uint64_t c = 0; c < channels; ++c) {
        std::vector<std::uint64_t> coord{y, x};
        if (channels == 4)
          coord.push_back(c);
        float actual;
        std::memcpy(&actual,
                    value.bytes().data() + value.byte_address(coord).value(),
                    4);
        if (std::abs(actual - expected[(y * shape[1] + x) * channels + c]) >
            1e-6F)
          return false;
      }
  return true;
}
}  // namespace
int main(int argc, char** argv) {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = argc == 2 ? std::make_shared<OperationRegistry>()
                            : make_default_operation_registry();
  if (argc == 2) {
    PS_CHECK(registry->load_plugin(argv[1]).ok());
    PS_CHECK(registry->freeze().ok());
  }
  Compiler compiler(registry);
  ExecutionContext execution(registry);
  for (bool mask : {false, true}) {
    const std::uint64_t h = 5, w = 7, channels = mask ? 1 : 4;
    const std::vector<std::uint64_t> shape =
        mask ? std::vector<std::uint64_t>{h, w}
             : std::vector<std::uint64_t>{h, w, 4};
    std::vector<float> pixels(h * w * channels);
    for (std::size_t i = 0; i < pixels.size(); ++i)
      pixels[i] = mask         ? static_cast<float>(i % 9) / 8
                  : i % 4 == 3 ? .5F
                               : static_cast<float>(i % 11) / 4;
    std::vector<std::uint8_t> bytes(pixels.size() * 4);
    std::memcpy(bytes.data(), pixels.data(), bytes.size());
    const std::string profile = "rgba;linear-srgb;premultiplied;hwc";
    const std::vector<ValueFacet> facets =
        mask ? std::vector<ValueFacet>{}
             : std::vector<ValueFacet>{
                   {"photospider.image", 1, {profile.begin(), profile.end()}}};
    auto image =
        Value::create(
            {ElementType::Float32, shape}, Region::whole(shape),
            mask ? StridedLayout{0, {28, 4}} : StridedLayout{0, {112, 16, 4}},
            bytes, facets)
            .take_value();
    WorkflowDocument document;
    document.inputs = {{1, "input", image.descriptor(), image.region(),
                        image.layout(), image.facets()}};
    for (std::uint64_t factor : {1, 2, 4, 16}) {
      document.nodes = {{1,
                         mask ? "mask.downsample_box" : "image.downsample_box",
                         {WorkflowInputReference{1}},
                         {{"factor", static_cast<std::int64_t>(factor)}}}};
      document.outputs = {{"result", 1, "value"}};
      GraphContext graph(document);
      PlanningOptions options;
      options.tile_height = 1;
      options.tile_width = 2;
      auto compiled = compiler.compile(graph, options);
      PS_CHECK(compiled.ok());
      auto result =
          execution.execute(compiled.value().plan, {{{"input", image}}});
      PS_CHECK(result.ok());
      const auto oh = (h + factor - 1) / factor, ow = (w + factor - 1) / factor;
      std::vector<double> sums(oh * ow * channels);
      std::vector<unsigned> counts(oh * ow);
      for (std::uint64_t y = 0; y < h; ++y)
        for (std::uint64_t x = 0; x < w; ++x) {
          const auto cell = (y / factor) * ow + x / factor;
          ++counts[cell];
          for (std::uint64_t c = 0; c < channels; ++c)
            sums[cell * channels + c] += pixels[(y * w + x) * channels + c];
        }
      std::vector<float> expected(sums.size());
      for (std::size_t i = 0; i < sums.size(); ++i)
        expected[i] = static_cast<float>(sums[i] / counts[i / channels]);
      PS_CHECK(close(result.value().values.at("result"), expected));
      auto dims =
          Region::whole(result.value().values.at("result").descriptor().shape)
              .dimensions();
      dims[0] = {oh - 1, 1};
      dims[1] = {ow - 1, 1};
      auto tile = compiled.value().plan.tile_plan("result", Region(dims));
      PS_CHECK(tile.ok());
      auto edge = execution.execute(tile.value(), {{{"input", image}}});
      PS_CHECK(edge.ok() && close(edge.value().values.at("result"), expected));
    }
    if (mask)
      continue;
    const std::vector<std::string> names{"x",     "y",    "radius", "red",
                                         "green", "blue", "alpha"};
    const std::vector<float> args{2.5F, 1.5F, 1, .8F, .2F, .1F, .5F};
    ExecutionBindings bindings{{{"input", image}}};
    std::vector<WorkflowInput> inputs{WorkflowInputReference{1}};
    for (std::size_t i = 0; i < args.size(); ++i) {
      auto value = scalar(args[i]);
      document.inputs.push_back({i + 2,
                                 names[i],
                                 value.descriptor(),
                                 value.region(),
                                 value.layout(),
                                 {}});
      bindings.inputs.push_back({names[i], value});
      inputs.push_back(WorkflowInputReference{i + 2});
    }
    document.nodes = {{1, "image.brush_circle", inputs, {}}};
    document.outputs = {{"result", 1, "value"}};
    GraphContext graph(document);
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    auto expected = pixels;
    for (std::uint64_t y = 0; y < h; ++y)
      for (std::uint64_t x = 0; x < w; ++x) {
        if (std::hypot(static_cast<double>(x) + .5 - args[0],
                       static_cast<double>(y) + .5 - args[1]) > args[2])
          continue;
        for (std::uint64_t c = 0; c < 4; ++c) {
          const float source = c == 3 ? args[6] : args[3 + c] * args[6];
          const float back = pixels[(y * w + x) * 4 + c] * (1 - args[6]);
          expected[(y * w + x) * 4 + c] = source + back;
        }
      }
    auto result = execution.execute(compiled.value().plan, bindings);
    PS_CHECK(result.ok() &&
             close(result.value().values.at("result"), expected));
    auto tile = compiled.value().plan.tile_plan(
        "result", Region({{0, 3}, {1, 3}, {0, 4}}));
    PS_CHECK(tile.ok());
    auto regional = execution.execute(tile.value(), bindings);
    PS_CHECK(regional.ok() &&
             close(regional.value().values.at("result"), expected));
    bindings.inputs[3].value = scalar(0);
    PS_CHECK(execution.execute(compiled.value().plan, bindings).status().code ==
             ErrorCode::InvalidArgument);
    bindings.inputs[3].value = scalar(1);
    bindings.inputs[1].value = scalar(std::numeric_limits<float>::quiet_NaN());
    PS_CHECK(!execution.execute(compiled.value().plan, bindings).ok());
  }
  return 0;
}
