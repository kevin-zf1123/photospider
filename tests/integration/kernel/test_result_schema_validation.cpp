#include <atomic>
#include <cstring>
#include <memory>
#include <utility>

#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Producer {
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    const auto region = phase.query.tensor_outputs->boxes().front();
    check(builder.publish_tensor_kernel(
        phase.query.tensor_slot, region, [](const auto& windows) {
          for (const auto& window : windows) {
            const auto axis = window.region().dimensions()[0];
            for (auto i = axis.offset; i < axis.offset + axis.extent; ++i) {
              auto run = take(window.row_run({i}));
              const double value = i + 1;
              std::memcpy(run.data, &value, sizeof(value));
            }
          }
          return Status::success();
        }));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int complete_tiles(unsigned aliases = 0, bool runtime_request = false) {
  auto registry = std::make_shared<OperationRegistry>();
  std::atomic<unsigned> calls{0}, starts{0};
  ValidationDefinition validator;
  validator.identity = {"test.complete", 1, "complete-three/1"};
  validator.requires_complete = true;
  validator.prepare = [](const ValidationPreparation&) {
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  validator.validate = [&](const void*, const ValidationContext& context) {
    ++calls;
    auto window = context.acquire_tensor(0, Region::whole({3}));
    if (!window.ok())
      return window.status();
    double value = 0;
    auto row = window.value().row_run({2});
    if (!row.ok())
      return row.status();
    std::memcpy(&value, row.value().data, sizeof(value));
    return value == 3 ? Status::success()
                      : Status{ErrorCode::OperationFailed, "last tile missing"};
  };
  check(registry->register_validator(std::move(validator)));
  OperationDefinition operation;
  operation.key = "test.complete";
  operation.traits.outputs = {multi_result::output(
      "value", multi_result::schema(ElementType::Float64, {3}))};
  operation.traits.outputs[0].output_schema.validators = {
      {"test.complete", 1, "complete-three/1"}};
  operation.start_result = [&](const auto&, const auto& allocator) {
    ++starts;
    return ResultContinuation::make<Producer>(allocator);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.complete", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  if (aliases)
    document.outputs = {{"a", 1, "value"}, {"b", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_extent = aliases ? 1 : 2;
  ResourceBudget root;
  if (aliases) {
    planning.output_requests["a"].members.emplace(
        0, take(TileSet::range(root, 0, aliases == 2 ? 3 : 1)));
    planning.output_requests["b"].members.emplace(
        0, take(TileSet::range(root, aliases == 1 ? 1 : 0,
                               aliases == 1 || aliases == 3 ? 3 : 1)));
  }
  const auto requests = planning.output_requests;
  if (runtime_request)
    planning.output_requests.clear();
  auto plan = take(Compiler(registry).compile(graph, planning)).plan;
  ExecutionContext execution(registry);
  const auto verify = [&](const auto& result) {
    PS_REQUIRE_OK(result);
    PS_CHECK(calls == 1 && starts == (aliases ? 3U : 2U));
    if (!aliases) {
      PS_CHECK(multi_result::number(result.value().results.at("value"), {2}) ==
               3);
    } else {
      for (const auto& name : {"a", "b"}) {
        const auto& output = result.value().results.at(name);
        auto descriptor = take(output.descriptor());
        const auto& selected = requests.at(name).members.at(0);
        for (unsigned i = 0; i < 3; ++i) {
          PS_CHECK(descriptor.tensor_coverage(0).contains({i}) ==
                   selected.contains(i));
          if (selected.contains(i))
            PS_CHECK(multi_result::number(output, {i}) == i + 1);
        }
      }
    }
    return 0;
  };
  if (runtime_request)
    return verify(
        execution.execute_fragments(take(execution.freeze(plan)), requests));
  return verify(execution.execute(plan));
}

int complete_bundle(bool declared) {
  auto registry = std::make_shared<OperationRegistry>();
  auto schema = multi_result::schema(ElementType::Float64, {1});
  auto second = schema.tensors[0];
  second.key = "second";
  schema.tensors.push_back(std::move(second));
  if (declared)
    schema.publication_scopes.push_back(
        {ResultPublicationPolicy::CompleteBundle, {0, 1}});
  unsigned calls = 0, starts = 0;
  ValidationDefinition validator;
  validator.identity = {"test.bundle", 1, "bundle/1"};
  validator.requires_complete = true;
  validator.prepare = [](const ValidationPreparation&) {
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  validator.validate = [&](const void*, const ValidationContext& context) {
    ++calls;
    if (!context.complete)
      return Status{ErrorCode::OperationFailed, "incomplete bundle"};
    for (unsigned slot = 0; slot < 2; ++slot) {
      auto window = context.acquire_tensor(slot, Region::whole({1}));
      if (!window.ok())
        return window.status();
    }
    return Status::success();
  };
  check(registry->register_validator(std::move(validator)));
  OperationDefinition operation;
  operation.key = "test.bundle";
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].output_schema.validators = {
      {"test.bundle", 1, "bundle/1"}};
  operation.start_result = [&](const auto&, const auto& allocator) {
    ++starts;
    return ResultContinuation::make<Producer>(allocator);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.bundle", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  ResourceBudget root;
  PlanningOptions options;
  options.tile_extent = 1;
  options.output_requests["value"].members.emplace(
      0, take(TileSet::range(root, 0, 1)));
  auto plan = take(Compiler(registry).compile(graph, options)).plan;
  ExecutionContext execution(registry);
  auto result = execution.execute(plan);
  PS_REQUIRE_OK(result);
  PS_CHECK(calls == 1 && starts == 2);
  auto descriptor = take(result.value().results.at("value").descriptor());
  PS_CHECK(descriptor.tensor_coverage(0).contains({0}) &&
           descriptor.tensor_coverage(1).empty());
  return 0;
}
struct DuplicateInputConsumer {
  bool combined, incomplete, supplied = false;
  DuplicateInputConsumer(bool combined, bool incomplete)
      : combined(combined), incomplete(incomplete) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!supplied) {
      supplied = true;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false,
           take(TileSet::range(phase.resources, 0, combined ? 2 : 1))});
      if (!combined)
        need.tiles.push_back(
            {1, 0, false,
             take(TileSet::range(phase.resources, incomplete ? 0 : 1,
                                 incomplete ? 1 : 2))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    // Union admission must not turn either port's grant into a full input.
    if (!combined) {
      for (unsigned port = 0; port < 2; ++port) {
        const auto& coverage = phase.tensors->at({port, 0}).coverage();
        const unsigned expected = port == 0 ? 0 : (incomplete ? 0 : 1);
        if (!coverage.contains({expected}) || coverage.contains({1 - expected}))
          return Result<ResultProgramPoll>(
              Status{ErrorCode::OperationFailed, "port grant was widened"});
      }
    }
    double first = 0, second = 0;
    check(phase.read_tensor(0, 0, {0}, &first, sizeof(first)));
    auto read =
        phase.read_tensor(combined ? 0 : 1, 0, {1}, &second, sizeof(second));
    if (!read.ok())
      return Result<ResultProgramPoll>(read);
    const double sum = first + second;
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        {reinterpret_cast<const std::uint8_t*>(&sum), sizeof(sum)}));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int duplicate_input_scope(bool combined, bool incomplete, bool distinct) {
  auto registry = std::make_shared<OperationRegistry>();
  std::atomic<unsigned> starts{0}, validations{0};
  ValidationDefinition validator;
  validator.identity = {"test.two_tiles", 1, "two-tiles/1"};
  validator.requires_complete = true;
  validator.prepare = [](const ValidationPreparation&) {
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  validator.validate = [&](const void*, const ValidationContext& context) {
    ++validations;
    auto input = context.acquire_tensor(0, Region::whole({2}));
    return input.ok() ? Status::success() : input.status();
  };
  check(registry->register_validator(std::move(validator)));
  OperationDefinition producer;
  producer.key = "test.two_tiles";
  producer.traits.outputs = {multi_result::output(
      "value", multi_result::schema(ElementType::Float64, {2}))};
  producer.traits.outputs[0].output_schema.validators = {
      {"test.two_tiles", 1, "two-tiles/1"}};
  producer.start_result = [&](const auto&, const auto& allocator) {
    ++starts;
    return ResultContinuation::make<Producer>(allocator);
  };
  check(registry->register_operation(producer));
  producer.key = "test.other_tiles";
  check(registry->register_operation(std::move(producer)));
  OperationDefinition consumer;
  consumer.key = "test.duplicate_input";
  consumer.traits.input_count = 2;
  consumer.traits.input_schema.resize(2);
  for (auto& input : consumer.traits.input_schema)
    input.tensor_key = "number";
  consumer.traits.outputs = {multi_result::output("sum")};
  consumer.map_dependencies = [combined, incomplete](const ResultStaticQuery&) {
    ResultStaticMapping mapping;
    mapping.inputs.push_back(
        {0, 0, false,
         take(TileSet::range(ResourceBudget{}, 0, combined ? 2 : 1))});
    if (!combined)
      mapping.inputs.push_back(
          {1, 0, false,
           take(TileSet::range(ResourceBudget{}, incomplete ? 0 : 1,
                               incomplete ? 1 : 2))});
    return Result<ResultStaticMapping>(std::move(mapping));
  };
  consumer.start_result = [=](const auto&, const auto& allocator) {
    return ResultContinuation::make<DuplicateInputConsumer>(allocator, combined,
                                                            incomplete);
  };
  check(registry->register_operation(std::move(consumer)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.two_tiles", {}, {}},
                    {2, "test.other_tiles", {}, {}},
                    {3,
                     "test.duplicate_input",
                     {WorkflowNodeOutput{1, "value"},
                      WorkflowNodeOutput{distinct ? 2U : 1U, "value"}},
                     {}}};
  document.outputs = {{"sum", 3, "sum"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_extent = 1;
  auto plan = take(Compiler(registry).compile(graph, planning)).plan;
  ExecutionContext context(registry);
  auto result = context.execute(plan);
  if (incomplete) {
    PS_CHECK(!result.ok() &&
             result.status().code == ErrorCode::InvalidArgument &&
             result.status().reason == FailureReason::UnauthorizedRead);
    PS_CHECK(starts == 2 && validations == 1);
  } else {
    PS_REQUIRE_OK(result);
    PS_CHECK(starts == (distinct ? 4U : 2U) &&
             validations == (distinct ? 2U : 1U));
    PS_CHECK(multi_result::number(result.value().results.at("sum")) == 3);
  }
  return 0;
}

}  // namespace
int main() {
  return duplicate_input_scope(false, false, false) ||
         duplicate_input_scope(true, false, false) ||
         duplicate_input_scope(false, true, false) ||
         duplicate_input_scope(false, false, true) || complete_tiles() ||
         complete_tiles(1) || complete_tiles(2) || complete_tiles(3) ||
         complete_tiles(4) || complete_tiles(1, true) ||
         complete_tiles(2, true) || complete_tiles(3, true) ||
         complete_tiles(4, true) || complete_bundle(true) ||
         complete_bundle(false);
}
