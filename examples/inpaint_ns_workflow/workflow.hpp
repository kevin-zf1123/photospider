#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
inline ps::Value value(const std::vector<float>& pixels,
                       const std::vector<std::uint64_t>& shape,
                       const ps::SemanticDescriptor& semantic) {
  auto output = take(ps::MutableValue::allocate(
      {ps::ElementType::Float32, shape}, ps::Region::whole(shape),
      ps::BufferAllocator{}));
  check(output.size() == pixels.size() * 4, "fixture shape");
  std::memcpy(output.data(), pixels.data(), pixels.size() * 4);
  return take(std::move(output).publish({take(ps::encode_semantic(semantic))}));
}
inline ps::Result<ps::ExecutionResult> run(
    const std::string& key, const std::vector<ps::Value>& inputs,
    const std::map<std::string, ps::ParameterValue>& parameters,
    ps::PlanningOptions planning = {}, ps::CancellationToken token = {}) {
  ps::WorkflowDocument doc;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    doc.inputs.push_back({i + 1, "input" + std::to_string(i),
                          inputs[i].descriptor(), inputs[i].region(),
                          inputs[i].layout(), inputs[i].facets()});
  doc.nodes = {{1,
                key,
                {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
                parameters}};
  doc.outputs = {{"result", 1, "image"}};
  auto registry = ps::make_default_operation_registry();
  ps::GraphContext graph(doc);
  auto compiled = ps::Compiler(registry).compile(graph, planning);
  if (!compiled.ok())
    return ps::Result<ps::ExecutionResult>(compiled.status());
  ps::ExecutionBindings bindings;
  for (std::size_t i = 0; i < inputs.size(); ++i)
    bindings.inputs.push_back({"input" + std::to_string(i), inputs[i]});
  ps::ExecutionContext context(registry);
  return context.execute(compiled.value().plan, bindings, token);
}
inline std::vector<float> pixels(const ps::Value& value) {
  const auto dimensions = value.region().dimensions();
  std::vector<std::uint64_t> coordinate;
  for (auto d : dimensions)
    coordinate.push_back(d.offset);
  std::vector<float> result(take(value.region().element_count()));
  for (auto& number : result) {
    const auto address = take(value.byte_address(coordinate));
    std::memcpy(&number, value.bytes().data() + address, 4);
    for (std::size_t axis = coordinate.size(); axis-- > 0;) {
      if (++coordinate[axis] <
          dimensions[axis].offset + dimensions[axis].extent)
        break;
      coordinate[axis] = dimensions[axis].offset;
    }
  }
  return result;
}
}  // namespace inpaint_example
