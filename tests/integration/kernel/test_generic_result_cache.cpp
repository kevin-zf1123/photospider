#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "execution/result_cache.hpp"
#include "execution/structured_result_cache.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/result_input_fixture.hpp"
#include "support/test_support.hpp"

namespace {
int cache_retains_resource_root() {
  using namespace ps;  // NOLINT(build/namespaces)
  using execution_internal::StructuredCacheState;
  using execution_internal::StructuredResultCache;
  ResourceBudget original, replacement;
  std::unique_ptr<StructuredCacheState> state;
  std::unique_ptr<StructuredResultCache> cache;
  struct Host final : execution_internal::StructuredCacheWorkServices {
    Status charge(std::uint64_t) override { return Status::success(); }
    CancellationToken cancellation() override { return {}; }
    ErrorCode stop() override { return ErrorCode::Ok; }
  } host;
  std::recursive_mutex gate;
  std::uint64_t observed = 0;
  {
    ResourceBudget wrapper = original;
    state = std::make_unique<StructuredCacheState>(wrapper, 1000);
    cache = std::make_unique<StructuredResultCache>(wrapper, 1000);
    wrapper = replacement;
    PS_CHECK(state->charge(7).ok());
    PS_CHECK(cache->charge(11, gate, host, observed).ok());
    PS_CHECK(original.statistics().issued.work == 18);
    PS_CHECK(replacement.statistics().issued.work == 0);
    auto facts = cache->supplied_facts({}, {}, 4096, host);
    PS_CHECK(facts.ok());
    PS_CHECK(facts.value().get_allocator().owned_by(original));
  }
  // Both components outlive the caller's wrapper and keep the same root.
  PS_CHECK(state->charge(13).ok());
  PS_CHECK(cache->charge(17, gate, host, observed).ok());
  PS_CHECK(original.statistics().issued.work == 48);
  PS_CHECK(replacement.statistics().issued.work == 0);
  PS_CHECK(observed == 28);
  auto facts = cache->supplied_facts({}, {}, 4096, host);
  PS_CHECK(facts.ok());
  PS_CHECK(facts.value().get_allocator().owned_by(original));
  return 0;
}
enum class CacheMode { Constant, First, Sum };
struct CacheProgram {
  CacheMode mode;
  std::shared_ptr<std::atomic<unsigned>> calls;
  bool requested = false;
  CacheProgram(CacheMode selected, std::shared_ptr<std::atomic<unsigned>> count)
      : mode(selected), calls(std::move(count)) {}
  ps::Result<ps::ResultProgramPoll> poll(
      const ps::ResultProgramPhase& phase) try {
    using namespace ps;  // NOLINT(build/namespaces)
    using multi_result::check;
    using multi_result::take;
    auto scratch =
        take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      for (std::uint32_t i = 0; i < phase.query.inputs.size(); ++i)
        need.tiles.push_back(
            {i, 0, false,
             ps::TileSet::range(phase.resources, 0, 1).take_value()});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (calls)
      ++*calls;
    const auto& schema = *phase.query.output.result_schema;
    auto builder = take(ResultBuilder::start(phase.resources, schema,
                                             phase.query.semantic_key));
    const auto region = Region::whole(schema.tensors[0].sample_shape());
    if (mode == CacheMode::First) {
      auto window = take(phase.tensors->at({0, 0}).acquire(region));
      ResultTensorViewTransform identity;
      identity.source_axes = {{0, 0, 1}};
      check(builder.publish_tensor_view(0, region, window, identity));
    } else {
      double value = mode == CacheMode::Constant ? 2 : 0;
      for (std::uint32_t i = 0; i < phase.query.inputs.size(); ++i) {
        const auto type = phase.query.inputs[i]
                              .result_schema->tensors[0]
                              .descriptor.element_type;
        if (type == ElementType::Float32) {
          float input = 0;
          check(phase.read_tensor(i, 0, {0}, &input, sizeof(input)));
          if (mode == CacheMode::Sum)
            value += input;
        } else {
          double input = 0;
          check(phase.read_tensor(i, 0, {0}, &input, sizeof(input)));
          if (mode == CacheMode::Sum)
            value += input;
        }
      }
      check(phase.consume_work(phase.query.inputs.size()));
      auto bytes = take(phase.allocator.allocate(8));
      std::memcpy(bytes.data(), &value, sizeof(value));
      check(builder.publish_tensor(0, region, {0, {8}},
                                   std::move(bytes).freeze()));
    }
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return ps::Result<ps::ResultProgramPoll>(failure.status);
  }
};
ps::OperationDefinition cache_operation(
    std::string key, const ps::SchemaTemplate& output,
    CacheMode mode = CacheMode::First, unsigned inputs = 1,
    std::shared_ptr<std::atomic<unsigned>> calls = {},
    ps::ElementType input_type = ps::ElementType::Float64) {
  ps::OperationDefinition operation;
  operation.key = std::move(key);
  operation.traits.input_count = inputs;
  operation.traits.input_schema.resize(inputs);
  for (auto& port : operation.traits.input_schema) {
    port.element_type = static_cast<std::uint32_t>(input_type);
    port.rank = 1;
  }
  auto result = multi_result::output("value", output);
  result.continuation_bytes = sizeof(CacheProgram);
  operation.traits.outputs = {std::move(result)};
  operation.traits.workspace_bytes = 8;
  operation.start_result = [mode, calls](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<CacheProgram>(allocator, mode, calls);
  };
  return operation;
}
ps::ExecutionContextConfig configuration(std::uint64_t bytes = 65536) {
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = bytes;
  config.managed_resources = ps::ResourceLimits{};
  return config;
}
int opaque_result_cache_preservation() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto operations = std::make_shared<OperationRegistry>();
  auto input_schema = multi_result::schema(ElementType::Float32);
  auto output_schema = multi_result::schema();
  output_schema.tensors[0].atomic_trailing_axes = 1;
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  PS_CHECK(operations
               ->register_operation(
                   cache_operation("source", output_schema, CacheMode::Constant,
                                   1, calls, ElementType::Float32))
               .ok());
  PS_CHECK(
      operations->register_operation(cache_operation("identity", output_schema))
          .ok());
  PS_CHECK(operations->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", input_schema)};
  document.nodes = {{1, "source", {WorkflowInputReference{1}}, {}},
                    {2, "identity", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(operations).compile(graph).take_value().plan.value();
  ExecutionContext execution(operations, configuration());
  auto root = execution.resource_budget().take_value();
  ExecutionBindings bindings{
      {multi_result::binding(root, "input", 1, input_schema)}};
  auto copied_document = document;
  copied_document.outputs = {{"result", 2, "value"}};
  GraphContext copied_graph(copied_document);
  auto copied_plan =
      Compiler(operations).compile(copied_graph).take_value().plan.value();
  for (unsigned run = 0; run < 2; ++run) {
    auto result = execution.execute(plan, bindings);
    PS_CHECK(result.ok());
    const auto& output = result.value().results.at("result");
    PS_CHECK(multi_result::number(output) == 2);
    PS_CHECK(output.schema().tensors[0].atomic_trailing_axes == 1);
    auto copied = execution.execute(copied_plan, bindings);
    PS_CHECK(copied.ok() &&
             multi_result::number(copied.value().results.at("result")) == 2);
    PS_CHECK(copied.value()
                 .results.at("result")
                 .schema()
                 .tensors[0]
                 .atomic_trailing_axes == 1);
    PS_CHECK(*calls == 1);
  }
  return 0;
}
int result_content_bits() {
  using namespace ps;  // NOLINT(build/namespaces)
  for (auto type : {ElementType::UInt8, ElementType::Int64,
                    ElementType::Float32, ElementType::Float64}) {
    const auto width = TensorSpan::element_size(type);
    auto schema = multi_result::schema(type, {3});
    auto registry = std::make_shared<OperationRegistry>();
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    PS_CHECK(registry
                 ->register_operation(cache_operation(
                     "bits.identity", schema, CacheMode::First, 1, calls, type))
                 .ok());
    PS_CHECK(registry->freeze().ok());
    WorkflowDocument doc;
    doc.inputs = {multi_result::declaration(1, "x", schema)};
    doc.nodes = {{1, "bits.identity", {WorkflowInputReference{1}}, {}}};
    doc.outputs = {{"y", 1, "value"}};
    GraphContext graph(doc);
    auto plan = Compiler(registry).compile(graph).take_value().plan.value();
    ExecutionContext context(registry, configuration());
    auto root = context.resource_budget().take_value();
    std::vector<std::uint8_t> backing(width * 3), logical(width * 3);
    for (std::size_t i = 0; i < backing.size(); ++i)
      backing[i] = static_cast<std::uint8_t>(255 - i);
    for (std::size_t i = 0; i < 3; ++i)
      std::memcpy(logical.data() + i * width, backing.data() + (2 - i) * width,
                  width);
    auto reversed =
        TensorSpan::create({type, {3}}, Region::whole({3}),
                           {2 * width, {-static_cast<std::int64_t>(width)}},
                           backing)
            .take_value();
    const auto bind = [&](const TensorSpan& value) {
      return ExecutionBindings{
          {{"x", result_input_fixture::source(root, value, &schema)}}};
    };
    auto first = context.execute(plan, bind(reversed));
    PS_CHECK(first.ok() && *calls == 1 &&
             result_input_fixture::bytes(first.value().results.at("y")) ==
                 logical);
    auto dense =
        TensorSpan::create({type, {3}}, Region::whole({3}),
                           {0, {static_cast<std::int64_t>(width)}}, logical)
            .take_value();
    auto equivalent = context.execute(plan, bind(dense));
    PS_CHECK(equivalent.ok() && *calls == 1 &&
             result_input_fixture::bytes(equivalent.value().results.at("y")) ==
                 logical);
    logical[width] ^= 1;
    auto edited =
        TensorSpan::create({type, {3}}, Region::whole({3}),
                           {0, {static_cast<std::int64_t>(width)}}, logical)
            .take_value();
    auto changed = context.execute(plan, bind(edited));
    PS_CHECK(changed.ok() && *calls == 2 &&
             result_input_fixture::bytes(changed.value().results.at("y")) ==
                 logical);
    // Optional verification fuel never prevents the normal identity operation.
    ExecutionOptions bounded;
    bounded.maximum_dependency_cache_work = 1;
    auto uncached = context.execute(plan, bind(edited), {}, bounded);
    PS_CHECK(uncached.ok() && *calls == 3 &&
             result_input_fixture::bytes(uncached.value().results.at("y")) ==
                 logical);
  }
  return 0;
}
int fragment_lru_storage() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  ResourceLimits limits;
  limits.capacity[ResourceKind::Payload] = 256;
  auto budget = std::make_shared<ResourceBudget>(limits);
  ResultCache cache(24, budget);
  const TensorDescriptor descriptor{ElementType::Float32, {4}};
  const auto make = [&](Region region) {
    auto writer =
        TensorWriter::allocate(descriptor, region, budget->allocator())
            .take_value();
    for (std::uint64_t i = 0; i < region.dimensions()[0].extent; ++i) {
      const float value =
          static_cast<float>(region.dimensions()[0].offset + i + 1);
      std::memcpy(writer.data() + i * 4, &value, 4);
    }
    return std::move(writer).publish().take_value();
  };
  // First fragment retains 16 backing bytes despite its 8-byte logical region.
  auto large = make(Region::whole({4}));
  const std::vector<TensorSpan> old{large.view(Region({{0, 2}})).take_value(),
                                    make(Region({{2, 2}}))};
  const auto epoch = cache.epoch();
  cache.put("old-head", old[0], epoch);
  cache.put("old-tail", old[1], epoch);
  PS_CHECK(cache.statistics().retained_bytes == 24 &&
           cache.statistics().entries == 2 &&
           cache.statistics().evictions == 0);
  cache.put("unrelated", make(Region({{2, 2}})), cache.epoch());
  PS_CHECK(!cache.get("old-head").valid());
  PS_CHECK(cache.get("old-tail").valid());
  // Only old [2,4) survives. Recompute with [0,3)+[3,4), same logical result.
  const std::vector<TensorSpan> next{make(Region({{0, 3}})),
                                     make(Region({{3, 1}}))};
  cache.put("new-head", next[0], epoch);
  cache.put("new-tail", next[1], epoch);
  std::vector<TensorSpan> retained{cache.get("new-head"),
                                   cache.get("new-tail")};
  auto result =
      Tensor::create(descriptor, Footprint::all({4}).take_value(), retained);
  PS_CHECK(result.ok());
  for (std::uint64_t i = 0; i < 4; ++i) {
    float value = 0;
    PS_CHECK(result.value().read({i}, &value, 4).ok() && value == i + 1);
  }
  cache.clear();
  cache.put("stale", next[0], epoch);
  PS_CHECK(cache.epoch() != epoch && !cache.get("stale").valid());
  PS_CHECK(cache.statistics().retained_bytes == 0);
  return 0;
}
int cache_shared_route_budget() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  const auto schema = multi_result::schema();
  auto leaf = cache_operation("cache.whole", schema);
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto merge = cache_operation("cache.merge", schema, CacheMode::Sum, 2, calls);
  auto branch1 = cache_operation("cache.branch1", schema);
  auto branch2 = cache_operation("cache.branch2", schema);
  PS_CHECK(registry->register_operation(leaf).ok());
  PS_CHECK(registry->register_operation(branch1).ok());
  PS_CHECK(registry->register_operation(branch2).ok());
  PS_CHECK(registry->register_operation(merge).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "x", schema)};
  document.nodes = {
      {1, leaf.key, {WorkflowInputReference{1}}, {}},
      {2, branch1.key, {WorkflowNodeOutput{1, "value"}}, {}},
      {3, branch2.key, {WorkflowNodeOutput{1, "value"}}, {}},
      {4,
       merge.key,
       {WorkflowNodeOutput{2, "value"}, WorkflowNodeOutput{3, "value"}},
       {}}};
  document.outputs = {{"y", 4, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan.value();
  ExecutionContext execution(registry, configuration());
  auto resource_root = execution.resource_budget().take_value();
  auto run_plan = plan;
  ExecutionBindings run_bindings = {
      {multi_result::binding(resource_root, "x", 7, schema)}};
  const DemandQuery query{
      {"y",
       TileRequest{{{0, TileSet::range(resource_root, 0, 1).take_value()}}}}};
  PS_CHECK(execution.execute_fragments(run_plan, run_bindings, query).ok() &&
           *calls == 1);
  // Split the old shared producer into two equivalent new source nodes.
  // Fixed per-port content proofs allow equivalent cloned routes to reuse
  // the merge while rebuilding dependency evidence for the current plan.
  document.nodes = {
      {10, leaf.key, {WorkflowInputReference{1}}, {}},
      {11, leaf.key, {WorkflowInputReference{1}}, {}},
      {20, branch1.key, {WorkflowNodeOutput{10, "value"}}, {}},
      {21, branch2.key, {WorkflowNodeOutput{11, "value"}}, {}},
      {30,
       merge.key,
       {WorkflowNodeOutput{20, "value"}, WorkflowNodeOutput{21, "value"}},
       {}}};
  document.outputs = {{"y", 30, "value"}};
  GraphContext changed(document);
  auto changed_plan =
      Compiler(registry).compile(changed).take_value().plan.value();
  auto changed_run_plan = changed_plan;
  ExecutionBindings changed_run_bindings = {
      {multi_result::binding(resource_root, "x", 7, schema)}};
  auto result = execution.execute_fragments(changed_run_plan,
                                            changed_run_bindings, query);
  PS_CHECK(result.ok() && *calls == 1);
  double actual = 0;
  PS_CHECK(result_input_fixture::read(result.value().results.at("y"), {0},
                                      &actual, sizeof(actual))
               .ok() &&
           actual == 14);
  auto warm = execution.execute_fragments(changed_run_plan,
                                          changed_run_bindings, query);
  PS_REQUIRE_OK(warm);
  PS_CHECK(*calls == 1 && warm.value().diagnostics.cache_hits > 0);
  ResourceVector<TileChange> changes{
      ResourceAllocator<TileChange>(resource_root)};
  changes.push_back({PlanWorkflowInput{0}, 0, 0,
                     TileSet::from_indices(resource_root, {0}).take_value(),
                     false});
  auto dirty = warm.value().dependencies.dirty_units(changes);
  PS_CHECK(dirty.ok());
  const TileUnit output{changed_plan.output("y").record_index, 0, 0};
  PS_CHECK(std::find(dirty.value().begin(), dirty.value().end(), output) !=
           dirty.value().end());
  return 0;
}
int diamond_cache_support() {
  using namespace ps;                      // NOLINT(build/namespaces)
  using namespace ps::execution_internal;  // NOLINT(build/namespaces)
  auto registry = std::make_shared<OperationRegistry>();
  const auto schema = multi_result::schema(ElementType::Float32);
  auto leaf = cache_operation("leaf", schema, CacheMode::First, 1, {},
                              ElementType::Float32);
  auto merge = cache_operation("merge", schema, CacheMode::First, 4, {},
                               ElementType::Float32);
  PS_CHECK(registry->register_operation(leaf).ok());
  PS_CHECK(registry->register_operation(merge).ok());
  PS_CHECK(registry->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "x", schema)};
  document.nodes = {{1, "leaf", {WorkflowInputReference{1}}, {}}};
  std::vector<WorkflowInput> siblings;
  for (std::uint64_t i = 2; i <= 5; ++i) {
    document.nodes.push_back({i, "leaf", {WorkflowNodeOutput{1, "value"}}, {}});
    siblings.push_back(WorkflowNodeOutput{i, "value"});
  }
  document.nodes.push_back({6, "merge", siblings, {}});
  document.outputs = {{"y", 6, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(registry).compile(graph).take_value().plan.value();
  const TileRequest q{
      {{0, TileSet::range(ResourceBudget{}, 0, 1).take_value()}}};
  // Public execution and retained-result replay must preserve diamond ancestry.
  ExecutionContext context(registry, configuration());
  auto run_plan = plan;
  ExecutionBindings run_bindings = {{multi_result::binding(
      context.resource_budget().take_value(), "x", 7, schema)}};
  auto computed = context.execute_fragments(run_plan, run_bindings, {{"y", q}});
  PS_CHECK(computed.ok());
  const TileUnit unit{plan.output("y").record_index, 0, 0};
  auto support = computed.value().dependencies.tile_record(unit).take_value();
  const auto external_evidence = [&](const auto& record) {
    if (!record || record->inputs.size() != 1)
      return false;
    const auto& input = record->inputs[0];
    const auto* source = std::get_if<PlanWorkflowInput>(&input.producer);
    return source && source->declaration_index == 0 &&
           input.producer_output == 0 && input.tensor_slot == 0 &&
           input.tiles == q.members.at(0);
  };
  PS_CHECK(external_evidence(support));
  auto reused = context.execute_fragments(run_plan, run_bindings, {{"y", q}});
  PS_REQUIRE_OK(reused);
  PS_CHECK(reused.value().diagnostics.cache_hits > 0);
  auto replayed = reused.value().dependencies.tile_record(unit).take_value();
  PS_CHECK(external_evidence(replayed));
  auto root = context.resource_budget().take_value();
  ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
  changes.push_back({PlanWorkflowInput{0}, 0, 0,
                     TileSet::from_indices(root, {0}).take_value(), false});
  auto dirty = reused.value().dependencies.dirty_units(changes).take_value();
  PS_CHECK(std::find(dirty.begin(), dirty.end(), unit) != dirty.end());
  return 0;
}
}  // namespace
int main() {
  PS_CHECK(cache_retains_resource_root() == 0);
  PS_CHECK(opaque_result_cache_preservation() == 0);
  PS_CHECK(result_content_bits() == 0);
  PS_CHECK(fragment_lru_storage() == 0);
  PS_CHECK(cache_shared_route_budget() == 0);
  PS_CHECK(diamond_cache_support() == 0);
  return 0;
}
