#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"

namespace {
template <class T>
T take(ps::Result<T> value) {
  if (!value.ok()) {
    throw std::runtime_error(value.status().message);
  }
  return value.take_value();
}
}  // namespace
int main() try {
  const std::array<std::uint32_t, 4> bits{0x3fcccccd, 0x7f800001, 0x80000000,
                                          0x7f800000};
  std::vector<std::uint8_t> bytes(sizeof(bits));
  std::memcpy(bytes.data(), bits.data(), bytes.size());
  const ps::ValueDescriptor descriptor{ps::ElementType::Float32, {4}};
  auto registry = ps::make_default_operation_registry();
  ps::ExecutionContext context(registry);
  const auto root = take(context.resource_budget());
  ps::SchemaTemplate schema;
  schema.id = "example.metadata";
  ps::ResultTensorSpec tensor;
  tensor.key = "samples";
  tensor.descriptor = descriptor;
  schema.tensors.push_back(std::move(tensor));
  auto builder = take(ps::ResultBuilder::start(root, schema, "source"));
  auto status = builder.bind_descriptor_relation(
      take(ps::ResultRelation::cartesian(root, 1, {0, 8, 0, 0})));
  if (!status.ok())
    throw std::runtime_error(status.message);
  auto storage = take(root.allocator().allocate(bytes.size()));
  std::memcpy(storage.data(), bytes.data(), bytes.size());
  status = builder.publish_tensor(
      0, ps::Region::whole({4}), {0, {4}}, std::move(storage).freeze(),
      take(ps::ResultRelation::cartesian(root, 4, {0, 1, 0, 0})),
      {true, true, true, true});
  if (!status.ok())
    throw std::runtime_error(status.message);
  auto source = take(builder.seal());
  ps::WorkflowDocument document;
  ps::WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "source";
  input.result_schema = std::make_shared<const ps::SchemaTemplate>(schema);
  document.inputs.push_back(std::move(input));
  ps::format::MetadataOptions options;
  options.set = {
      {"/semantic/component",
       ps::TensorChannelDescription{"coverage", "coverage", "ratio"}},
      {"/annotations/app.note",
       ps::ValueFacet{"app.note", 1, {'d', 'e', 'm', 'o'}}}};
  auto assigned = take(ps::format::assign_metadata(
      document, ps::WorkflowInputReference{1}, options));
  auto removed = take(ps::format::remove_metadata(document, assigned,
                                                  {"/annotations/app.note"}));
  document.outputs = {{"assigned", assigned.source_node, "values"},
                      {"cleaned", removed.source_node, "values"}};
  ps::Compiler compiler(registry);
  ps::GraphContext graph(document);
  auto compiled = take(compiler.compile(graph));
  auto result = take(context.execute(compiled.plan, {{{"source", source}}}));
  for (const auto* name : {"assigned", "cleaned"}) {
    const auto& value = result.results.at(name);
    for (std::uint64_t i = 0; i < 4; ++i) {
      std::uint32_t actual = 0;
      status = value.read_tensor(take(value.descriptor()), 0, {i}, &actual, 4);
      if (!status.ok())
        throw std::runtime_error(status.message);
      if (actual != bits[i]) {
        throw std::runtime_error("exact-byte oracle failed");
      }
    }
  }
  if (!source.schema().tensors[0].facets.empty() ||
      result.results.at("assigned").schema().tensors[0].facets.size() != 2 ||
      result.results.at("cleaned").schema().tensors[0].facets.size() != 1) {
    throw std::runtime_error("metadata oracle failed");
  }
  std::cout << "1.6, signaling NaN, -0 and +Inf preserved bit-for-bit; source "
               "unchanged; annotation removed only from cleaned output\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
