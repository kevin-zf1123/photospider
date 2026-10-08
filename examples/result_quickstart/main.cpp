#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

#include "photospider/photospider.hpp"

namespace {
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void require_ok(const ps::Status& status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
struct Constant final {
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    auto started = ps::ResultBuilder::start(phase.resources,
                                            *phase.query.output.result_schema,
                                            phase.query.semantic_key);
    if (!started.ok())
      return ps::Result<ps::ResultProgramPoll>(started.status());
    auto builder = started.take_value();
    auto relation = ps::ResultRelation::cartesian(phase.resources, 1, {});
    if (!relation.ok())
      return ps::Result<ps::ResultProgramPoll>(relation.status());
    auto status = builder.bind_descriptor_relation(relation.value());
    if (!status.ok())
      return ps::Result<ps::ResultProgramPoll>(status);
    const double number = 42;
    status = builder.publish_tensor(
        0, ps::Region::whole({1}),
        ps::ByteView(reinterpret_cast<const std::uint8_t*>(&number),
                     sizeof(number)),
        relation.take_value(), {true, true, true, true},
        phase.query.cancellation);
    if (!status.ok())
      return ps::Result<ps::ResultProgramPoll>(status);
    auto sealed = builder.seal();
    return sealed.ok() ? ps::Result<ps::ResultProgramPoll>(
                             ps::ResultPublication{sealed.take_value(), true})
                       : ps::Result<ps::ResultProgramPoll>(sealed.status());
  }
};
void run() {
  ps::SchemaTemplate schema;
  schema.id = "example.scalar";
  ps::ResultTensorSpec tensor;
  tensor.key = "number";
  tensor.descriptor = {ps::ElementType::Float64, {1}};
  schema.tensors.push_back(std::move(tensor));
  ps::OperationDefinition operation;
  operation.key = "example.constant";
  operation.traits.input_count = 0;
  operation.traits.input_schema.clear();
  auto& output = operation.traits.outputs[0];
  output.result_schema = schema;
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = "example.scalar";
  output.output_schema.result_schema_version = schema.version;
  output.region_rule = ps::OperationRegionRule::Whole;
  output.continuation_bytes = sizeof(Constant);
  output.maximum_dependency_stages = 1;
  operation.start_result = [](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<Constant>(allocator);
  };
  auto registry = std::make_shared<ps::OperationRegistry>();
  require_ok(registry->register_operation(std::move(operation)));
  require_ok(registry->freeze());
  ps::WorkflowDocument document;
  document.nodes = {{1, "example.constant", {}, {}}};
  document.outputs = {{"answer", 1, "value"}};
  ps::GraphContext graph(document);
  const auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext context(registry);
  const auto executed = take(context.execute(compiled.plan));
  const auto& result = executed.results.at("answer");
  double answer = 0;
  require_ok(result.read_tensor(take(result.descriptor()), 0, {0}, &answer,
                                sizeof(answer)));
  if (answer != 42)
    throw std::runtime_error("constant workflow returned unexpected value");
  std::cout << "answer=" << answer << '\n';
}
}  // namespace
int main() {
  try {
    run();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
