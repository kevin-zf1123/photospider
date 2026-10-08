#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace s1_fixture {
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
inline void check(ps::Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
inline ps::ValueFacet profile() {
  return take(ps::encode_semantic(ps::rgba_semantics()));
}
inline ps::SchemaTemplate schema(std::vector<std::uint64_t> shape = {2, 2, 4},
                                 bool image = true) {
  const bool spatial = image || shape.size() == 2;
  ps::SchemaTemplate result;
  result.id = spatial ? "photospider.image" : "example.numeric";
  ps::ResultTensorSpec tensor;
  tensor.key = spatial ? "pixels" : "value";
  tensor.descriptor = {ps::ElementType::Float32, std::move(shape)};
  if (spatial) {
    tensor.batch_axes = {1, 1};
    tensor.layout.spatial = true;
    if (!image)
      tensor.layout.channel_axis.reset();
    tensor.facets = {image
                         ? profile()
                         : take(ps::encode_semantic(ps::coverage_semantics()))};
  }
  result.tensors.push_back(std::move(tensor));
  return result;
}
inline ps::ResultRef tensor(const ps::ResourceBudget& root,
                            const std::vector<float>& pixels,
                            std::vector<std::uint64_t> shape = {2, 2, 4},
                            bool image = true) {
  const auto layout = schema(std::move(shape), image);
  const auto count = take(layout.tensors[0].sample_count());
  if (pixels.size() != count)
    throw std::runtime_error("fixture sample count mismatch");
  auto builder = take(ps::ResultBuilder::start(root, layout, "example.source"));
  check(builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {}))));
  check(builder.publish_tensor(
      0, ps::Region::whole(layout.tensors[0].sample_shape()),
      ps::ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                   pixels.size() * 4),
      take(ps::ResultRelation::cartesian(root, count, {})),
      {true, true, true, true}));
  return take(builder.seal());
}
inline ps::ResultRef scalar(const ps::ResourceBudget& root, float number) {
  return tensor(root, {number}, {1}, false);
}
inline ps::WorkflowInputDeclaration declaration(
    std::uint64_t id, std::string name, const ps::SchemaTemplate& schema) {
  ps::WorkflowInputDeclaration input;
  input.id = id;
  input.name = std::move(name);
  input.result_schema = std::make_shared<ps::SchemaTemplate>(schema);
  return input;
}
inline ps::WorkflowInputDeclaration declaration(std::uint64_t id,
                                                std::string name,
                                                const ps::ResultRef& input) {
  return declaration(id, std::move(name), input.schema());
}
inline std::array<float, 16> input_pixels(bool second) {
  return second ? std::array<float, 16>{.25F,  0, .125F, .5F,  .125F, .25F,
                                        .125F, 1, .5F,   .25F, 0,     1,
                                        0,     0, 0,     0}
                : std::array<float, 16>{.125F, .25F, 0, .5F,  .25F, .125F,
                                        .125F, .5F,  0, .25F, .5F,  1,
                                        0,     0,    0, 0};
}
inline ps::ExecutionBindings bindings(const ps::ResourceBudget& root,
                                      bool second = false) {
  const auto pixels = input_pixels(second);
  return {{{"image", tensor(root, {pixels.begin(), pixels.end()})},
           {"gain", scalar(root, second ? .5F : 2)},
           {"opacity", scalar(root, second ? .25F : .5F)}}};
}
inline ps::WorkflowDocument document() {
  ps::WorkflowDocument result;
  result.inputs = {declaration(1, "image", schema()),
                   declaration(2, "gain", schema({1}, false)),
                   declaration(3, "opacity", schema({1}, false))};
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
  options.execution_mode = ps::ExecutionMode::CpuExact;
  options.output_regions.emplace(
      "result", ps::Region({{0, 1}, {0, 1}, {0, 1}, {1, 1}, {0, 4}}));
  return options;
}
// Independent binary-fraction calculation, with a rounded result at each stage.
inline std::array<float, 16> cpu_reference(bool second) {
  auto pixels = input_pixels(second);
  const float gain = second ? .5F : 2, opacity = second ? .25F : .5F;
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    const float exposed =
        i % 4 == 3 ? pixels[i]
                   : static_cast<float>(static_cast<double>(pixels[i]) * gain);
    pixels[i] = static_cast<float>(static_cast<double>(exposed) * opacity);
  }
  return pixels;
}
inline bool oracle(const ps::ExecutionResult& result, bool second = false) {
  const std::array<float, 16> expected =
      second ? std::array<float, 16>{1.F / 32, 0,        1.F / 64, 1.F / 8,
                                     1.F / 64, 1.F / 32, 1.F / 64, 1.F / 4,
                                     1.F / 16, 1.F / 32, 0,        1.F / 4,
                                     0,        0,        0,        0}
             : std::array<float, 16>{.125F, .25F, 0, .25F, .25F, .125F,
                                     .125F, .25F, 0, .25F, .5F,  .5F,
                                     0,     0,    0, 0};
  const auto reference = cpu_reference(second);
  if (std::memcmp(reference.data(), expected.data(), sizeof(expected)) != 0 ||
      result.results.size() != 1 || !result.results.count("result"))
    return false;
  const auto& output = result.results.at("result");
  if (!output.schema().same_schema(schema()))
    return false;
  const auto descriptor = output.descriptor();
  if (!descriptor.ok() || descriptor.value().tensor_coverage(0).empty())
    return false;
  return descriptor.value()
      .tensor_coverage(0)
      .visit(
          [&](const auto& at) {
            float actual = 0;
            auto status =
                output.read_tensor(descriptor.value(), 0, at, &actual, 4);
            if (!status.ok())
              return status;
            return std::memcmp(&actual,
                               &expected[(at[2] * 2 + at[3]) * 4 + at[4]],
                               4) == 0
                       ? ps::Status::success()
                       : ps::Status{ps::ErrorCode::OperationFailed,
                                    "image oracle mismatch"};
          },
          16)
      .ok();
}
}  // namespace s1_fixture
