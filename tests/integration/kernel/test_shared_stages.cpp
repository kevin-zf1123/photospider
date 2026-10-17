#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/plugin/tensor_helpers.hpp"
#include "photospider/plugin/whole_stage.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
constexpr std::uint64_t kWork = 100000;
struct Probe {
  std::atomic<unsigned> polls{0}, tiles{0}, validations{0};
};
struct Program {
  std::shared_ptr<Probe> probe;
  bool prefix;
  unsigned position = 0;
  std::optional<ResultBuilder> builder;
  Program(std::shared_ptr<Probe> probe, bool prefix)
      : probe(std::move(probe)), prefix(prefix) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    if (phase.query.shared_stage == UINT32_MAX) {
      const auto found = phase.shared_stages->find(0);
      if (found == phase.shared_stages->end()) {
        ResultProgramNeed need;
        need.shared_stages.push_back(0);
        return Poll(std::move(need));
      }
      ++probe->tiles;
      return Poll(
          ResultPublication{take(sdk::to_tile(phase, found->second)), true});
    }
    ++probe->polls;
    if (!builder)
      builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {}, phase.query.tile_extent));
    const double value = position + 1;
    check(builder->publish_tensor(
        0, Region({{position, 1}}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    ++position;
    const bool complete = !prefix || position == 2;
    return Poll(ResultPublication{
        complete ? take(builder->seal()) : builder->reference(), complete});
  }
};
int regression(bool prefix) {
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<OperationRegistry>();
  auto schema = multi_result::schema(ElementType::Float64, {prefix ? 2U : 1U});
  if (!prefix) {
    ValidationDefinition validator;
    validator.identity = {"test.stage", 1, "work-limit/1"};
    validator.requires_complete = true;
    validator.prepare = [](const ValidationPreparation&) {
      return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
    };
    validator.validate = [probe](const void*,
                                 const ValidationContext& context) {
      ++probe->validations;
      // The failing service must remain sticky even when the callback ignores
      // it.
      context.consume(kWork + 1);
      return Status::success();
    };
    check(registry->register_validator(std::move(validator)));
  }
  OperationDefinition operation;
  operation.key = "test.stage";
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].continuation_bytes = sizeof(Program);
  OperationSharedStage stage;
  stage.key = "compute";
  stage.result_schema = schema;
  stage.continuation_bytes = sizeof(Program);
  stage.maximum_dependency_stages = 3;
  stage.maximum_work = kWork;
  stage.maximum_output_payload_bytes = prefix ? 16 : 8;
  if (!prefix)
    stage.validators = {{"test.stage", 1, "work-limit/1"}};
  operation.traits.shared_stages = {stage};
  operation.start_result = [probe, prefix](const auto&, const auto& allocator) {
    return ResultContinuation::make<Program>(allocator, probe, prefix);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.stage", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_extent = 1;
  auto plan = take(Compiler(registry).compile(graph, planning)).plan;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext execution(registry, config);
  auto result = execution.execute(plan);
  if (prefix) {
    PS_REQUIRE_OK(result);
    PS_CHECK(probe->polls == 2 && probe->tiles == 2);
    PS_CHECK(multi_result::number(result.value().results.at("value"), {1}) ==
             2);
    // Direct registry hosts formerly rejected the same nonfinal stage commit.
    ResourceBudget root;
    auto allocator = root.allocator();
    ResultProgramMetadata metadata;
    metadata.output.result_schema =
        std::make_shared<const SchemaTemplate>(schema);
    std::map<std::string, ParameterValue> parameters;
    ResultProgramQuery query(metadata, parameters);
    query.semantic_key = "direct.stage";
    query.shared_stage = 0;
    query.tile_extent = 1;
    auto continuation =
        take(registry->start_result("test.stage", query, allocator));
    ResultObjectInputs inputs;
    ResultProgramPhase phase{
        query,
        inputs,
        allocator,
        root,
        [&](auto n) { return root.consume({n}); },
        std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)};
    auto first = continuation.poll(phase);
    PS_REQUIRE_OK(first);
    const auto& first_commit = std::get<ResultPublication>(first.value());
    PS_CHECK(!first_commit.complete);
    auto first_facts = take(first_commit.result.descriptor(false));
    PS_CHECK(first_facts.tensor_coverage(0).contains({0}) &&
             !first_facts.tensor_coverage(0).contains({1}));
    auto second = continuation.poll(phase);
    PS_REQUIRE_OK(second);
    const auto& last = std::get<ResultPublication>(second.value());
    PS_CHECK(last.complete && multi_result::number(last.result, {1}) == 2);
  } else {
    PS_CHECK(!result.ok() &&
             result.status().reason == FailureReason::WorkLimit);
    PS_CHECK(probe->polls == 1 && probe->validations == 1 && probe->tiles == 0);
  }
  return 0;
}
// Regression: revisiting an earlier visible prefix after later tiles had
// replaced the weak output alias used to leave a fresh actor waiting forever.
struct PrefixProgram {
  std::shared_ptr<unsigned> produced;
  unsigned turn = 0;
  explicit PrefixProgram(std::shared_ptr<unsigned> count)
      : produced(std::move(count)) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    if (!phase.query.inputs.empty() && turn < 2) {
      ResultProgramNeed need;
      auto tile = turn++ == 0 ? 2U : 0U;
      need.tiles.push_back(
          {0, 0, false, take(TileSet::range(phase.resources, tile, tile + 1))});
      return Poll(std::move(need));
    }
    if (!phase.query.inputs.empty()) {
      double value = -1;
      check(phase.read_tensor(0, 0, {0}, &value, sizeof(value)));
      if (value != 0)
        return Poll(Status{ErrorCode::OperationFailed, "wrong prefix tile"});
    } else {
      ++*produced;
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    const double value = phase.query.tile_unit;
    check(builder.publish_tensor(
        0, phase.query.tensor_outputs->boxes().front(),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
int prefix_revisit() {
  auto registry = std::make_shared<OperationRegistry>();
  auto count = std::make_shared<unsigned>(0);
  auto schema = multi_result::schema(ElementType::Float64, {3});
  schema.publication_scopes.push_back(
      {ResultPublicationPolicy::StablePrefix, {0}});
  for (bool consumer : {false, true}) {
    OperationDefinition operation;
    operation.key = consumer ? "test.prefix_read" : "test.prefix";
    operation.traits.outputs = {multi_result::output(
        "value", consumer ? multi_result::schema() : schema)};
    operation.traits.outputs[0].maximum_dependency_stages = 4;
    if (consumer) {
      operation.traits.input_count = 1;
      operation.traits.input_schema.resize(1);
      operation.traits.input_schema[0].tensor_key = "number";
    }
    operation.start_result = [count](const auto&, const auto& allocator) {
      return ResultContinuation::make<PrefixProgram>(allocator, count);
    };
    check(registry->register_operation(std::move(operation)));
  }
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {1, "test.prefix", {}, {}},
      {2, "test.prefix_read", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"value", 2, "value"}};
  GraphContext graph(document);
  PlanningOptions planning;
  planning.tile_extent = 1;
  auto plan = take(Compiler(registry).compile(graph, planning)).plan;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext execution(registry, config);
  auto result = execution.execute(plan);
  PS_REQUIRE_OK(result);
  PS_CHECK(*count == 3);
  return 0;
}
Result<ResultProgramPoll> empty_publication(const ResultProgramPhase& phase) {
  auto builder = take(ResultBuilder::start(phase.resources,
                                           *phase.query.output.result_schema,
                                           phase.query.semantic_key));
  return Result<ResultProgramPoll>(
      ResultPublication{take(builder.seal()), true});
}
int direct_validator_failure(bool empty) {
  auto registry = std::make_shared<OperationRegistry>();
  auto schema = multi_result::schema(ElementType::Float64, {2});
  ValidationDefinition validator;
  validator.identity = {"test.reject", 1, "reject/1"};
  validator.requires_complete = empty;
  validator.prepare = [](const ValidationPreparation&) {
    return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
  };
  validator.validate = [](const void*, const ValidationContext& context) {
    return context.tensor_coverage(0)->contains({1})
               ? Status{ErrorCode::OperationFailed, "rejected tile"}
               : Status::success();
  };
  check(registry->register_validator(std::move(validator)));
  OperationDefinition operation;
  operation.key = "test.reject";
  operation.traits.outputs = {multi_result::output("value", schema)};
  OperationSharedStage stage;
  stage.key = "stage";
  stage.result_schema = schema;
  stage.continuation_bytes = sizeof(Program);
  stage.maximum_dependency_stages = 4;
  stage.maximum_work = kWork;
  stage.maximum_output_payload_bytes = 16;
  stage.validators = {{"test.reject", 1, "reject/1"}};
  operation.traits.shared_stages = {stage};
  operation.start_result = [empty](const auto&, const auto& allocator) {
    if (!empty)
      return ResultContinuation::make<Program>(allocator,
                                               std::make_shared<Probe>(), true);
    return ResultContinuation::stateless<empty_publication>();
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  ResourceBudget root;
  auto allocator = root.allocator();
  ResultProgramMetadata metadata;
  metadata.output.result_schema =
      std::make_shared<const SchemaTemplate>(schema);
  std::map<std::string, ParameterValue> parameters;
  ResultProgramQuery query(metadata, parameters);
  query.semantic_key = "direct.reject";
  query.shared_stage = 0;
  query.tile_extent = 1;
  auto continuation =
      take(registry->start_result("test.reject", query, allocator));
  ResultObjectInputs inputs;
  ResultProgramPhase phase{
      query,
      inputs,
      allocator,
      root,
      [&](auto n) { return root.consume({n}); },
      std::make_shared<std::atomic<ErrorCode>>(ErrorCode::Ok)};
  auto first = continuation.poll(phase);
  if (empty) {
    PS_CHECK(!first.ok());
  } else {
    PS_REQUIRE_OK(first);
    const auto prefix = std::get<ResultPublication>(first.value()).result;
    auto second = continuation.poll(phase);
    PS_CHECK(!second.ok());
    for (bool destroy : {false, true}) {
      if (destroy)
        continuation = {};
      const auto failure = prefix.production_status();
      PS_CHECK(failure.code == second.status().code &&
               failure.reason == second.status().reason &&
               failure.detail.origin == second.status().detail.origin &&
               failure.detail.scope == second.status().detail.scope &&
               failure.message == second.status().message);
    }
    // The escaped first commit cannot gain rejected samples on a later poll.
    auto facts = take(prefix.descriptor(false));
    PS_CHECK(facts.tensor_coverage(0).contains({0}) &&
             !facts.tensor_coverage(0).contains({1}));
    double value = 0;
    check(prefix.read_tensor(facts, 0, {0}, &value, sizeof(value)));
    PS_CHECK(value == 1);
  }
  return 0;
}
// These selections previously serialized to identical grant keys.
TileRequest grant_selection(const ResourceBudget& root, bool second) {
  TileRequest request;
  request.members.emplace(0, take(TileSet::from_indices(
                                 root, second ? std::vector<TileIndex>{0}
                                              : std::vector<TileIndex>{0, 2})));
  request.members.emplace(
      second ? 2 : 5,
      take(TileSet::from_indices(root, second ? std::vector<TileIndex>{3, 4, 6}
                                              : std::vector<TileIndex>{6})));
  return request;
}
struct GrantProgram {
  unsigned turn = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    if (phase.query.shared_stage == UINT32_MAX && turn < 2) {
      ResultProgramNeed need;
      need.shared_stages.emplace_back(0,
                                      grant_selection(phase.resources, turn++));
      return Poll(std::move(need));
    }
    if (phase.query.shared_stage == UINT32_MAX) {
      const auto& grant = phase.shared_stages->at(0);
      auto facts = take(grant.descriptor());
      auto grids = take(TileGrid::members(grant.schema(), 1));
      auto selected = grant_selection(phase.resources, true);
      for (std::uint32_t slot = 0; slot < grids.size(); ++slot) {
        auto found = selected.members.find(slot);
        auto expected = take(grids[slot].coverage(
            found == selected.members.end() ? TileSet{} : found->second));
        if (facts.tensor_coverage(slot) != expected)
          return Poll(
              Status{ErrorCode::OperationFailed, "wrong grant coverage"});
      }
    }
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {}, phase.query.tile_extent));
    for (std::uint32_t slot = 0;
         slot < phase.query.output.result_schema->tensors.size(); ++slot) {
      const auto& tensor = phase.query.output.result_schema->tensors[slot];
      std::vector<double> values(take(tensor.sample_count()), 1);
      check(builder.publish_tensor(
          slot, Region::whole(tensor.sample_shape()),
          ByteView(reinterpret_cast<const std::uint8_t*>(values.data()),
                   values.size() * sizeof(double))));
    }
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
int grant_key_boundaries() {
  auto registry = std::make_shared<OperationRegistry>();
  auto schema = multi_result::schema(ElementType::Float64, {7});
  for (unsigned slot = 1; slot < 6; ++slot) {
    auto tensor = schema.tensors[0];
    tensor.key = "member" + std::to_string(slot);
    schema.tensors.push_back(std::move(tensor));
  }
  OperationDefinition operation;
  operation.key = "test.grant";
  operation.traits.outputs = {multi_result::output("value")};
  operation.traits.outputs[0].maximum_dependency_stages = 4;
  OperationSharedStage stage;
  stage.key = "stage";
  stage.result_schema = schema;
  stage.continuation_bytes = sizeof(GrantProgram);
  stage.maximum_dependency_stages = 4;
  stage.maximum_work = kWork;
  stage.maximum_output_payload_bytes = 336;
  operation.traits.shared_stages = {stage};
  operation.start_result = [](const auto&, const auto& allocator) {
    return ResultContinuation::make<GrantProgram>(allocator);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.grant", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.tile_extent = 1;
  auto plan = take(Compiler(registry).compile(graph, options)).plan;
  ExecutionContext execution(registry);
  auto result = execution.execute(plan);
  PS_REQUIRE_OK(result);
  return 0;
}
// A[0] -> B[0] -> A[1] is acyclic even while A waits for B.
struct InterleavedStages {
  bool committed_seed;
  unsigned turn = 0;
  std::optional<ResultBuilder> builder;
  explicit InterleavedStages(bool committed) : committed_seed(committed) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    const auto stage = phase.query.shared_stage;
    if (stage == UINT32_MAX) {
      if (phase.shared_stages->empty()) {
        ResultProgramNeed need;
        need.shared_stages.emplace_back(0);
        return Poll(std::move(need));
      }
      return Poll(ResultPublication{
          take(sdk::to_tile(phase, phase.shared_stages->at(0))), true});
    }
    if (!builder)
      builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {}, phase.query.tile_extent));
    if (stage == 0 && turn++ == 0) {
      const double seed = 10;
      check(builder->publish_tensor(
          0, Region({{0, 1}}),
          ByteView(reinterpret_cast<const std::uint8_t*>(&seed),
                   sizeof(seed))));
      return Poll(ResultPublication{builder->reference(), false});
    }
    const auto other = stage == 0 ? 1U : 0U;
    if (!phase.shared_stages->count(other)) {
      ResultProgramNeed need;
      TileRequest request;
      const auto tile = stage == 1 && !committed_seed ? 1U : 0U;
      request.members.emplace(
          0, take(TileSet::range(phase.resources, tile, tile + 1)));
      need.shared_stages.emplace_back(other, std::move(request));
      return Poll(std::move(need));
    }
    const double value =
        multi_result::number(phase.shared_stages->at(other)) + 1;
    check(builder->publish_tensor(
        0, Region({{stage == 0 ? 1U : 0U, 1}}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    return Poll(ResultPublication{take(builder->seal()), true});
  }
};
int interleaved_commits(bool committed_seed) {
  auto registry = std::make_shared<OperationRegistry>();
  const auto schema = multi_result::schema(ElementType::Float64, {2});
  OperationDefinition operation;
  operation.key = "test.interleaved";
  operation.traits.outputs = {multi_result::output("value", schema)};
  operation.traits.outputs[0].continuation_bytes = sizeof(InterleavedStages);
  for (unsigned stage = 0; stage < 2; ++stage) {
    OperationSharedStage definition;
    definition.key = stage ? "B" : "A";
    definition.result_schema = stage ? multi_result::schema() : schema;
    definition.continuation_bytes = sizeof(InterleavedStages);
    definition.maximum_dependency_stages = 8;
    definition.maximum_work = kWork;
    definition.maximum_output_payload_bytes = 16;
    operation.traits.shared_stages.push_back(std::move(definition));
  }
  operation.start_result = [committed_seed](const auto&,
                                            const auto& allocator) {
    return ResultContinuation::make<InterleavedStages>(allocator,
                                                       committed_seed);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.interleaved", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.tile_extent = 1;
  auto plan = take(Compiler(registry).compile(graph, options)).plan;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext execution(registry, config);
  auto result = execution.execute(plan);
  if (!committed_seed) {
    PS_CHECK(!result.ok() && result.status().code == ErrorCode::Cycle);
  } else {
    PS_REQUIRE_OK(result);
    PS_CHECK(multi_result::number(result.value().results.at("value"), {1}) ==
             12);
  }
  return 0;
}
struct CacheIdentity {
  bool input, requested = false;
  TileIndex input_tile;
  explicit CacheIdentity(bool input, TileIndex tile = 0)
      : input(input), input_tile(tile) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    if (input && !requested) {
      requested = true;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false,
           take(TileSet::range(phase.resources, input_tile, input_tile + 1))});
      return Poll(std::move(need));
    }
    double value = 17;
    if (input)
      check(phase.read_tensor(0, 0, {input_tile}, &value, sizeof(value)));
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        phase.query.tensor_slot,
        phase.query.tensor_outputs ? phase.query.tensor_outputs->boxes().front()
                                   : Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
struct CacheRelay {
  unsigned polls = 0;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    const auto stage = phase.query.shared_stage == UINT32_MAX ? 1U : 0U;
    if (++polls <= 2) {
      ResultProgramNeed need;
      need.shared_stages.emplace_back(stage);
      return Poll(std::move(need));
    }
    const double value = multi_result::number(phase.shared_stages->at(stage));
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    return Poll(ResultPublication{take(builder.seal()), true});
  }
};
struct PartialFallback {
  unsigned turn = 0;
  std::optional<ResultBuilder> builder;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    if (phase.query.shared_stage == UINT32_MAX) {
      if (++turn <= 2) {
        ResultProgramNeed need;
        TileRequest request;
        request.members.emplace(
            0, take(TileSet::range(phase.resources, phase.query.tile_unit,
                                   phase.query.tile_unit + 1)));
        need.shared_stages.emplace_back(0, std::move(request));
        return Poll(std::move(need));
      }
      return Poll(ResultPublication{
          take(sdk::to_tile(phase, phase.shared_stages->at(0))), true});
    }
    if (!builder)
      builder = take(ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {}, phase.query.tile_extent));
    if (turn == 1) {
      ++turn;
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false, take(TileSet::range(phase.resources, 0, 1))});
      return Poll(std::move(need));
    }
    const auto tile = turn ? 1U : 0U;
    double value = 17;
    if (tile)
      check(phase.read_tensor(0, 0, {0}, &value, sizeof(value)));
    check(builder->publish_tensor(
        0, Region({{tile, 1}}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    ++turn;
    return Poll(ResultPublication{
        tile ? take(builder->seal()) : builder->reference(), tile != 0});
  }
};
// Fallback provenance must cross SDK stages, nested/reused grants and bundles.
enum class FallbackBoundary {
  Tensor,
  WholeStage,
  SharedRelay,
  Bundle,
  Partial
};
int fallback_cache_boundary(FallbackBoundary mode) {
  auto registry = std::make_shared<OperationRegistry>();
  unsigned starts[3]{}, sink_tiles[2]{};
  for (unsigned node = 0; node < 3; ++node) {
    OperationDefinition operation;
    operation.key = "test.fallback" + std::to_string(node);
    auto schema = multi_result::schema(
        ElementType::Float64,
        {mode == FallbackBoundary::Partial && node ? 2U : 1U});
    if (node == 1 && mode == FallbackBoundary::Bundle) {
      auto second = schema.tensors[0];
      second.key = "second";
      schema.tensors.push_back(std::move(second));
      schema.publication_scopes.push_back(
          {ResultPublicationPolicy::CompleteBundle, {0, 1}});
    }
    operation.traits.outputs = {multi_result::output("value", schema)};
    operation.traits.outputs[0].maximum_dependency_stages = 4;
    if (node) {
      operation.traits.input_count = 1;
      operation.traits.input_schema.resize(1);
      operation.traits.input_schema[0].tensor_key = "number";
    } else {
      operation.traits.supports_gpu = true;
      operation.traits.allows_cpu_fallback = true;
    }
    operation.start_result = [&, node, mode](const ResultProgramQuery& query,
                                             const auto& allocator) {
      ++starts[node];
      if (node == 2)
        ++sink_tiles[query.tile_unit];
      return ResultContinuation::make<CacheIdentity>(
          allocator,
          node != 0 && !(node == 1 && mode == FallbackBoundary::Bundle &&
                         query.tensor_slot == 0),
          mode == FallbackBoundary::Partial && node == 2 ? query.tile_unit : 0);
    };
    if (node == 1 && mode == FallbackBoundary::WholeStage)
      operation = sdk::with_whole_stage(std::move(operation), kWork);
    if (node == 1 && mode == FallbackBoundary::SharedRelay) {
      for (unsigned stage = 0; stage < 2; ++stage) {
        OperationSharedStage shared;
        shared.key = "stage" + std::to_string(stage);
        shared.result_schema = schema;
        shared.input_indices = {0};
        shared.continuation_bytes = 256;
        shared.maximum_dependency_stages = 4;
        shared.maximum_work = kWork;
        shared.maximum_output_payload_bytes = 8;
        operation.traits.shared_stages.push_back(std::move(shared));
      }
      operation.start_result = [&](const ResultProgramQuery& query,
                                   const auto& allocator) {
        ++starts[1];
        if (query.shared_stage == 0)
          return ResultContinuation::make<CacheIdentity>(allocator, true);
        return ResultContinuation::make<CacheRelay>(allocator);
      };
    }
    if (node == 1 && mode == FallbackBoundary::Partial) {
      OperationSharedStage shared;
      shared.key = "partial";
      shared.result_schema = schema;
      shared.input_indices = {0};
      shared.mandatory = true;
      shared.continuation_bytes = sizeof(PartialFallback);
      shared.maximum_dependency_stages = 4;
      shared.maximum_work = kWork;
      shared.maximum_output_payload_bytes = 16;
      operation.traits.outputs[0].continuation_bytes = sizeof(PartialFallback);
      operation.traits.shared_stages = {shared};
      operation.start_result = [](const auto&, const auto& allocator) {
        return ResultContinuation::make<PartialFallback>(allocator);
      };
    }
    check(registry->register_operation(std::move(operation)));
  }
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {
      {1, "test.fallback0", {}, {}},
      {2, "test.fallback1", {WorkflowNodeOutput{1, "value"}}, {}},
      {3, "test.fallback2", {WorkflowNodeOutput{2, "value"}}, {}}};
  document.outputs = {{"value", 3, "value"}};
  GraphContext graph(document);
  PlanningOptions options;
  options.execution_mode = ExecutionMode::NativeGpu;
  options.tile_extent = 1;
  auto plan = take(Compiler(registry).compile(graph, options)).plan;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.gpu_enabled = false;
  config.result_cache_bytes = 1024 * 1024;
  ExecutionContext execution(registry, config);
  for (unsigned run = 1; run <= 2; ++run) {
    auto result = execution.execute(plan);
    PS_REQUIRE_OK(result);
    PS_CHECK(multi_result::number(result.value().results.at("value")) == 17);
    PS_CHECK(starts[0] == run);
    PS_CHECK(result.value().diagnostics.fallback_reasons.size() == 1);
    if (mode == FallbackBoundary::Partial) {
      PS_CHECK(sink_tiles[0] == 1 && sink_tiles[1] == run);
      PS_CHECK(result.value().diagnostics.cache_hits == run - 1);
    } else {
      PS_CHECK(starts[2] == run);
      PS_CHECK(result.value().diagnostics.cache_hits == 0);
    }
  }
  return 0;
}
}  // namespace
int main() {
  return regression(false) || regression(true) || prefix_revisit() ||
         direct_validator_failure(false) || direct_validator_failure(true) ||
         grant_key_boundaries() || interleaved_commits(true) ||
         interleaved_commits(false) ||
         fallback_cache_boundary(FallbackBoundary::Tensor) ||
         fallback_cache_boundary(FallbackBoundary::WholeStage) ||
         fallback_cache_boundary(FallbackBoundary::SharedRelay) ||
         fallback_cache_boundary(FallbackBoundary::Bundle) ||
         fallback_cache_boundary(FallbackBoundary::Partial);
}
