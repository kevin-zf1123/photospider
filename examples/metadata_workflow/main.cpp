#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
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
  const auto region = ps::Region::whole({4});
  auto source = take(ps::Value::create(descriptor, region, {0, {4}}, bytes));
  ps::WorkflowDocument document;
  document.inputs = {{1, "source", descriptor, region, {0, {4}}, {}}};
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
  auto registry = ps::make_default_operation_registry();
  ps::Compiler compiler(registry);
  ps::GraphContext graph(document);
  auto compiled = take(compiler.compile(graph));
  ps::ExecutionContext context(registry);
  auto result = take(context.execute(compiled.plan, {{{"source", source}}}));
  for (const auto* name : {"assigned", "cleaned"}) {
    const auto& value = result.values.at(name);
    for (std::uint64_t i = 0; i < 4; ++i) {
      auto offset = take(value.byte_address({i}));
      if (std::memcmp(value.bytes().data() + offset, &bits[i], 4)) {
        throw std::runtime_error("exact-byte oracle failed");
      }
    }
  }
  if (!source.facets().empty() ||
      result.values.at("assigned").facets().size() != 2 ||
      result.values.at("cleaned").facets().size() != 1) {
    throw std::runtime_error("metadata oracle failed");
  }
  std::cout << "1.6, signaling NaN, -0 and +Inf preserved bit-for-bit; source "
               "unchanged; annotation removed only from cleaned output\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
