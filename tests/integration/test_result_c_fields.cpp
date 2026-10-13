#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
SchemaTemplate control_schema() {
  auto schema = multi_result::schema(ElementType::Int64);
  schema.id = "fixture.control";
  schema.version = 1;
  schema.tensors[0].key = "samples";
  return schema;
}
float number(const ResultRef& result) {
  float value = 0;
  check(result.read_tensor(take(result.descriptor()), 0, {0}, &value, 4));
  return value;
}
WorkflowDocument workflow(std::int64_t mode = 0) {
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "count", control_schema())};
  document.nodes = {
      {1, "fixture.result.rows", {WorkflowInputReference{1}}, {}},
      {2,
       "fixture.result.rows_sum",
       {WorkflowNodeOutput{1, "rows"}},
       {{"mode", mode}}},
      {3, "fixture.result.rows_first", {WorkflowNodeOutput{1, "rows"}}, {}}};
  document.outputs = {{"rows", 1, "rows"},
                      {"sum", 2, "number"},
                      {"first", 3, "number"}};
  return document;
}
int execution() {
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_RESULT_FIXTURE));
  check(registry->freeze());
  ExecutionContextConfig config;
  config.result_cache_bytes = 8192;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  auto bindings = [&](int count) {
    return ExecutionBindings{
        {multi_result::binding(root, "count", count, control_schema())}};
  };
  ResultRef retained;
  for (std::int64_t mode : {0, 8}) {
    GraphContext graph(workflow(mode));
    auto plan = take(Compiler(registry).compile(graph)).plan;
    for (int count : {0, 1, 3, 4}) {
      auto result = take(context.execute(plan, bindings(count)));
      PS_CHECK(number(result.results.at("sum")) == count * (count + 1) / 2);
      PS_CHECK(number(result.results.at("first")) == (count ? 1 : 0));
      PS_CHECK(take(result.results.at("rows").descriptor()).rows(0) ==
               static_cast<std::uint64_t>(count));
      if (count == 3)
        retained = result.results.at("rows");
    }
  }
  context.clear_result_cache();
  auto read = take(retained.prepare_read(take(retained.descriptor()), 0, 0, 3));
  auto window = take(read.load(24));
  const std::int64_t expected[] = {1, 2, 3};
  PS_CHECK(window->bytes().size() == sizeof(expected));
  PS_CHECK(std::memcmp(window->bytes().data(), expected, sizeof(expected)) ==
           0);
  for (std::int64_t mode = 1; mode <= 7; ++mode) {
    auto document = workflow(mode);
    document.outputs = {{"sum", 2, "number"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    auto result = context.execute(plan, bindings(3));
    PS_CHECK(!result.ok());
    PS_CHECK(result.status().code == ErrorCode::InvalidArgument);
    if (mode <= 2) {
      PS_CHECK(result.status().reason == FailureReason::UnauthorizedRead);
      PS_CHECK(result.status().detail.origin == FailureOrigin::Protocol);
      PS_CHECK(result.status().detail.scope == FailureScope::Group);
    }
  }
  for (std::int64_t mode : {0, 8}) {
    context.clear_result_cache();
    GraphContext bounded_graph(workflow(mode));
    auto bounded_plan = take(Compiler(registry).compile(bounded_graph)).plan;
    ExecutionOptions options;
    options.maximum_result_window_bytes = 16;
    auto bounded = context.execute(bounded_plan, bindings(3), {}, options);
    if (mode == 8) {
      PS_CHECK(!bounded.ok());
      PS_CHECK(bounded.status().code == ErrorCode::ResourceExhausted);
      PS_CHECK(bounded.status().reason == FailureReason::CapacityLimit);
    } else {
      PS_CHECK(bounded.ok());
      PS_CHECK(number(bounded.value().results.at("sum")) == 6);
    }
  }
  GraphContext graph(workflow());
  auto plan = take(Compiler(registry).compile(graph)).plan;
  auto handle = take(context.open_demand(plan, bindings(3)));
  auto first = take(handle.request({{"sum", TileRequest{true, {}}}}));
  PS_CHECK(number(first.results.at("sum")) == 6);
  check(handle.replace_bindings(bindings(0)).status());
  auto empty = take(handle.request({{"sum", TileRequest{true, {}}}}));
  PS_CHECK(number(empty.results.at("sum")) == 0);
  check(handle.replace_bindings(bindings(4)).status());
  auto four = take(handle.request({{"sum", TileRequest{true, {}}}}));
  PS_CHECK(number(four.results.at("sum")) == 10);
  return 0;
}
int direct() {
  // The registry is retired before the second poll, and the published Result
  // survives both registry and C continuation retirement.
  ResourceBudget root;
  ResourceAllocationScope scope(root);
  auto registry = std::make_shared<OperationRegistry>();
  check(registry->load_plugin(PS_RESULT_FIXTURE));
  auto schema = *take(registry->find_traits("fixture.result.rows"))
                     .outputs[0]
                     .result_schema;
  auto builder = take(ResultBuilder::start(root, schema, "c.field.input"));
  const std::int64_t values[] = {11, 23};
  check(builder.append(
      0, 2,
      ByteView(reinterpret_cast<const std::uint8_t*>(values), sizeof(values))));
  check(builder.publish(0, 2));
  auto input = take(builder.seal());
  OperationMetadata input_metadata;
  input_metadata.result_schema = std::make_shared<const SchemaTemplate>(schema);
  const std::vector<OperationMetadata> inputs{input_metadata};
  const std::map<std::string, ParameterValue> parameters;
  auto prepared = take(registry->prepare_operation("fixture.result.rows_sum",
                                                   inputs, parameters));
  auto outputs =
      take(infer_operation_outputs(prepared->traits(), inputs, parameters));
  ResultProgramMetadata metadata{inputs, outputs[0]};
  ResultProgramQuery query(metadata, parameters);
  query.prepared = prepared;
  query.semantic_key = "c.fields.direct";
  auto allocator = root.allocator();
  auto continuation =
      take(registry->start_result("fixture.result.rows_sum", query, allocator));
  ResultObjectInputs objects;

  bool deny_work = false, cancel_read = false;
  CancellationSource cancellation;
  query.cancellation = cancellation.token();
  ResultProgramPhase phase{
      query,
      objects,
      allocator,
      root,
      [&](std::uint64_t work) {
        if (cancel_read && work)
          cancellation.cancel();
        return deny_work && work ? Status{ErrorCode::ResourceExhausted, {}}
                                 : root.consume({work});
      },
      {}};
  auto need = take(continuation.poll(phase));
  PS_CHECK(std::holds_alternative<ResultProgramNeed>(need));
  PS_CHECK(std::get<ResultProgramNeed>(need).tiles.size() == 1);
  PS_CHECK(std::get<ResultProgramNeed>(need).tiles[0].whole);
  objects.emplace(0, input);
  auto denied =
      take(registry->start_result("fixture.result.rows_sum", query, allocator));
  take(denied.poll(phase));
  deny_work = true;
  auto failed = denied.poll(phase);
  PS_CHECK(!failed.ok());
  PS_CHECK(failed.status().code == ErrorCode::ResourceExhausted);
  deny_work = false;
  auto cancelled =
      take(registry->start_result("fixture.result.rows_sum", query, allocator));
  take(cancelled.poll(phase));
  cancel_read = true;
  auto stopped = cancelled.poll(phase);
  PS_CHECK(!stopped.ok());
  PS_CHECK(stopped.status().code == ErrorCode::Cancelled);
  cancel_read = false;
  query.cancellation = {};
  {
    const std::map<std::string, ParameterValue> invalid_parameters{
        {"mode", std::int64_t{1}}};
    ResultProgramQuery invalid_query(metadata, invalid_parameters);
    invalid_query.semantic_key = "c.fields.unauthorized";
    auto invalid = take(registry->start_result("fixture.result.rows_sum",
                                               invalid_query, allocator));
    ResultObjectInputs unavailable;
    auto failure = std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok);
    ResultProgramPhase invalid_phase{
        invalid_query,
        unavailable,
        allocator,
        root,
        [&](std::uint64_t work) { return root.consume({work}); },
        failure};
    auto rejected = invalid.poll(invalid_phase);
    PS_CHECK(!rejected.ok() &&
             rejected.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(rejected.status().reason == FailureReason::UnauthorizedRead);
    PS_CHECK(rejected.status().detail.origin == FailureOrigin::Protocol);
    PS_CHECK(rejected.status().detail.scope == FailureScope::Group);
    PS_CHECK(invalid.poll(invalid_phase).status().code ==
             ErrorCode::InvalidArgument);
  }
  registry.reset();
  auto published = take(continuation.poll(phase));
  PS_CHECK(std::holds_alternative<ResultPublication>(published));
  auto result = std::get<ResultPublication>(published).result;
  continuation = {};
  PS_CHECK(number(result) == 34);
  return 0;
}
}  // namespace
int main() {
  try {
    return execution() || direct();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
