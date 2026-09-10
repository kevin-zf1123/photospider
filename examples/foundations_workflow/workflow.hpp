#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace foundations {
using Parameters = std::map<std::string, ps::ParameterValue>;
inline void require(bool condition, const std::string& reason) {
  if (!condition)
    throw std::runtime_error(reason);
}
template <class T>
T take(ps::Result<T> result) {
  require(result.ok(), result.status().message);
  return result.take_value();
}
template <class T>
ps::Value array(const std::vector<T>& data,
                std::vector<std::uint64_t> shape = {},
                const std::vector<ps::ValueFacet>& facets = {}) {
  if (shape.empty())
    shape = {data.size()};
  const auto type = std::is_same_v<T, float>          ? ps::ElementType::Float32
                    : std::is_same_v<T, double>       ? ps::ElementType::Float64
                    : std::is_same_v<T, std::int64_t> ? ps::ElementType::Int64
                                                      : ps::ElementType::UInt8;
  auto value = take(ps::MutableValue::allocate(
      {type, shape}, ps::Region::whole(shape), ps::BufferAllocator{}));
  require(value.size() == data.size() * sizeof(T),
          "array shape/data size mismatch");
  std::memcpy(value.data(), data.data(), data.size() * sizeof(T));
  return take(std::move(value).publish(facets));
}
template <class T>
void exact(const ps::Value& value, const std::vector<T>& expected) {
  require(value.bytes().size() == expected.size() * sizeof(T) &&
              std::memcmp(value.bytes().data(), expected.data(),
                          expected.size() * sizeof(T)) == 0,
          "exact sample oracle failed");
}
inline void close(const ps::Value& value, const std::vector<float>& expected,
                  double tolerance = 1e-5) {
  require(value.bytes().size() == expected.size() * 4,
          "sample count oracle failed");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    float actual;
    std::memcpy(&actual, value.bytes().data() + i * 4, 4);
    require(
        std::isfinite(actual) && std::abs(actual - expected[i]) <=
                                     tolerance * (1 + std::abs(expected[i])),
        "float sample oracle failed");
  }
}
inline ps::WorkflowDocument document(
    const std::vector<ps::Value>& inputs,
    const std::vector<ps::WorkflowNode>& nodes) {
  ps::WorkflowDocument doc;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    doc.inputs.push_back({i + 1, "input" + std::to_string(i),
                          inputs[i].descriptor(), inputs[i].region(),
                          inputs[i].layout(), inputs[i].facets()});
  doc.nodes = nodes;
  doc.outputs = {{"result", nodes.back().id, "value"}};
  return doc;
}
inline ps::ExecutionBindings bindings(const std::vector<ps::Value>& inputs) {
  ps::ExecutionBindings result;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    result.inputs.push_back({"input" + std::to_string(i), inputs[i]});
  return result;
}
inline ps::Result<ps::ExecutionResult> evaluate(
    const std::vector<ps::Value>& inputs,
    const std::vector<ps::WorkflowNode>& nodes,
    ps::PlanningOptions options = {}) {
  auto registry = ps::make_default_operation_registry();
  ps::GraphContext graph(document(inputs, nodes));
  auto compiled = ps::Compiler(registry).compile(graph, options);
  if (!compiled.ok())
    return ps::Result<ps::ExecutionResult>(compiled.status());
  ps::ExecutionContext execution(registry);
  return execution.execute(compiled.value().plan, bindings(inputs));
}
inline ps::Result<ps::ExecutionResult> operation(
    const std::string& key, const std::vector<ps::Value>& inputs,
    const Parameters& parameters = {}) {
  std::vector<ps::WorkflowInput> refs;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    refs.push_back(ps::WorkflowInputReference{i + 1});
  return evaluate(inputs, {{1, key, refs, parameters}});
}
inline ps::Value output(ps::Result<ps::ExecutionResult> result) {
  return take(std::move(result)).values.at("result");
}
inline void rejected(const ps::Result<ps::ExecutionResult>& result,
                     ps::ErrorCode code) {
  require(!result.ok() && result.status().code == code,
          "expected rejection code was not returned");
}
inline ps::WorkflowNode next(std::uint64_t id, const std::string& key,
                             std::uint64_t input,
                             const Parameters& parameters = {}) {
  return {id, key, {ps::WorkflowNodeOutput{input, "value"}}, parameters};
}
inline std::vector<ps::ValueFacet> facets(
    const ps::SemanticDescriptor& descriptor) {
  return {take(ps::encode_semantic(descriptor))};
}
inline Parameters interpretation(const ps::SemanticDescriptor& descriptor) {
  return {{"semantic", take(ps::semantic_parameter(descriptor))}};
}
inline Parameters indices(std::initializer_list<std::uint32_t> list) {
  return {{"indices", take(ps::channel_indices_parameter(list))}};
}
void numeric();
void channels();
void alpha_color();
void expressions();
void generator_gain();
void components();
}  // namespace foundations
