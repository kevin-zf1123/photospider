#include <cstdint>
#include <iostream>
#include <vector>

#include "photospider/data/result.hpp"
#include "photospider/data/tensor.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
int collection_work(bool reverse) {
  constexpr std::uint64_t count = 4096;
  ResourceBudget root;
  TensorDescriptor descriptor{ElementType::UInt8, {count}};
  auto span = TensorSpan::create(descriptor, Region::whole({count}),
                                 {reverse ? count - 1 : 0, {reverse ? -1 : 1}},
                                 std::vector<std::uint8_t>(count, 7));
  PS_REQUIRE_OK(span);
  auto coverage = Footprint::all({count});
  PS_REQUIRE_OK(coverage);
  auto tensor =
      Tensor::create(descriptor, coverage.take_value(), {span.take_value()});
  PS_REQUIRE_OK(tensor);
  std::uint64_t left = 512;
  FootprintLimits limits;
  limits.consume_work = [&](std::uint64_t work) {
    if (work > left)
      return Status{ErrorCode::ResourceExhausted, "copy work limit"};
    left -= work;
    return Status::success();
  };
  auto result =
      tensor.value().collect(Region::whole({count}), root.allocator(), limits);
  PS_CHECK(!result.ok() &&
           result.status().code == ErrorCode::ResourceExhausted);
  return 0;
}
int writer_topology() {
  ResourceBudget root;
  ResultTensorSpec spec;
  spec.key = "value";
  spec.descriptor = {ElementType::UInt8, {1}};
  spec.layout.spatial = true;
  auto writer = TensorWriter::allocate(spec.descriptor, Region::whole({1}),
                                       root.allocator());
  PS_REQUIRE_OK(writer);
  auto writable = writer.take_value();
  auto window = ResultTensorWriteWindow::borrow_dense(spec, writable);
  PS_CHECK(!window.ok());
  return 0;
}
}  // namespace
int main() {
  return collection_work(false) | collection_work(true) | writer_topology();
}
