#include <atomic>
#include <cfenv>  // NOLINT(build/c++11)
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned arrived = 0;
  bool meet() {
    std::unique_lock<std::mutex> lock(mutex);
    ++arrived;
    changed.notify_all();
    return changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return arrived == 2; });
  }
};
struct Counts {
  std::atomic<unsigned> prepared{0}, destroyed{0}, started{0}, retired{0};
  std::atomic<bool> invalid_retirement{false};
  std::shared_ptr<Gate> gate;
};
struct Program {
  std::shared_ptr<Counts> counts;
  bool input;
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>>(true);
  Program(std::shared_ptr<Counts> counts, bool input)
      : counts(std::move(counts)), input(input) {}
  ~Program() {
    alive->store(false);
    ++counts->destroyed;
  }
};
struct Computation {
  const Program* program;
  bool requested = false;
  std::shared_ptr<Counts> counts;
  std::shared_ptr<std::atomic<bool>> alive;
  explicit Computation(const Program* program)
      : program(program), counts(program->counts), alive(program->alive) {}
  ~Computation() {
    if (!alive->load())
      counts->invalid_retirement = true;
    ++counts->retired;
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!phase.query.prepared || phase.query.prepared->state() != program)
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "prepared owner was lost"});
    if (counts->gate && !counts->gate->meet())
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "concurrent poll timed out"});
    const bool input = program->input && phase.query.output_index == 0;
    if (input && !requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false, take(TileSet::range(phase.resources, 0, 1))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    double value = 7 + phase.query.output_index;
    if (input) {
      double source = 0;
      check(phase.read_tensor(0, 0, {0}, &source, sizeof(source)));
      value += source;
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{},
        phase.query.tile_extent));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
