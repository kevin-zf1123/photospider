#pragma once

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace s1_fixture {
inline ps::ValueFacet profile() {
  const std::string text = "rgba;linear-srgb;premultiplied;hwc";
  return {"photospider.image", 1, {text.begin(), text.end()}};
}
inline ps::Value value(std::vector<float> pixels,
                       std::vector<std::uint64_t> shape = {2, 2, 4},
                       bool image = true) {
  std::vector<std::int64_t> strides(shape.size());
  std::int64_t stride = 4;
  for (std::size_t i = shape.size(); i > 0; --i) {
    strides[i - 1] = stride;
    stride *= static_cast<std::int64_t>(shape[i - 1]);
  }
  std::vector<std::uint8_t> bytes(pixels.size() * sizeof(float));
  std::memcpy(bytes.data(), pixels.data(), bytes.size());
  auto result = ps::Value::create({ps::ElementType::Float32, shape},
                                  ps::Region::whole(shape), {0, strides},
                                  std::move(bytes),
                                  image ? std::vector<ps::ValueFacet>{profile()}
                                        : std::vector<ps::ValueFacet>{});
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
inline ps::Value scalar(float number) {
  return value({number}, {1}, false);
}
inline ps::WorkflowInputDeclaration declaration(std::uint64_t id,
                                                std::string name,
                                                const ps::Value& input) {
  return {id,
          std::move(name),
          input.descriptor(),
          input.region(),
          input.layout(),
          input.facets()};
}
inline ps::ExecutionBindings bindings(bool second = false) {
  return {{{"image", second ? value({.25F, 0, .125F, .5F, .125F, .25F, .125F, 1,
                                     .5F, .25F, 0, 1, 0, 0, 0, 0})
                            : value({.125F, .25F, 0, .5F, .25F, .125F, .125F,
                                     .5F, 0, .25F, .5F, 1, 0, 0, 0, 0})},
           {"gain", scalar(second ? .5F : 2)},
           {"opacity", scalar(second ? .25F : .5F)}}};
}
inline ps::WorkflowDocument document() {
  auto input = bindings();
  ps::WorkflowDocument result;
  for (std::size_t i = 0; i < input.inputs.size(); ++i)
    result.inputs.push_back(
        declaration(i + 1, input.inputs[i].name, input.inputs[i].value));
  result.nodes = {
      {10,
       "image.exposure_gain",
       {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
       {}},
      {20,
       "image.opacity",
       {ps::WorkflowNodeOutput{10, "value"}, ps::WorkflowInputReference{3}},
       {}}};
  result.outputs = {{"result", 20, "value"}};
  return result;
}
inline ps::PlanningOptions demand() {
  ps::PlanningOptions options;
  options.output_regions.emplace("result",
                                 ps::Region({{0, 1}, {1, 1}, {0, 4}}));
  return options;
}
// Independent, bounded CPU calculation for the exact A/B table. Binary-fraction
// inputs make both stage results exactly representable even before rounding.
// No operation callback, registry, or kernel arithmetic participates here.
inline std::array<float, 16> cpu_reference(bool second) {
  const auto input = bindings(second);
  std::array<float, 16> pixels{};
  std::memcpy(pixels.data(), input.inputs[0].value.bytes().data(),
              sizeof(pixels));
  float gain = 0;
  float opacity = 0;
  std::memcpy(&gain, input.inputs[1].value.bytes().data(), sizeof(gain));
  std::memcpy(&opacity, input.inputs[2].value.bytes().data(), sizeof(opacity));
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    const float exposed =
        index % 4 == 3
            ? pixels[index]
            : static_cast<float>(static_cast<double>(pixels[index]) * gain);
    pixels[index] = static_cast<float>(static_cast<double>(exposed) * opacity);
  }
  return pixels;
}
inline bool oracle(const ps::ExecutionResult& result, bool second = false) {
  // Frozen independently rounded binary fractions from ADR0016.
  const std::array<float, 16> expected =
      second ? std::array<float, 16>{1.F / 32, 0,        1.F / 64, 1.F / 8,
                                     1.F / 64, 1.F / 32, 1.F / 64, 1.F / 4,
                                     1.F / 16, 1.F / 32, 0,        1.F / 4,
                                     0,        0,        0,        0}
             : std::array<float, 16>{.125F, .25F, 0, .25F, .25F, .125F,
                                     .125F, .25F, 0, .25F, .5F,  .5F,
                                     0,     0,    0, 0};
  const auto reference = cpu_reference(second);
  if (std::memcmp(reference.data(), expected.data(), sizeof(expected)) != 0)
    return false;
  const auto found = result.values.find("result");
  if (result.values.size() != 1 || found == result.values.end())
    return false;
  const auto& output = found->second;
  const auto facet = profile();
  if (!output.valid() ||
      output.descriptor().element_type != ps::ElementType::Float32 ||
      output.descriptor().shape != std::vector<std::uint64_t>({2, 2, 4}) ||
      output.region().empty() || output.region().dimensions()[2].offset != 0 ||
      output.region().dimensions()[2].extent != 4 ||
      output.facets().size() != 1 || output.facets()[0].key != facet.key ||
      output.facets()[0].version != facet.version ||
      output.facets()[0].payload != facet.payload)
    return false;
  const auto yd = output.region().dimensions()[0],
             xd = output.region().dimensions()[1];
  if (output.bytes().size() != yd.extent * xd.extent * 16)
    return false;
  for (std::uint64_t y = yd.offset; y < yd.offset + yd.extent; ++y)
    for (std::uint64_t x = xd.offset; x < xd.offset + xd.extent; ++x)
      for (std::uint64_t c = 0; c < 4; ++c) {
        auto address = output.byte_address({y, x, c});
        if (!address.ok() ||
            std::memcmp(output.bytes().data() + address.value(),
                        &expected[(y * 2 + x) * 4 + c], sizeof(float)) != 0)
          return false;
      }
  return true;
}
}  // namespace s1_fixture
