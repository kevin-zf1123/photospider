#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "image_vertical/image_fixture.hpp"

namespace s4_fixture {
/** @brief Public-API scene and independent whole-image numerical oracle. */
struct Scene {
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
  std::vector<float> expected;
  std::uint64_t height = 13, width = 17, channels = 4;
};
inline void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
inline float foreground(std::uint64_t y, std::uint64_t x, std::uint64_t c,
                        bool signed_data = false) {
  if (signed_data) {
    const float alpha = (x + y) % 11 == 0  ? 0.F
                        : (x + y) % 7 == 0 ? 1.F
                        : (x + y) % 5 == 0 ? 1e-10F
                                           : .5F;
    return c == 3       ? alpha
           : alpha == 0 ? 0.F
           : c == 0     ? -2.F - static_cast<float>(x % 4) * .125F
           : c == 1     ? 4.F + static_cast<float>(y % 3) * .25F
                        : -.125F;
  }
  return c == 0   ? static_cast<float>(x % 4) * .125F
         : c == 1 ? static_cast<float>(y % 3) * .25F
         : c == 2 ? .125F
                  : .5F;
}
inline float background(std::uint64_t c, bool signed_data = false) {
  if (signed_data && c < 3)
    return c == 1 ? 2.F : -1.F;
  return c == 0 ? .25F : c == 1 ? .5F : c == 2 ? .125F : .5F;
}
inline float mask(std::uint64_t y, std::uint64_t x) {
  return static_cast<float>((x + y) % 9) * .125F;
}
inline Scene scene(unsigned kind, int radius = 2, double sigma = 1.25,
                   std::uint64_t factor = 4, bool signed_data = false) {
  Scene s;
  std::vector<float> pixels, masks, back;
  for (std::uint64_t y = 0; y < 13; ++y)
    for (std::uint64_t x = 0; x < 17; ++x) {
      masks.push_back(mask(y, x));
      for (std::uint64_t c = 0; c < 4; ++c) {
        pixels.push_back(foreground(y, x, c, signed_data));
        back.push_back(background(c, signed_data));
      }
    }
  auto add = [&](const std::string& name, ps::Value value) {
    s.bindings.inputs.push_back({name, value});
    s.document.inputs.push_back(
        s1_fixture::declaration(s.document.inputs.size() + 1, name, value));
  };
  add("image", kind == 6 ? s1_fixture::value(masks, {13, 17}, false)
                         : s1_fixture::value(pixels, {13, 17, 4}));
  if (kind < 2)
    add("factor", s1_fixture::scalar(kind == 0 ? 2.F : .5F));
  if (kind == 3)
    add("mask", s1_fixture::value(masks, {13, 17}, false));
  if (kind == 4)
    add("background", s1_fixture::value(back, {13, 17, 4}));
  if (kind == 7) {
    const char* names[] = {"x", "y", "radius", "red", "green", "blue", "alpha"};
    const float args[] = {5.5F,
                          4.5F,
                          3.F,
                          signed_data ? -4.F : .25F,
                          signed_data ? 8.F : .5F,
                          signed_data ? -2.F : .75F,
                          .5F};
    for (int i = 0; i < 7; ++i)
      add(names[i], s1_fixture::scalar(args[i]));
  }
  const char* keys[] = {"image.exposure_gain", "image.opacity",
                        "image.gaussian_blur", "image.mask",
                        "image.source_over",   "image.downsample_box",
                        "mask.downsample_box", "image.brush_circle"};
  ps::WorkflowNode node;
  node.id = 10;
  node.operation = keys[kind];
  for (const auto& input : s.document.inputs)
    node.inputs.push_back(ps::WorkflowInputReference{input.id});
  if (kind == 2)
    node.parameters = {{"radius", static_cast<std::int64_t>(radius)},
                       {"sigma", sigma}};
  if (kind == 5 || kind == 6) {
    node.parameters = {{"factor", static_cast<std::int64_t>(factor)}};
    s.height = 13 / factor + (13 % factor != 0);
    s.width = 17 / factor + (17 % factor != 0);
  }
  if (kind == 6)
    s.channels = 1;
  s.document.nodes = {node};
  s.document.outputs = {{"result", 10, "value"}};
  double weights_total = 0;
  for (int i = -radius; i <= radius; ++i)
    weights_total +=
        std::exp(-static_cast<double>(i * i) / (2 * sigma * sigma));
  for (std::uint64_t y = 0; y < s.height; ++y)
    for (std::uint64_t x = 0; x < s.width; ++x)
      for (std::uint64_t c = 0; c < s.channels; ++c) {
        double value = foreground(y, x, c, signed_data);
        if (kind == 0 && c < 3)
          value *= 2;
        if (kind == 1)
          value *= .5;
        if (kind == 2) {
          value = 0;
          // Independent 2D convolution, without the implementation's horizontal
          // scratch.
          for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx) {
              const auto yy = std::clamp(static_cast<std::int64_t>(y) + dy,
                                         INT64_C(0), INT64_C(12));
              const auto xx = std::clamp(static_cast<std::int64_t>(x) + dx,
                                         INT64_C(0), INT64_C(16));
              const double weight =
                  std::exp(-static_cast<double>(dx * dx + dy * dy) /
                           (2 * sigma * sigma)) /
                  (weights_total * weights_total);
              value += foreground(yy, xx, c, signed_data) * weight;
            }
        }
        if (kind == 3)
          value *= mask(y, x);
        if (kind == 4)
          value += background(c, signed_data) *
                   (1 - foreground(y, x, 3, signed_data));
        if (kind == 5 || kind == 6) {
          value = 0;
          const auto h = std::min(factor, 13 - y * factor),
                     w = std::min(factor, 17 - x * factor);
          for (std::uint64_t yy = y * factor; yy < y * factor + h; ++yy)
            for (std::uint64_t xx = x * factor; xx < x * factor + w; ++xx)
              value +=
                  kind == 6 ? mask(yy, xx) : foreground(yy, xx, c, signed_data);
          value /= static_cast<double>(h * w);
        }
        if (kind == 7) {
          const double dx = static_cast<double>(x) + .5 - 5.5,
                       dy = static_cast<double>(y) + .5 - 4.5;
          if (dx * dx + dy * dy <= 9) {
            const float source[] = {signed_data ? -2.F : .125F,
                                    signed_data ? 4.F : .25F,
                                    signed_data ? -1.F : .375F, .5F};
            value = source[c] + value * .5;
          }
        }
        s.expected.push_back(static_cast<float>(value));
      }
  return s;
}
inline void check(const Scene& s, const ps::Value& value) {
  require(value.facets().size() == 1, "output must retain one typed facet");
  auto semantic = ps::decode_semantic(value.facets()[0]);
  require(semantic.ok() && semantic.value().kind ==
                               (s.channels == 4 ? ps::SemanticKind::Image
                                                : ps::SemanticKind::Mask),
          "output typed semantics changed");
  const auto y = value.region().dimensions()[0],
             x = value.region().dimensions()[1];
  for (auto row = y.offset; row < y.offset + y.extent; ++row)
    for (auto col = x.offset; col < x.offset + x.extent; ++col)
      for (std::uint64_t c = 0; c < s.channels; ++c) {
        auto coordinate = s.channels == 4
                              ? std::vector<std::uint64_t>{row, col, c}
                              : std::vector<std::uint64_t>{row, col};
        auto offset = value.byte_address(coordinate);
        require(offset.ok(), "invalid result layout");
        float actual;
        std::memcpy(&actual, value.bytes().data() + offset.value(), 4);
        const auto expected =
            s.expected[(row * s.width + col) * s.channels + c];
        require(std::isfinite(actual) && std::abs(actual - expected) <=
                                             1e-6F + 1e-5F * std::abs(expected),
                "independent image oracle mismatch");
      }
}
inline std::uint64_t all_operations(
    ps::ExecutionContext& execution,
    const std::shared_ptr<ps::OperationRegistry>& operations,
    ps::ExecutionMode mode, std::uint64_t* fallback_count = nullptr) {
  ps::Compiler compiler(operations);
  if (fallback_count)
    *fallback_count = 0;
  std::uint64_t dispatches = 0;
  for (unsigned scenario = 0; scenario < 16; ++scenario) {
    const auto kind = scenario % 8;
    auto s = scene(kind, 2, 1.25, 4, scenario >= 8);
    ps::GraphContext graph(s.document);
    for (bool tiled : {false, true}) {
      ps::PlanningOptions options;
      options.execution_mode = mode;
      if (tiled) {
        options.tile_height = 2;
        options.tile_width = 3;
        std::vector<ps::RegionDimension> dims = {{1, s.height - 1},
                                                 {1, s.width - 1}};
        if (s.channels == 4)
          dims.push_back({0, 4});
        options.output_regions = {{"result", ps::Region(dims)}};
      }
      auto compiled = compiler.compile(graph, options);
      require(compiled.ok(), compiled.status().message);
      auto result = execution.execute(compiled.value().plan, s.bindings);
      require(result.ok(), result.status().message);
      check(s, result.value().values.at("result"));
      if (mode == ps::ExecutionMode::MetalFp32 && execution.gpu_enabled()) {
        require(result.value().diagnostics.native_dispatch_count > 0,
                std::string("missing native dispatch: ") +
                    s.document.nodes[0].operation);
        require(result.value().diagnostics.fallback_reasons.empty(),
                "unexpected numeric fallback");
      }
      dispatches += result.value().diagnostics.native_dispatch_count;
      if (fallback_count)
        *fallback_count += result.value().diagnostics.fallback_reasons.size();
    }
  }
  return dispatches;
}
/** @brief Checks unsupported numeric inputs and exact large-coordinate
 * coverage. */
