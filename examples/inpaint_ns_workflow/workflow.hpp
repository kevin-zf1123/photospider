#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../numeric_workflow/result_fixture.hpp"
#include "photospider/photospider.hpp"

namespace inpaint_example {
inline void check(bool pass, const std::string& message) {
  if (!pass)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> result) {
  check(result.ok(), result.status().message);
  return result.take_value();
}
inline ps::SchemaTemplate schema(const std::vector<std::uint64_t>& shape,
                                 const ps::SemanticDescriptor& semantic,
                                 std::vector<std::uint64_t> batches = {1, 1}) {
  ps::SchemaTemplate schema;
  schema.id = "photospider.image";
  ps::ResultTensorSpec tensor;
  tensor.key = "pixels";
  tensor.batch_axes.assign(batches.begin(), batches.end());
  tensor.layout.spatial = true;
  tensor.descriptor = {ps::ElementType::Float32, shape};
  tensor.facets = {take(ps::encode_semantic(semantic))};
  if (shape.size() == 2)
    tensor.layout.channel_axis.reset();
  schema.tensors.push_back(std::move(tensor));
  return schema;
}
inline ps::ResultRef source(const ps::ResourceBudget& root,
                            const ps::SchemaTemplate& schema,
                            const ps::StridedLayout& layout,
                            const std::vector<std::uint8_t>& bytes) {
  auto builder = take(ps::ResultBuilder::start(root, schema, "inpaint.source"));
  check(builder
            .bind_descriptor_relation(
                take(ps::ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
            .ok(),
        "source descriptor");
  auto buffer = take(root.allocator().allocate(bytes.size()));
  std::memcpy(buffer.data(), bytes.data(), bytes.size());
  const auto& tensor = schema.tensors[0];
  check(
      builder
          .publish_tensor(0, ps::Region::whole(tensor.sample_shape()), layout,
                          std::move(buffer).freeze(),
                          take(ps::ResultRelation::cartesian(
                              root, take(tensor.sample_count()), {0, 1, 0, 0})),
                          {true, true, true, true})
          .ok(),
      "source publication");
  return take(builder.seal());
}
struct Input {
  ps::SchemaTemplate description;
  ps::StridedLayout layout;
  std::vector<std::uint8_t> data;
};
inline Input value(const std::vector<float>& pixels,
                   const std::vector<std::uint64_t>& shape,
                   const ps::SemanticDescriptor& semantic,
                   std::vector<std::uint64_t> batches = {1, 1}) {
  auto description = schema(shape, semantic, std::move(batches));
  check(take(description.tensors[0].sample_count()) == pixels.size(),
        "fixture shape");
  const auto samples = description.tensors[0].sample_shape();
  ps::StridedLayout layout;
  layout.byte_strides.resize(samples.size());
  std::uint64_t stride = 4;
  for (std::size_t i = samples.size(); i-- > 0;) {
    layout.byte_strides[i] = stride;
    stride *= samples[i];
  }
  std::vector<std::uint8_t> bytes(pixels.size() * 4);
  std::memcpy(bytes.data(), pixels.data(), bytes.size());
  return {std::move(description), std::move(layout), std::move(bytes)};
}
struct Prepared {
  ps::WorkflowDocument document;
  ps::ExecutionBindings bindings;
};
inline Prepared prepare(
    const ps::ResourceBudget& root, const std::string& key,
    const std::vector<Input>& inputs,
    const std::map<std::string, ps::ParameterValue>& parameters) {
  Prepared result;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    ps::WorkflowInputDeclaration input;
    input.id = i + 1;
    input.name = "input" + std::to_string(i);
    input.result_schema =
        std::make_shared<ps::SchemaTemplate>(inputs[i].description);
    result.bindings.inputs.push_back(
        {input.name, source(root, inputs[i].description, inputs[i].layout,
                            inputs[i].data)});
    result.document.inputs.push_back(std::move(input));
  }
  result.document.nodes = {
      {1,
       key,
       {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
       parameters}};
  result.document.outputs = {{"result", 1, "image"}};
  return result;
}
inline ps::ExecutionOptions options() {
  ps::ExecutionOptions options;
  options.dependencies.maximum_work = UINT64_C(1000000000000000);
  options.maximum_dependency_work = UINT64_C(1000000000000000);
  return options;
}
inline ps::Result<ps::ExecutionResult> run(
    const std::string& key, const std::vector<Input>& inputs,
    const std::map<std::string, ps::ParameterValue>& parameters,
    ps::PlanningOptions planning = {}, ps::CancellationToken token = {},
    ps::ResourceLimits limits = {},
    ps::ResourceStatistics* statistics = nullptr) {
  auto registry = ps::make_default_operation_registry();
  ps::ResourceBudget root;
  ps::Result<ps::ExecutionResult> result(
      ps::Status{ps::ErrorCode::Internal, {}});
  {
    ps::ExecutionContextConfig config;
    config.gpu_enabled = false;
    config.managed_resources = limits;
    ps::ExecutionContext context(registry, config);
    root = take(context.resource_budget());
    auto prepared = prepare(root, key, inputs, parameters);
    ps::GraphContext graph(prepared.document);
    auto compiled = ps::Compiler(registry).compile(graph, planning);
    if (!compiled.ok())
      return ps::Result<ps::ExecutionResult>(compiled.status());
    result = context.execute(compiled.value().plan, prepared.bindings, token,
                             options());
  }
  if (statistics)
    *statistics = root.statistics();
  return result;
}

inline std::vector<std::uint8_t> bytes(const ps::ResultRef& result) {
  return numeric_result_fixture::bytes(result);
}
inline std::vector<float> pixels(const ps::ResultRef& result) {
  const auto data = bytes(result);
  std::vector<float> values(data.size() / 4);
  std::memcpy(values.data(), data.data(), data.size());
  return values;
}
inline std::vector<std::uint8_t> bytes(const Input& input) {
  ps::ResourceBudget root;
  return bytes(source(root, input.description, input.layout, input.data));
}
inline std::vector<float> pixels(const Input& input) {
  ps::ResourceBudget root;
  return pixels(source(root, input.description, input.layout, input.data));
}

}  // namespace inpaint_example
