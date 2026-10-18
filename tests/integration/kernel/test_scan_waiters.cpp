#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#include "execution/execution_test_hooks.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false, timed_out = false;
  std::atomic<unsigned> borrowed{0}, starts{0}, destroyed{0}, prefixes{0};
  void hold() {
    std::unique_lock<std::mutex> lock(mutex);
    if (entered)
      return;
    entered = true;
    changed.notify_all();
    timed_out = !changed.wait_for(lock, std::chrono::seconds(10),
                                  [&] { return released; });
  }
  bool wait() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    changed.notify_all();
  }
};
Gate* active = nullptr;
void published() noexcept {
  active->hold();
}
void borrowed() noexcept {
  ++active->borrowed;
}
struct Scan {
  Gate* counts;
  unsigned stage = 0;
  bool restored = false;
  double carry = 0;
  explicit Scan(Gate* gate) : counts(gate) { ++counts->starts; }
  ~Scan() { ++counts->destroyed; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (!stage++) {
      auto saved = take(phase.checkpoint_before(1, 0));
      if (saved) {
        carry = multi_result::number(saved->state());
        restored = true;
      }
      ResultProgramNeed need;
      need.tiles.push_back(
          {0, 0, false, take(TileSet::range(phase.resources, 0, 1))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (!restored) {
      check(phase.read_tensor(0, 0, {0}, &carry, sizeof(carry)));
      ++counts->prefixes;
      auto state =
          multi_result::binding(phase.resources, "carry", carry).result;
      check(phase.checkpoint_publish(1, 0, state));
    }
    double next = 0;
    check(phase.read_tensor(0, 0, {1}, &next, sizeof(next)));
    if (!std::isfinite(next))
      return Result<ResultProgramPoll>(
          Status{ErrorCode::OperationFailed, "nonfinite scan input 1"});
    const double sum = carry + next;
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&sum), sizeof(sum))));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
int concurrent_scans(bool cancel_owner, bool finite, bool cache) {
  Gate gate;
  auto registry = std::make_shared<OperationRegistry>();
  auto input_schema = multi_result::schema(ElementType::Float64, {2});
  OperationDefinition operation;
  operation.key = "test.checkpoint_scan";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);

