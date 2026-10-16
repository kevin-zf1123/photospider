#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Copy {
  std::shared_ptr<std::atomic<unsigned>> executed;
  std::shared_ptr<ResultTensorInputs> captured;
  bool requested = false;
  explicit Copy(std::shared_ptr<std::atomic<unsigned>> count,
                std::shared_ptr<ResultTensorInputs> inputs = {})
      : executed(std::move(count)), captured(std::move(inputs)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false, take(TileSet::range(phase.resources, 0, 1))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double value;
    check(phase.read_tensor(0, 0, {0}, &value, 8));
    ++*executed;
    if (captured)
      *captured = *phase.tensors;
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&value), 8}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
}  // namespace
int main() {
  auto registry = std::make_shared<OperationRegistry>();
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto executed = std::make_shared<std::atomic<unsigned>>(0);
  ValidationDefinition validator;
  validator.identity = {"fixture.range", 1, "strict/1"};
  validator.prepare = [](const ValidationPreparation& input) {
    const auto limit = std::get<double>(input.parameters.at("limit"));
    return Result<std::shared_ptr<const void>>(
        std::make_shared<const double>(limit));
  };
  validator.validate = [calls](const void* state,
                               const ValidationContext& context) {
    ++*calls;
    auto window = context.acquire_tensor(0, Region::whole({1}));
    if (!window.ok())
      return window.status();
    auto run = window.value().row_run({0});
    if (!run.ok())
      return run.status();
    double value;
    std::memcpy(&value, run.value().data, 8);
    return value >= 0 && value <= *static_cast<const double*>(state)
               ? Status::success()
               : Status{ErrorCode::OperationFailed, "range rejected"};
  };
  check(registry->register_validator(validator));
  const auto schema = multi_result::schema();
  OperationDefinition operation;
  operation.key = "fixture.copy_validated";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  auto& port = operation.traits.input_schema[0];
  port.result_schema_id = schema.id;
  port.result_schema_version = schema.version;
  port.validators = {validator.identity};
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].output_schema.validators = {validator.identity};
  operation.traits.parameter_schema = {
      {"limit", OperationParameterType::Float64}};
  operation.start_result = [executed](const auto&, const auto& allocator) {
    return ResultContinuation::make<Copy>(allocator, executed);
  };
  auto captured = std::make_shared<ResultTensorInputs>();
  auto capture = operation;
  capture.key = "fixture.capture";
  capture.traits.input_schema[0].validators.clear();
  capture.traits.outputs[0].output_schema.validators.clear();
  capture.start_result = [executed, captured](const auto&,
                                              const auto& allocator) {
    return ResultContinuation::make<Copy>(allocator, executed, captured);
  };
  check(registry->register_operation(std::move(capture)));
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", schema)};
  document.nodes = {{1,
                     "fixture.copy_validated",
                     {WorkflowInputReference{1}},
                     {{"limit", 5.0}}}};
  document.outputs = {{"value", 1, "value"}};
  ExecutionContext context(registry);
  auto root = take(context.resource_budget());
  // Obtain real host-authorized capabilities, then pair them with another
  // same-schema object in a direct invocation. Validation must cover what the
  // computation will read, not an unrelated object supplied for the port.
  document.nodes[0].operation = "fixture.capture";
  GraphContext capture_graph(document);
  auto capture_plan = take(Compiler(registry).compile(capture_graph));
  auto bad_binding = multi_result::binding(root, "input", -1, schema);
  PS_CHECK(context.execute(capture_plan.plan, {{bad_binding}}).ok());
  PS_CHECK(captured->size() == 1);
  auto good_binding = multi_result::binding(root, "input", 2, schema);
  OperationMetadata input;
  input.result_schema = std::make_shared<const SchemaTemplate>(schema);
  const std::map<std::string, ParameterValue> parameters{{"limit", 5.0}};
  auto prepared = take(registry->prepare_operation("fixture.copy_validated",
                                                   {input}, parameters));
  auto outputs =
      take(infer_operation_outputs(prepared->traits(), {input}, parameters));
  ResultProgramMetadata metadata{{input}, outputs[0]};
  ResultProgramQuery query(metadata, parameters);
  query.prepared = prepared;
  query.semantic_key = "direct.validation";
  auto allocator = root.allocator();
  auto continuation =
      take(registry->start_result("fixture.copy_validated", query, allocator));
  ResultObjectInputs objects;
  ResultProgramPhase phase{query,
                           objects,
                           allocator,
                           root,
                           [&](auto n) { return root.consume({n}); },
                           {}};
  PS_CHECK(continuation.poll(phase).ok());
  objects.emplace(0, good_binding.result);
  phase.tensors = captured.get();
  const auto before_direct = executed->load();
  auto mismatched = continuation.poll(phase);
  PS_CHECK(!mismatched.ok() &&
           mismatched.status().code == ErrorCode::TypeMismatch);
  PS_CHECK(executed->load() == before_direct);
  // The same object may legitimately supply both maps; its validator still
  // checks the numeric input before computation.
  objects[0] = bad_binding.result;
  auto invalid_direct =
      take(registry->start_result("fixture.copy_validated", query, allocator));
  auto bad = invalid_direct.poll(phase);
  PS_CHECK(!bad.ok() && bad.status().code == ErrorCode::InvalidArgument);
  PS_CHECK(executed->load() == before_direct);
  PS_CHECK(context.execute(capture_plan.plan, {{good_binding}}).ok());
  objects[0] = good_binding.result;
  auto valid_direct =
      take(registry->start_result("fixture.copy_validated", query, allocator));
  PS_CHECK(valid_direct.poll(phase).ok());
  auto published = take(valid_direct.poll(phase));
  PS_CHECK(std::holds_alternative<ResultPublication>(published));
  PS_CHECK(
      multi_result::number(std::get<ResultPublication>(published).result) == 2);
  return 0;
}
