#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
ps::Value field(const std::vector<float>& samples,
                const std::vector<std::uint64_t>& shape) {
  auto writer = ps::MutableValue::allocate({ps::ElementType::Float32, shape},
                                           ps::Region::whole(shape),
                                           ps::BufferAllocator{})
                    .take_value();
  std::memcpy(writer.data(), samples.data(), samples.size() * sizeof(float));
  return std::move(writer).publish().take_value();
}

int generic_roi() {
  using namespace ps;  // NOLINT(build/namespaces)
  const std::vector<std::uint64_t> shape{4, 5};
  std::vector<float> samples(20);
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = static_cast<float>(i);
  samples.back() = std::numeric_limits<float>::quiet_NaN();
  const auto input = field(samples, shape);
  const auto kernel = field({1, 2, 3}, {1, 3});
  WorkflowDocument document;
  document.inputs = {
      {1, "field", input.descriptor(), input.region(), input.layout(), {}},
      {2, "kernel", kernel.descriptor(), kernel.region(), kernel.layout(), {}}};
  document.nodes = {{1,
                     "field.convolve",
                     {WorkflowInputReference{1}, WorkflowInputReference{2}},
                     {{"anchor_y", INT64_C(0)},
                      {"anchor_x", INT64_C(1)},
                      {"boundary", std::string("zero")}}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto registry = make_default_operation_registry();
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry);
  auto frozen = execution.freeze(compiled.value().plan,
                                 {{{"field", input}, {"kernel", kernel}}});
  PS_CHECK(frozen.ok());
  const auto roi =
      Footprint::from_regions(shape, {Region({{1, 1}, {1, 1}})}).take_value();
  auto result = execution.execute_fragments(frozen.value(), {{"value", roi}});
  PS_CHECK(result.ok());
  float observed = 0;
  PS_CHECK(result.value()
               .values.at("value")
               .read({1, 1}, &observed, sizeof(observed))
               .ok());
  // Convolution reverses [1,2,3]: 3*5 + 2*6 + 1*7 = 34.
  PS_CHECK(observed == 34);
  const auto remote =
      Footprint::from_regions(shape, {Region({{3, 1}, {4, 1}})}).take_value();
  auto dirty = result.value().dependencies.potential_dirty("field", remote);
  PS_CHECK(dirty.ok() && dirty.value().at("value").empty());
  return 0;
}
}  // namespace

int main() {
  return generic_roi();
}