  operation.traits.input_schema[0].tensor_key = "number";
  operation.traits.outputs = {multi_result::output("value")};
  operation.traits.outputs[0].continuation_bytes = sizeof(Scan);
  operation.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<Scan>(allocator, &gate);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "x", input_schema)};
  document.nodes = {
      {1, "test.checkpoint_scan", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"y", 1, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan.value();
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = cache ? 1048576 : 0;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  const double values[]{1,
                        finite ? 2 : std::numeric_limits<double>::infinity()};
  auto builder = take(ResultBuilder::start(root, input_schema, "input"));
  check(builder.publish_tensor(
      0, Region::whole({2}),
      ByteView(reinterpret_cast<const std::uint8_t*>(values), sizeof(values))));
  auto frozen = take(context.freeze(plan, {{{"x", take(builder.seal())}}}));
  execution_testing::ExecutionTestHooks hooks;
  hooks.checkpoint_published = published;
  hooks.checkpoint_borrowed = borrowed;
  active = &gate;
  execution_testing::install_execution_test_hooks(&hooks);
  struct Cleanup {
    ~Cleanup() {
      execution_testing::install_execution_test_hooks(nullptr);
      active = nullptr;
    }
  } cleanup;
  CancellationSource cancellation;
  // The same Whole unit misses twice while the first callback is held.
  auto owner = std::async(std::launch::async, [&] {
    return context.execute(frozen, cancellation.token());
  });
  const bool entered = gate.wait();
  auto peer =
      std::async(std::launch::async, [&] { return context.execute(frozen); });
  const bool independent =
      peer.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  if (cancel_owner)
    cancellation.cancel();
  gate.release();
  auto first = owner.get();
  auto second = peer.get();
  PS_CHECK(entered && independent && !gate.timed_out);
  PS_CHECK(gate.starts == 2 && gate.destroyed == 2);
  PS_CHECK(gate.prefixes == 1);
  PS_CHECK(gate.borrowed > 0);
  for (const auto* result : {&first, &second}) {
    if (result == &first && cancel_owner) {
      PS_CHECK(result->status().code == ErrorCode::Cancelled);
    } else if (!finite) {
      PS_CHECK(result->status().code == ErrorCode::OperationFailed &&
               result->status().message == "nonfinite scan input 1");
    } else {
      PS_REQUIRE_OK(*result);
      PS_CHECK(multi_result::number(result->value().results.at("y")) == 3);
      auto record = take(result->value().dependencies.tile_record({0, 0, 0}));
      PS_CHECK(record && record->inputs.size() == 1 &&
               record->inputs[0].tiles.size() == 1 &&
               record->inputs[0].tiles.contains(0));
    }
  }
  if (finite) {
    auto warm = take(context.execute(frozen));
    PS_CHECK(multi_result::number(warm.results.at("y")) == 3);
    PS_CHECK(cache ? warm.diagnostics.cache_hits > 0
                   : warm.diagnostics.cache_hits == 0);
    context.clear_result_cache();
    const auto before = gate.prefixes.load();
    auto cold = take(context.execute(frozen));
    PS_CHECK(multi_result::number(cold.results.at("y")) == 3);
    PS_CHECK(gate.prefixes == before + 1 && cold.diagnostics.cache_hits == 0);
  }
  PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  return 0;
}
// Optional checkpoint lookup must not poison the Run when metadata is scarce.
// A hit or miss keeps the instruction's already-bound static grants.
struct CapacityProbe {
  Gate gate;
  std::uint64_t remaining = 0;
  bool hit = false, grants_complete = false;
  ErrorCode failure = ErrorCode::Internal;
};
struct CapacityScan {
  CapacityProbe* probe;
  unsigned ordinal, turn = 0;
  explicit CapacityScan(CapacityProbe* state)
      : probe(state), ordinal(++state->gate.starts) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    if (ordinal == 2) {
      const auto available = phase.resources.available_capacity();
      const auto bytes = available[ResourceKind::Metadata] - probe->remaining -
                         ResourceBudget::lease_metadata_bytes();
      auto held =
          take(phase.resources.reserve(ResourceCapacity::host(bytes, bytes)));
      auto saved = phase.checkpoint_before(1, 0);
      held = {};
      if (!saved.ok())
        return Answer(saved.status());
      probe->hit = saved.value().has_value();
      probe->failure = phase.failure->load();
      probe->grants_complete = phase.tensors->size() == 100;
    } else if (!turn++) {
      ResultProgramNeed need;
      for (unsigned port = 0; port < 100; ++port)
        need.tiles.push_back(
            {port, 0, false, take(TileSet::range(phase.resources, 0, 1))});
      return Answer(std::move(need));
    } else {
      auto state = multi_result::binding(phase.resources, "carry", 7).result;
      check(phase.checkpoint_publish(1, 0, state));
    }
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    const double value = 7;
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
int checkpoint_restore_capacity(bool enough) {
  CapacityProbe probe;
  probe.remaining = enough ? 65536 : 2048;
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "test.checkpoint_capacity";
  operation.traits.input_count = 100;
  operation.traits.input_schema.resize(100);
  for (auto& input : operation.traits.input_schema)
    input.tensor_key = "number";
  operation.traits.outputs = {multi_result::output("value")};
  operation.traits.outputs[0].continuation_bytes = sizeof(CapacityScan);
  operation.traits.cacheable = false;
  operation.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<CapacityScan>(allocator, &probe);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  WorkflowNode node{1, "test.checkpoint_capacity", {}, {}};
  for (unsigned port = 0; port < 100; ++port) {
    document.inputs.push_back(
        multi_result::declaration(port + 1, "input" + std::to_string(port)));
    node.inputs.push_back(WorkflowInputReference{port + 1});
  }
  document.nodes.push_back(std::move(node));
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan.value();
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  ExecutionBindings bindings;
  for (unsigned port = 0; port < 100; ++port)
    bindings.inputs.push_back(
        multi_result::binding(root, "input" + std::to_string(port), 1));
  auto frozen = take(context.freeze(plan, bindings));
  execution_testing::ExecutionTestHooks hooks;
  hooks.checkpoint_published = published;
  hooks.checkpoint_borrowed = borrowed;
  active = &probe.gate;
  execution_testing::install_execution_test_hooks(&hooks);
  struct Cleanup {
    ~Cleanup() {
      execution_testing::install_execution_test_hooks(nullptr);
      active = nullptr;
    }
  } cleanup;
  auto owner =
      std::async(std::launch::async, [&] { return context.execute(frozen); });
  const bool entered = probe.gate.wait();
  auto peer =
      std::async(std::launch::async, [&] { return context.execute(frozen); });
  const bool finished =
      peer.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  probe.gate.release();
  auto first = owner.get();
  auto second = peer.get();
  PS_CHECK(entered && finished && !probe.gate.timed_out);
  PS_REQUIRE_OK(first);
  PS_REQUIRE_OK(second);
  PS_CHECK(probe.hit == enough && probe.failure == ErrorCode::Ok);
  PS_CHECK(probe.grants_complete);
  PS_CHECK(probe.gate.borrowed == (enough ? 1U : 0U));
  auto record = take(second.value().dependencies.tile_record({0, 0, 0}));
  PS_CHECK(record && record->inputs.size() == 100);
  PS_CHECK(multi_result::number(second.value().results.at("value")) == 7);
  return 0;
}
struct BlockCapacityProbe {
  std::uint64_t remaining;
  unsigned computations = 0;
  ErrorCode failure = ErrorCode::Internal;
};
struct BlockCapacityScan {
  BlockCapacityProbe* probe;
  explicit BlockCapacityScan(BlockCapacityProbe* probe) : probe(probe) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Answer = Result<ResultProgramPoll>;
    auto incoming = multi_result::binding(phase.resources, "carry", 7).result;
    const auto compute = [&]() -> Result<ResultRef> {
      ++probe->computations;
      return Result<ResultRef>(incoming);
    };
    auto warm = phase.block(1, 0, 1, 0, incoming, compute);
    if (!warm.ok())
      return Answer(warm.status());
    const auto available = phase.resources.available_capacity();
    const auto bytes = available[ResourceKind::Metadata] - probe->remaining -
                       ResourceBudget::lease_metadata_bytes();
    auto held =
        take(phase.resources.reserve(ResourceCapacity::host(bytes, bytes)));
    auto restored = phase.block(1, 0, 1, 0, incoming, compute);
    held = {};
    probe->failure = phase.failure->load();
    if (!restored.ok())
      return Answer(restored.status());
    const double value = multi_result::number(restored.value());
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const std::uint8_t*>(&value),
                 sizeof(value))));
    return Answer(ResultPublication{take(builder.seal()), true});
  }
};
int block_restore_capacity(bool cache, bool enough) {
  BlockCapacityProbe probe{enough ? 65536U : 4096U};
  auto registry = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "test.block_capacity";
  operation.traits.outputs = {multi_result::output("value")};
  operation.traits.outputs[0].continuation_bytes = sizeof(BlockCapacityScan);
  operation.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<BlockCapacityScan>(allocator, &probe);
  };
  check(registry->register_operation(std::move(operation)));
  check(registry->freeze());
  WorkflowDocument document;
  document.nodes = {{1, "test.block_capacity", {}, {}}};
  document.outputs = {{"value", 1, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(registry).compile(graph)).plan.value();
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = cache ? 1048576 : 0;
  ExecutionContext context(registry, config);
  auto result = context.execute(plan);
  PS_REQUIRE_OK(result);
  PS_CHECK(probe.failure == ErrorCode::Ok);
  PS_CHECK(probe.computations == (cache && enough ? 1U : 2U));
  if (cache)
    PS_CHECK(context.cache_statistics().hits == 1);
  PS_CHECK(result.value().diagnostics.block_cache_hits ==
           (cache && enough ? 1U : 0U));
  PS_CHECK(multi_result::number(result.value().results.at("value")) == 7);
  return 0;
}
}  // namespace
int main() {
  for (bool cache : {false, true})
    for (bool cancel : {false, true})
      for (bool finite : {false, true})
        PS_CHECK(concurrent_scans(cancel, finite, cache) == 0);
  PS_CHECK(checkpoint_restore_capacity(false) == 0);
  PS_CHECK(checkpoint_restore_capacity(true) == 0);
  PS_CHECK(block_restore_capacity(true, false) == 0);
  PS_CHECK(block_restore_capacity(true, true) == 0);
  PS_CHECK(block_restore_capacity(false, false) == 0);
  return 0;
}