OperationDefinition definition(const std::shared_ptr<Counts>& counts) {
  OperationDefinition op;
  op.key = "test.prepared";
  op.traits.input_count = 1;
  op.traits.input_schema.resize(1);

  op.traits.input_schema[0].tensor_key = "number";
  op.traits.input_schema[0].element_type =
      static_cast<unsigned>(ElementType::Float64);
  op.traits.input_schema[0].rank = 1;
  op.traits.requires_metadata_specialization = true;
  op.traits.cacheable = false;
  op.traits.parameter_schema = {{"input", OperationParameterType::Bool},
                                {"tag", OperationParameterType::Float64}};
  op.traits.outputs = {multi_result::output("value"),
                       multi_result::output("constant")};
  for (auto& output : op.traits.outputs) {
    output.continuation_bytes = sizeof(Computation);
  }
  op.prepare_static = [counts](const auto& inputs, const auto& parameters) {
    const bool input = std::get<bool>(parameters.at("input"));
    if (inputs.size() != 1 || !inputs[0].result_schema)
      return Result<OperationPreparation>(
          Status{ErrorCode::TypeMismatch, "complete inputs required"});
    OperationPreparation prepared;
    prepared.outputs.resize(2);
    for (unsigned i = 0; i < 2; ++i) {
      prepared.outputs[i].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(multi_result::schema());
      prepared.outputs[i].input_indices = input && i == 0
                                              ? std::vector<std::uint32_t>{0}
                                              : std::vector<std::uint32_t>{};
    }
    prepared.state = std::make_shared<const Program>(counts, input);
    ++counts->prepared;
    return Result<OperationPreparation>(std::move(prepared));
  };
  op.start_result = [counts](const auto& query, const auto& allocator) {
    ++counts->started;
    return ResultContinuation::make<Computation>(
        allocator, static_cast<const Program*>(query.prepared->state()));
  };
  return op;
}
std::shared_ptr<OperationRegistry> registry(
    const std::shared_ptr<Counts>& counts) {
  auto operations = std::make_shared<OperationRegistry>();
  check(operations->register_operation(definition(counts)));
  auto other = definition(counts);
  other.key = "test.other_prepared";
  check(operations->register_operation(std::move(other)));
  check(operations->freeze());
  return operations;
}
ResultProgramMetadata metadata() {
  ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(multi_result::schema());
  metadata.inputs = {metadata.output};
  return metadata;
}
std::map<std::string, ParameterValue> parameters(bool input) {
  return {{"input", input}, {"tag", 0.0}};
}
int compiled_reuse() {
  auto counts = std::make_shared<Counts>();
  {
    auto operations = registry(counts);
    WorkflowDocument document;
    document.inputs = {multi_result::declaration(1, "input")};
    document.nodes = {
        {1, "test.prepared", {WorkflowInputReference{1}}, parameters(true)}};
    document.outputs = {{"value", 1, "value"}, {"constant", 1, "constant"}};
    GraphContext graph(document);
    auto compiled = take(Compiler(operations).compile(graph));
    PS_CHECK(counts->prepared == 1);
    const auto& prepared = compiled.semantic.nodes()[0].prepared;
    PS_CHECK(prepared == compiled.optimized.nodes()[0].prepared);
    PS_CHECK(prepared == compiled.plan.steps()[0].prepared);
    PS_CHECK(prepared == compiled.plan.steps()[1].prepared);
    auto selected = take(compiled.plan.tile_plan(
        "value",
        TileRequest{{{0, take(TileSet::range(ResourceBudget{}, 0, 1))}}}));
    PS_CHECK(selected.steps()[0].prepared == prepared);
    PS_CHECK(prepared->traits().outputs[0].input_indices ==
             std::optional<std::vector<std::uint32_t>>({{0}}));
    PS_CHECK(prepared->traits().outputs[1].input_indices->empty());
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    ExecutionContext context(operations, config);
    auto root = take(context.resource_budget());
    for (double source : {10., 20.}) {
      auto binding = multi_result::binding(root, "input", source);
      auto result = take(context.execute(compiled.plan, {{binding}}));
      PS_CHECK(multi_result::number(result.results.at("value")) == source + 7);
      PS_CHECK(multi_result::number(result.results.at("constant")) == 8);
      PS_CHECK(result.results.at("value").association().size() == 1);
      PS_CHECK(result.results.at("value").association()[0] ==
               binding.result.object_id());
      PS_CHECK(result.results.at("constant").association().empty());
    }
    PS_CHECK(counts->prepared == 1 && counts->started == 4);
  }
  PS_CHECK(counts->destroyed == 1);
  return 0;
}
int static_seals_and_lifetime() {
  static_assert(!std::is_copy_constructible_v<PreparedOperation> &&
                !std::is_move_constructible_v<PreparedOperation>);
  auto counts = std::make_shared<Counts>();
  auto operations = registry(counts);
  auto foreign = registry(std::make_shared<Counts>());
  auto inputs = metadata();
  auto params = parameters(false);
  ResultProgramQuery query(inputs, params);
  query.semantic_key = "prepared.direct";
  query.prepared = take(
      operations->prepare_operation("test.prepared", inputs.inputs, params));
  ResourceBudget root;
  PS_CHECK(
      operations->start_result("test.prepared", query, root.allocator()).ok());
  const auto starts = counts->started.load();
  PS_CHECK(
      operations->start_result("test.other_prepared", query, root.allocator())
          .status()
          .code == ErrorCode::Stale);
  PS_CHECK(foreign->start_result("test.prepared", query, root.allocator())
               .status()
               .code == ErrorCode::Stale);
  params["tag"] = -0.0;
  PS_CHECK(operations->start_result("test.prepared", query, root.allocator())
               .status()
               .code == ErrorCode::Stale);
  params = parameters(false);
  auto changed = multi_result::schema(ElementType::Float64, {2});
  inputs.inputs[0].result_schema =
      std::make_shared<const SchemaTemplate>(changed);
  PS_CHECK(operations->start_result("test.prepared", query, root.allocator())
               .status()
               .code == ErrorCode::Stale);
  inputs = metadata();
  PS_CHECK(counts->started == starts && counts->prepared == 1);
  // A different NaN payload must not reuse a bit-sealed static program.
  std::uint64_t bits = UINT64_C(0x7ff8000000000042);
  double nan;
  std::memcpy(&nan, &bits, sizeof(nan));
  params["tag"] = nan;
  query.prepared = take(
      operations->prepare_operation("test.prepared", inputs.inputs, params));
  PS_CHECK(
      operations->start_result("test.prepared", query, root.allocator()).ok());
  ++bits;
  std::memcpy(&nan, &bits, sizeof(nan));
  params["tag"] = nan;
  PS_CHECK(operations->start_result("test.prepared", query, root.allocator())
               .status()
               .code == ErrorCode::Stale);
  PS_CHECK(counts->prepared == 2);
  // The continuation owns the immutable program after external owners retire.
  params = parameters(false);
  query.prepared = take(
      operations->prepare_operation("test.prepared", inputs.inputs, params));
  auto continuation =
      take(operations->start_result("test.prepared", query, root.allocator()));
  query.prepared.reset();
  operations.reset();
  PS_CHECK(counts->destroyed == 2);
  ResultObjectInputs objects;
  auto allocator = root.allocator();
  ResultProgramPhase phase{query, objects, allocator, root, {}, {}};
  auto publication = take(continuation.poll(phase));
  PS_CHECK(std::holds_alternative<ResultPublication>(publication));
  auto result = std::get<ResultPublication>(std::move(publication)).result;
  continuation = {};
  PS_CHECK(counts->destroyed == 3 && !counts->invalid_retirement);
  PS_CHECK(counts->retired == counts->started);
  PS_CHECK(multi_result::number(result) == 7);
  result = {};
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
int concurrent_reuse() {
  auto counts = std::make_shared<Counts>();
  counts->gate = std::make_shared<Gate>();
  auto operations = registry(counts);
  auto inputs = metadata();
  auto params = parameters(false);
  ResultProgramQuery query(inputs, params);
  query.semantic_key = "prepared.concurrent";
  query.prepared = take(
      operations->prepare_operation("test.prepared", inputs.inputs, params));
  ResourceBudget root;
  const auto run = [&] {
    auto allocator = root.allocator();
    auto continuation =
        take(operations->start_result("test.prepared", query, allocator));
    ResultObjectInputs objects;
    ResultProgramPhase phase{query, objects, allocator, root, {}, {}};
    auto publication = take(continuation.poll(phase));
    return multi_result::number(
        std::get<ResultPublication>(publication).result);
  };
  auto first = std::async(std::launch::async, run);
  auto second = std::async(std::launch::async, run);
  PS_CHECK(first.get() == 7 && second.get() == 7);
  PS_CHECK(counts->prepared == 1 && counts->started == 2 &&
           counts->retired == 2);
  query.prepared.reset();
  PS_CHECK(counts->destroyed == 1 && !counts->invalid_retirement);
  return 0;
}
int projection_validation() {
  auto counts = std::make_shared<Counts>();
  auto operations = std::make_shared<OperationRegistry>();
  auto op = definition(counts);
  // Static preparation may narrow a registered projection, never broaden it.
  op.traits.outputs[0].input_indices = std::vector<std::uint32_t>{};
  check(operations->register_operation(std::move(op)));
  check(operations->freeze());
  auto inputs = metadata();
  PS_CHECK(
      operations
          ->prepare_operation("test.prepared", inputs.inputs, parameters(false))
          .ok());
  PS_CHECK(
      operations
          ->prepare_operation("test.prepared", inputs.inputs, parameters(true))
          .status()
          .code == ErrorCode::InvalidArgument);
  const auto calls = counts->prepared.load();
  // Empty runtime projections still require the complete static input contract.
  PS_CHECK(
      !operations->prepare_operation("test.prepared", {}, parameters(false))
           .ok());
  inputs.inputs[0].result_schema = std::make_shared<const SchemaTemplate>(
      multi_result::schema(ElementType::Int64));
  PS_CHECK(
      operations
          ->prepare_operation("test.prepared", inputs.inputs, parameters(false))
          .status()
          .code == ErrorCode::TypeMismatch);
  PS_CHECK(counts->prepared == calls && counts->started == 0);
  PS_CHECK(counts->destroyed == counts->prepared);
  for (const auto& ports :
       {std::vector<std::uint32_t>{0, 0}, std::vector<std::uint32_t>{1}}) {
    auto invalid = std::make_shared<OperationRegistry>();
    auto operation = definition(counts);
    const auto prepare = operation.prepare_static;
    operation.prepare_static = [prepare, ports](const auto& inputs,
                                                const auto& parameters) {
      auto result = prepare(inputs, parameters);
      if (!result.ok())
        return result;
      auto prepared = result.take_value();
      prepared.outputs[0].input_indices = ports;
      return Result<OperationPreparation>(std::move(prepared));
    };
    check(invalid->register_operation(std::move(operation)));
    check(invalid->freeze());
    auto rejected = invalid->prepare_operation(
        "test.prepared", metadata().inputs, parameters(false));
    PS_CHECK(!rejected.ok() &&
             rejected.status().code == ErrorCode::InvalidArgument);
    PS_CHECK(counts->started == 0 && counts->destroyed == counts->prepared);
  }
  return 0;
}
int projected_execution() {
  auto counts = std::make_shared<Counts>();
  auto operations = std::make_shared<OperationRegistry>();
  check(operations->register_operation(definition(counts)));
  std::atomic<unsigned> source_calls{0};
  OperationDefinition source;
  source.key = "test.unwanted";
  source.traits.outputs = {multi_result::output("value")};
  source.start_result = [&](const auto&, const auto&) {
    ++source_calls;
    return Result<ResultContinuation>(
        Status{ErrorCode::OperationFailed, "selected source failed"});
  };
  check(operations->register_operation(std::move(source)));
  check(operations->freeze());
  ExecutionContext context(operations);
  for (bool input : {false, true}) {
    WorkflowDocument document;
    document.nodes = {{1, "test.unwanted", {}, {}},
                      {2,
                       "test.prepared",
                       {WorkflowNodeOutput{1, "value"}},
                       parameters(input)}};
    document.outputs = {{"value", 2, "value"}};
    GraphContext graph(document);
    auto compiled = take(Compiler(operations).compile(graph));
    auto result = context.execute(compiled.plan);
    if (!input) {
      PS_CHECK(result.ok() &&
               multi_result::number(result.value().results.at("value")) == 7);
      PS_CHECK(source_calls == 0);
    } else {
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::OperationFailed);
      PS_CHECK(source_calls == 1);
    }
  }
  return 0;
}

