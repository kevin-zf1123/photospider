#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "minimal_ops.hpp"  // NOLINT(build/include_subdir)
namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void run() {
  auto operations = std::make_shared<OperationRegistry>();
  if (!unified_example::register_gather(operations.get()).ok() ||
      !operations->freeze().ok())
    throw std::runtime_error("registration failed");
  auto schema =
      std::make_shared<SchemaTemplate>(unified_example::image_schema());
  WorkflowInputDeclaration a;
  a.id = 1;
  a.name = "a";
  a.result_schema = schema;
  auto b = a;
  b.id = 2;
  b.name = "b";
  WorkflowInputDeclaration control;
  control.id = 3;
  control.name = "control";
  control.descriptor = {ElementType::Int64, {2, 2, 2, 4}};
  control.region = Region::whole(control.descriptor.shape);
  control.layout.byte_strides = {128, 64, 32, 8};
  WorkflowDocument document;
  document.inputs = {a, b, control};
  document.nodes = {{1,
                     "example.gather",
                     {WorkflowInputReference{1}, WorkflowInputReference{2},
                      WorkflowInputReference{3}},
                     {}}};
  document.outputs = {{"image", 1, "image"}, {"count", 1, "count"}};
  GraphContext graph(document);
  auto compiled = take(Compiler(operations).compile(graph));
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext execution(operations, config);
  auto root = take(execution.resource_budget());
  ExecutionBinding left;
  left.name = "a";
  left.result = unified_example::input_image(root);
  ExecutionBinding right;
  right.name = "b";
  right.result = unified_example::input_image(root, 5000);
  std::vector<std::int64_t> controls(32, 0);
  auto binding = [&] {
    auto writer = take(MutableValue::allocate(
        control.descriptor, control.region, BufferAllocator{}));
    std::memcpy(writer.data(), controls.data(), controls.size() * 8);
    ExecutionBinding result;
    result.name = "control";
    result.value = take(std::move(writer).publish());
    return result;
  };
  auto selectors = binding();
  auto all = take(execution.execute(compiled.plan, {{left, right, selectors}}));
  std::int64_t count = 0;
  std::memcpy(&count, all.values.at("count").bytes().data(), 8);
  auto q = take(Footprint::from_regions(
      {2, 2, 2, 4}, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}})}));
  auto handle =
      take(execution.open_demand(compiled.plan, {{left, right, selectors}}));
  auto first = take(handle.request({{"image", q}}));
  float before = 0, after = 0;
  auto image = first.results.at("image");
  auto status =
      image.read_image(take(image.descriptor()), 0, {1, 0, 1, 2}, &before, 4);
  if (!status.ok())
    throw std::runtime_error(status.message);
  controls[22] = 3;
  selectors = binding();
  auto update = take(handle.replace_bindings({{left, right, selectors}}));
  auto changed = take(handle.request({{"image", q}}));
  image = changed.results.at("image");
  status =
      image.read_image(take(image.descriptor()), 0, {1, 0, 1, 2}, &after, 4);
  if (!status.ok())
    throw std::runtime_error(status.message);
  if (count != 32 || before != 1012 || after != 6013 ||
      update.potential_dirty.at("image") != q)
    throw std::runtime_error("unexpected workflow result");
  std::cout << "count=" << count << " frame=1 layer=0 y=1 x=2 before=" << before
            << " after=" << after << '\n';
}
}  // namespace
int main() {
  try {
    run();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
