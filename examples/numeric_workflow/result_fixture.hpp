#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace numeric_result_fixture {
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
inline void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
inline ps::SchemaTemplate source_schema(const ps::Value& value) {
  ps::SchemaTemplate schema;
  schema.id = "manual.lowpass.input";
  ps::ResultTensorSpec member;
  member.key = "data";
  member.descriptor = value.descriptor();
  member.facets = value.facets();
  for (const auto& facet : member.facets)
    if (facet.key == "photospider.color-array")
      member.atomic_trailing_axes = 1;
  schema.tensors.push_back(std::move(member));
  return schema;
}
inline ps::ResultRef source(
    const ps::ResourceBudget& root, const ps::Value& value,
    const ps::SchemaTemplate* override_schema = nullptr) {
  auto schema = override_schema ? *override_schema : source_schema(value);
  auto bytes = take(root.allocator().allocate(value.bytes().size()));
  std::memcpy(bytes.data(), value.bytes().data(), value.bytes().size());
  auto builder = take(ps::ResultBuilder::start(
      root, schema, "lowpass.input", {}, {}, 128, 128, value.resources()));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
              .ok(),
          "lowpass source descriptor");
  require(
      builder
          .publish_tensor(
              0, value.region(), value.layout(), std::move(bytes).freeze(),
              take(ps::ResultRelation::cartesian(
                  root, take(schema.tensors[0].sample_count()), {0, 1, 0, 0})),
              {true, true, true, true})
          .ok(),
      "lowpass source Result publication");
  return take(builder.seal());
}
inline void declare_sources(ps::WorkflowDocument* document,
                            const std::vector<ps::Value>& values) {
  for (std::size_t i = 0; i < values.size(); ++i) {
    ps::WorkflowInputDeclaration input;
    input.id = i + 1;
    input.name = "input" + std::to_string(i);
    input.result_schema =
        std::make_shared<ps::SchemaTemplate>(source_schema(values[i]));
    document->inputs.push_back(std::move(input));
  }
}
inline ps::ExecutionBindings bind_sources(
    const ps::ResourceBudget& root, const std::vector<ps::Value>& values,
    const ps::WorkflowDocument* document = nullptr) {
  ps::ExecutionBindings bindings;
  for (std::size_t i = 0; i < values.size(); ++i)
    bindings.inputs.push_back(
        {"input" + std::to_string(i),
         source(root, values[i],
                document ? document->inputs[i].result_schema.get() : nullptr)});
  return bindings;
}

inline ps::Status read(const ps::ResultRef& result,
                       const std::vector<std::uint64_t>& at, void* bits,
                       std::size_t width) {
  auto descriptor = result.descriptor();
  return descriptor.ok()
             ? result.read_tensor(descriptor.value(), 0, at, bits, width)
             : descriptor.status();
}
inline std::vector<std::uint8_t> bytes(const ps::ResultRef& result) {
  const auto facts = numeric_result_fixture::take(result.descriptor());
  const auto& spec = result.schema().tensors[0];
  const auto count =
      numeric_result_fixture::take(facts.tensor_coverage(0).element_count());
  const auto width = ps::Value::element_size(spec.descriptor.element_type);
  std::vector<std::uint8_t> output(count * width);
  std::uint64_t next = 0;
  for (const auto& box : facts.tensor_coverage(0).boxes()) {
    const auto window =
        numeric_result_fixture::take(result.acquire_tensor(facts, 0, box));
    const auto covered = numeric_result_fixture::take(
        ps::Footprint::from_regions(spec.sample_shape(), {box}));
    auto status = covered.visit(
        [&](const auto& at) {
          const auto row = window.row_run(at);
          if (!row.ok())
            return row.status();
          std::memcpy(output.data() + next++ * width, row.value().data, width);
          return ps::Status::success();
        },
        count);
    if (!status.ok())
      throw std::runtime_error(status.message);
  }
  return output;
}
}  // namespace numeric_result_fixture
