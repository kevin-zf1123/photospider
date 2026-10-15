#include <array>
#include <cstring>
#include <memory>
#include <utility>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"
namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Report {
  bool malformed;
  bool requested = false;
  explicit Report(bool value) : malformed(value) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& p) {
    if (!requested && !p.query.inputs.empty()) {
      requested = true;
      return Result<ResultProgramPoll>(
          ResultProgramNeed{{{0, true, false, {}}}});
    }
    auto builder = take(ResultBuilder::start(
        p.resources, *p.query.output.result_schema, p.query.semantic_key));
    const std::array<double, 9> values{
        malformed ? -1.0 : 1.0, 2, 3, 4, 5, 6, 7, 8, 9};
    if (!p.query.output.result_schema->fields.empty()) {
      check(
          builder.append(0, 1,
                         {reinterpret_cast<const std::uint8_t*>(values.data()),
                          sizeof(values)}));
      check(builder.publish(0, 1));
    }
    for (unsigned slot = 0; slot < p.query.output.result_schema->tensors.size();
         ++slot)
      check(builder.publish_tensor(
          slot, Region::whole({1}),
          {reinterpret_cast<const std::uint8_t*>(values.data()), 8}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
}  // namespace
int exercise(bool fields, unsigned tensors) {
  SchemaTemplate schema;
  schema.id = "fixture.report";
  if (fields)
    schema.fields = {
        {"record", ElementType::Float64, {ResultExtentKind::Fixed, 1}, {9}}};
  for (unsigned slot = 0; slot < tensors; ++slot) {
    auto tensor = multi_result::schema().tensors[0];
    tensor.key = "tensor" + std::to_string(slot);
    schema.tensors.push_back(std::move(tensor));
  }
  PS_REQUIRE_OK(schema.validate(true));
  auto registry = std::make_shared<OperationRegistry>();
  ValidationDefinition validator;
  validator.identity = {"fixture.report", 1, "positive-record/1"};
  validator.requires_complete = true;
  validator.prepare = [schema](const ValidationPreparation& p)
      -> Result<std::shared_ptr<const void>> {
    if (!p.schema.same_schema(schema))
      return Result<std::shared_ptr<const void>>(
          Status{ErrorCode::TypeMismatch, {}});
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  validator.validate = [](const void*, const ValidationContext& c) {
    double first = 1;
    if (!c.complete)
      return Status{ErrorCode::TypeMismatch, "incomplete report"};
    if (!c.schema.fields.empty()) {
      auto field = c.read_field(0, 0, 1);
      if (!field.ok())
        return field.status();
      std::memcpy(&first, field.value()->bytes().data(), 8);
    }
    for (unsigned slot = 0; slot < c.schema.tensors.size(); ++slot) {
      auto window = c.acquire_tensor(slot, Region::whole({1}));
      if (!window.ok())
        return window.status();
      auto row = window.value().row_run({0});
      if (!row.ok())
        return row.status();
      std::memcpy(&first, row.value().data, 8);
      if (first <= 0)
        return Status{ErrorCode::OperationFailed, "negative tensor"};
    }
    return first > 0 ? Status::success()
                     : Status{ErrorCode::OperationFailed, "negative report"};
  };
  check(registry->register_validator(validator));
  for (bool bad : {false, true}) {
    OperationDefinition op;
    op.key = bad ? "fixture.bad" : "fixture.good";
    op.traits.outputs = {multi_result::output("report", schema)};
    op.traits.outputs[0].output_schema.validators = {validator.identity};
    op.start_result = [bad](const auto&, const auto& allocator) {
      return ResultContinuation::make<Report>(allocator, bad);
    };
    check(registry->register_operation(std::move(op)));
  }
  OperationDefinition consumer;
  consumer.key = "fixture.consume";
  consumer.traits.input_count = 1;
  consumer.traits.input_schema.resize(1);
  consumer.traits.input_schema[0].result_schema_id = schema.id;
  consumer.traits.input_schema[0].result_schema_version = schema.version;
  consumer.traits.input_schema[0].validators = {validator.identity};
  consumer.traits.outputs = {multi_result::output("report", schema)};
  consumer.traits.outputs[0].output_schema.validators = {validator.identity};
  consumer.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<Report>(allocator, false);
  };
  check(registry->register_operation(std::move(consumer)));
  check(registry->freeze());
  ExecutionContext context(registry);
  for (bool bad : {false, true}) {
    WorkflowDocument doc;
    doc.nodes = {{1, bad ? "fixture.bad" : "fixture.good", {}, {}}};
    doc.nodes.push_back(
        {2, "fixture.consume", {WorkflowNodeOutput{1, "report"}}, {}});
    doc.outputs = {{"report", 2, "report"}};
    GraphContext graph(doc);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    auto result = context.execute(plan);
    PS_CHECK(bad ? !result.ok() &&
                       result.status().code == ErrorCode::OperationFailed
                 : result.ok());
    if (!bad && fields) {
      ExecutionOptions options;
      options.maximum_result_window_bytes = 71;
      auto bounded = context.execute(plan, {}, {}, options);
      PS_CHECK(!bounded.ok() &&
               bounded.status().code == ErrorCode::ResourceExhausted);
    }
  }
  return 0;
}

int main() {
  return exercise(true, 0) || exercise(true, 1) || exercise(false, 2);
}