inline void numeric_edges(
    ps::ExecutionContext& execution,
    const std::shared_ptr<ps::OperationRegistry>& operations) {
  ps::Compiler compiler(operations);
  ps::ExecutionContext cpu(operations);
  for (unsigned kind : {0U, 2U, 5U}) {
    auto s = scene(kind, 64, 8, 16);
    std::vector<float> pixels(13 * 17 * 4, 1);
    for (std::size_t i = 0; i < pixels.size(); ++i)
      if (i % 4 != 3)
        pixels[i] = kind == 0   ? std::numeric_limits<float>::denorm_min()
                    : kind == 2 ? std::numeric_limits<float>::max()
                                : std::numeric_limits<float>::max() / 2;
    s.bindings.inputs[0].value = s1_fixture::value(pixels, {13, 17, 4});
    ps::GraphContext graph(s.document);
    ps::PlanningOptions options;
    auto exact = compiler.compile(graph, options);
    require(exact.ok(), exact.status().message);
    auto expected = cpu.execute(exact.value().plan, s.bindings);
    options.execution_mode = ps::ExecutionMode::MetalFp32;
    auto native = compiler.compile(graph, options);
    require(native.ok(), native.status().message);
    auto actual = execution.execute(native.value().plan, s.bindings);
    require(expected.ok() == actual.ok(), "numeric fallback changed success");
    if (actual.ok()) {
      require(actual.value().values.at("result").bytes() ==
                  expected.value().values.at("result").bytes(),
              "numeric fallback not exact");
      require(!actual.value().diagnostics.fallback_reasons.empty(),
              "missing numeric fallback reason");
    } else {
      require(actual.status().code == expected.status().code,
              "numeric fallback changed error");
    }
  }
  for (unsigned kind : {2U, 5U, 6U}) {
    auto s = scene(kind, 64, 64, 16);
    ps::GraphContext graph(s.document);
    ps::PlanningOptions options;
    options.execution_mode = ps::ExecutionMode::MetalFp32;
    auto compiled = compiler.compile(graph, options);
    require(compiled.ok(), compiled.status().message);
    auto result = execution.execute(compiled.value().plan, s.bindings);
    require(result.ok(), result.status().message);
    check(s, result.value().values.at("result"));
    if (execution.gpu_enabled())
      require(result.value().diagnostics.native_dispatch_count > 0,
              "max radius/factor failed to dispatch");
  }
  auto s = scene(7);
  const std::uint64_t x = 8388608;
  const std::vector<std::uint64_t> shape{1, x + 1, 4};
  const ps::Region region({{0, 1}, {x, 1}, {0, 4}});
  auto storage = ps::BufferAllocator().allocate(16).take_value();
  const float pixel[4] = {.125F, .25F, .5F, 1};
  std::memcpy(storage.data(), pixel, 16);
  auto value = ps::Value::from_storage(
      {ps::ElementType::Float32, shape}, region, {0, {16, 16, 4}, {0, x, 0}},
      std::move(storage).freeze(), {s1_fixture::profile()});
  require(value.ok(), value.status().message);
  auto regional = std::make_shared<ps::RegionalSource>();
  regional->descriptor = value.value().descriptor();
  regional->facets = value.value().facets();
  regional->read = [](const ps::Region& demand, std::uint8_t* data,
                      std::uint64_t size, const ps::BufferAllocator&,
                      const ps::CancellationToken&) {
    if (size != 16)
      return ps::Result<ps::Region>(
          ps::Status::failure(ps::ErrorCode::InvalidArgument,
                              "large coordinate test expects one pixel"));
    const float source_pixel[4] = {.125F, .25F, .5F, 1};
    std::memcpy(data, source_pixel, 16);
    return ps::Result<ps::Region>(demand);
  };
  s.bindings.inputs[0].value = {};
  s.bindings.inputs[0].source = regional;
  s.bindings.inputs[1].value = s1_fixture::scalar(static_cast<float>(x));
  s.bindings.inputs[2].value = s1_fixture::scalar(.5F);
  s.bindings.inputs[3].value = s1_fixture::scalar(.25F);
  s.document.inputs[0] = {1,
                          "image",
                          {ps::ElementType::Float32, shape},
                          ps::Region::whole(shape),
                          {0, {static_cast<std::int64_t>((x + 1) * 16), 16, 4}},
                          {s1_fixture::profile()}};
  ps::GraphContext graph(s.document);
  ps::PlanningOptions options;
  options.execution_mode = ps::ExecutionMode::MetalFp32;
  options.output_regions = {{"result", region}};
  auto compiled = compiler.compile(graph, options);
  require(compiled.ok(), compiled.status().message);
  auto result = execution.execute(compiled.value().plan, s.bindings);
  require(result.ok(), result.status().message);
  require(std::memcmp(result.value().values.at("result").bytes().data(), pixel,
                      16) == 0,
          "large-coordinate circle coverage changed");
  if (execution.gpu_enabled())
    require(result.value().diagnostics.native_dispatch_count == 1,
            "large-coordinate stamp did not use Metal");
}

}  // namespace s4_fixture