int floating_owner_restoration() {
  struct Restore {
    std::fenv_t saved{};
    Restore() { std::fegetenv(&saved); }
    ~Restore() { std::fesetenv(&saved); }
  } restore;
  for (unsigned mode = 0; mode < 3; ++mode) {
    PS_CHECK(std::fesetround(FE_DOWNWARD) == 0);
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    unsigned schemas_destroyed = 0, states_destroyed = 0;
    bool inherited = false;
    auto registry = std::make_shared<OperationRegistry>();
    OperationDefinition operation;
    operation.key = "test.environment";
    operation.traits.outputs = {multi_result::output("value")};
    operation.traits.requires_metadata_specialization = true;
    const auto specialize = [&](const auto&, const auto&) {
      inherited = std::fegetround() == FE_DOWNWARD &&
                  std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO;
      std::fesetround(FE_UPWARD);
      std::feraiseexcept(FE_INVALID);
      OperationOutputSpecialization output;
      output.metadata.result_schema = std::shared_ptr<const SchemaTemplate>(
          new SchemaTemplate(multi_result::schema()),
          [&](const SchemaTemplate* schema) {
            delete schema;
            ++schemas_destroyed;
            std::fesetround(FE_TOWARDZERO);
            std::feraiseexcept(FE_OVERFLOW);
          });
      return Result<std::vector<OperationOutputSpecialization>>(
          std::vector<OperationOutputSpecialization>{std::move(output)});
    };
    if (!mode) {
      operation.specialize_metadata = specialize;
    } else {
      operation.prepare_static = [&, mode](const auto& inputs,
                                           const auto& parameters) {
        OperationPreparation prepared;
        prepared.outputs = take(specialize(inputs, parameters));
        if (mode == 2)
          prepared.outputs.push_back(prepared.outputs.front());
        prepared.state =
            std::shared_ptr<const void>(new int(7), [&](const void* state) {
              delete static_cast<const int*>(state);
              ++states_destroyed;
              std::fesetround(FE_TOWARDZERO);
              std::feraiseexcept(FE_OVERFLOW);
            });
        return Result<OperationPreparation>(std::move(prepared));
      };
    }
    operation.start_result = [](const auto&, const auto& allocator) {
      return ResultContinuation::make<multi_result::Program>(allocator, -1, 7);
    };
    check(registry->register_operation(std::move(operation)));
    check(registry->freeze());
    {
      auto prepared = registry->prepare_operation("test.environment", {}, {});
      PS_CHECK(prepared.ok() == (mode != 2));
      PS_CHECK(inherited && schemas_destroyed == 1);
      PS_CHECK(std::fegetround() == FE_DOWNWARD &&
               std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
    }
    registry.reset();
    PS_CHECK(states_destroyed == (mode ? 1U : 0U));
    PS_CHECK(std::fegetround() == FE_DOWNWARD &&
             std::fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO);
  }
  return 0;
}

}  // namespace
int main() {
  PS_CHECK(floating_owner_restoration() == 0);
  PS_CHECK(compiled_reuse() == 0);
  PS_CHECK(static_seals_and_lifetime() == 0);
  PS_CHECK(projection_validation() == 0);
  PS_CHECK(concurrent_reuse() == 0);
  PS_CHECK(projected_execution() == 0);
  return 0;
}
