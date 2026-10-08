#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace ps::test_image {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok()) {
    throw std::runtime_error(result.status().message);
  }
  return result.take_value();
}
inline void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
inline SchemaTemplate schema(bool mask = false) {
  SchemaTemplate schema;
  schema.id = "photospider.image";
  ResultTensorSpec slot;
  slot.batch_axes = {1, 1};
  slot.layout.spatial = true;
  slot.key = "pixels";
  slot.batch_axes[0] = slot.batch_axes[1] = 2;
  slot.descriptor = {ElementType::Float32,
                     mask ? std::vector<std::uint64_t>{3, 5}
                          : std::vector<std::uint64_t>{3, 5, 4}};
  if (mask) {
    slot.layout.channel_axis.reset();
  }
  slot.facets = {
      take(encode_semantic(mask ? coverage_semantics() : rgba_semantics()))};
  schema.tensors.push_back(std::move(slot));
  return schema;
}
inline ResultRef image(const ResourceBudget& root, bool mask = false,
                       float bias = 0) {
  const auto s = schema(mask);
  auto builder = take(ResultBuilder::start(root, s, "test.source"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0})))
              .ok(),
          "source descriptor");
  const auto shape = s.tensors[0].sample_shape();
  const auto count = take(s.tensors[0].sample_count());
  std::vector<float> pixels(count);
  for (std::uint64_t i = 0; i < count; ++i) {
    pixels[i] = mask ? .25F : i % 4 == 3 ? .5F : bias + (i / 4) * .125F;
  }
  require(builder
              .publish_tensor(
                  0, Region::whole(shape),
                  ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                           pixels.size() * 4),
                  take(ResultRelation::cartesian(root, count, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "source publication");
  return take(builder.seal());
}
inline ResultRef scalar(const ResourceBudget& root, float number) {
  SchemaTemplate schema;
  schema.id = "test.scalar";
  ResultTensorSpec tensor;
  tensor.key = "control";
  tensor.descriptor = {ElementType::Float32, {1}};
  schema.tensors.push_back(tensor);
  auto builder = take(ResultBuilder::start(root, schema, "test.control"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
              .ok(),
          "control descriptor");
  auto bytes = take(root.allocator().allocate(5));
  std::memcpy(bytes.data() + 1, &number, 4);
  require(builder
              .publish_tensor(
                  0, Region::whole({1}), {1, {INT64_MIN}},
                  std::move(bytes).freeze(),
                  take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "control publication");
  return take(builder.seal());
}
struct Driver {
  std::shared_ptr<OperationRegistry> registry =
      make_default_operation_registry();
  ExecutionContextConfig config;
  std::unique_ptr<ExecutionContext> context;
  ResourceBudget root;
  bool native = false;
  explicit Driver(bool gpu = false) : native(gpu) {
    config.gpu_enabled = gpu;
    config.managed_resources = ResourceLimits{};
    context = std::make_unique<ExecutionContext>(registry, config);
    root = take(context->resource_budget());
  }
  struct Run {
    std::shared_ptr<GraphContext> graph;
    ExecutionPlan plan;
    ExecutionBindings bindings;
  };
  Run prepare(const std::string& operation,
              std::vector<ExecutionBinding> inputs,
              std::map<std::string, ParameterValue> parameters = {},
              const std::string& output = "value") {
    WorkflowDocument document;
    WorkflowNode node;
    node.id = 1;
    node.operation = operation;
    node.parameters = std::move(parameters);
    for (unsigned i = 0; i < inputs.size(); ++i) {
      WorkflowInputDeclaration declaration;
      declaration.id = i + 1;
      declaration.name = inputs[i].name;
      if (inputs[i].result.valid()) {
        declaration.result_schema =
            std::make_shared<SchemaTemplate>(inputs[i].result.schema());
      }
      document.inputs.push_back(std::move(declaration));
      node.inputs.push_back(WorkflowInputReference{i + 1});
    }
    document.nodes.push_back(std::move(node));
    document.outputs = {{"out", 1, output}};
    auto graph = std::make_shared<GraphContext>(document);
    PlanningOptions planning;
    planning.execution_mode =
        native ? ExecutionMode::NativeGpu : ExecutionMode::CpuExact;
    auto compiled = take(Compiler(registry).compile(*graph, planning));
    return {std::move(graph), std::move(compiled.plan), {std::move(inputs)}};
  }
  ResultRef run(const std::string& operation,
                std::vector<ExecutionBinding> inputs,
                std::map<std::string, ParameterValue> parameters = {},
                const std::string& output = "value") {
    auto prepared =
        prepare(operation, std::move(inputs), std::move(parameters), output);
    return take(context->execute(prepared.plan, prepared.bindings))
        .results.at("out");
  }
};
inline float read(const ResultRef& result,
                  std::vector<std::uint64_t> coordinate) {
  float value = 0;
  require(
      result.read_tensor(take(result.descriptor()), 0, coordinate, &value, 4)
          .ok(),
      "read image output");
  return value;
}
inline void with_spatial_oracle(
    bool mask,
    const std::function<void(
        Driver&, const SchemaTemplate&, const ExecutionBinding&,
        const std::vector<float>&,
        const std::function<void(const ResultRef&, const Region&,
                                 const std::vector<float>&, std::uint64_t)>&)>&
        test) {
  Driver d;
  constexpr std::uint64_t height = 5, width = 7;
  const auto channels = mask ? 1U : 4U;
  auto spec = schema(mask);
  spec.tensors[0].batch_axes = {1, 1};
  spec.tensors[0].descriptor.shape =
      mask ? std::vector<std::uint64_t>{height, width}
           : std::vector<std::uint64_t>{height, width, channels};
  std::vector<float> pixels(height * width * channels);
  for (std::size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = mask         ? static_cast<float>(i % 9) / 8
                : i % 4 == 3 ? .5F
                             : static_cast<float>(i % 11) / 4;
  auto builder = take(ResultBuilder::start(d.root, spec, "oracle.spatial"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(d.root, 1, {0, 1, 0, 0})))
              .ok(),
          "oracle descriptor");
  require(builder
              .publish_tensor(
                  0, Region::whole(spec.tensors[0].sample_shape()),
                  ByteView(reinterpret_cast<const std::uint8_t*>(pixels.data()),
                           pixels.size() * 4),
                  take(ResultRelation::cartesian(d.root, pixels.size(),
                                                 {0, 1, 0, 0})),
                  {true, true, true, true})
              .ok(),
          "oracle input");
  ExecutionBinding input{"source", take(builder.seal())};
  const auto verify = [&](const ResultRef& result, const Region& region,
                          const std::vector<float>& expected,
                          std::uint64_t columns) {
    const auto y = region.dimensions()[2], x = region.dimensions()[3];
    for (auto row = y.offset; row < y.offset + y.extent; ++row)
      for (auto column = x.offset; column < x.offset + x.extent; ++column)
        for (unsigned c = 0; c < channels; ++c) {
          std::vector<std::uint64_t> coordinate{0, 0, row, column};
          if (!mask)
            coordinate.push_back(c);
          require(std::abs(read(result, coordinate) -
                           expected[(row * columns + column) * channels + c]) <=
                      1e-6F,
                  "spatial output differs from independent oracle");
        }
  };
  test(d, spec, input, pixels, verify);
}
}  // namespace ps::test_image
