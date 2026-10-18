#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "execution/execution_test_hooks.hpp"
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
using multi_result::check;
using multi_result::take;
struct Control {
  std::weak_ptr<const CpuStorage> source;
  std::atomic<unsigned> sources{0}, active{0}, peak{0};
};
struct Program {
  Control* control;
  unsigned mode;
  bool requested = false;
  Program(Control* control, unsigned mode) : control(control), mode(mode) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    if (mode && !requested) {
      requested = true;
      ResultProgramNeed need;
      for (unsigned port = 0; port < (mode == 3 ? 2U : 1U); ++port)
        need.tiles.push_back(
            {port, 0, false, take(TileSet::range(phase.resources, 0, 1))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    const auto active = ++control->active;
    auto peak = control->peak.load();
    while (peak < active &&
           !control->peak.compare_exchange_weak(peak, active)) {
    }
    struct Retire {
      Control* control;
      ~Retire() { --control->active; }
    } retire{control};
    double number = 2;
    if (mode) {
      check(phase.read_tensor(0, 0, {0}, &number, sizeof(number)));
      if (mode == 3) {
        double right = 0;
        check(phase.read_tensor(1, 0, {0}, &right, sizeof(right)));
        number += right;
        // The static VM retains executed tiles for the current activation.
        // Run retirement releases them after downstream consumers finish.
        if (control->source.expired())
          return Result<ResultProgramPoll>(
              Status{ErrorCode::OperationFailed,
                     "source released before downstream completion"});
      } else {
        if (control->source.expired())
          return Result<ResultProgramPoll>(
              Status{ErrorCode::OperationFailed, "source released during use"});
        number += mode;
      }
    } else {
      ++control->sources;
    }
    check(phase.consume_work(1));
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key, {}));

    auto made = phase.resources.allocator().allocate(sizeof(number));
    if (!made.ok())
      return Result<ResultProgramPoll>(made.status());
    auto buffer = made.take_value();
    std::memcpy(buffer.data(), &number, sizeof(number));
    auto storage = std::move(buffer).freeze();
    if (!mode)
      control->source = storage;
    check(builder.publish_tensor(0, Region::whole({1}), {0, {8}},
                                 std::move(storage)));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
std::shared_ptr<OperationRegistry> registry(Control* control) {
  auto registry = std::make_shared<OperationRegistry>();
  const std::string keys[] = {"source", "left", "right", "join"};
  for (unsigned mode = 0; mode < 4; ++mode) {
    OperationDefinition operation;
    operation.key = keys[mode];
    operation.traits.cacheable = false;
    operation.traits.input_count = mode == 3 ? 2 : mode ? 1 : 0;
    operation.traits.input_schema.resize(operation.traits.input_count);
    for (auto& input : operation.traits.input_schema) {
      input.tensor_key = "number";
    }
    operation.traits.outputs = {multi_result::output("value")};
    operation.start_result = [control, mode](const auto&,
                                             const auto& allocator) {
      return ResultContinuation::make<Program>(allocator, control, mode);
    };
    check(registry->register_operation(std::move(operation)));
  }
  check(registry->freeze());
  return registry;
}
std::mutex retirement_mutex;
std::condition_variable retirement_changed;
bool body_finished = false, release_body = false;
std::weak_ptr<const CpuStorage>* callback_storage = nullptr;
void hold_callback_owner() noexcept {
  if (!callback_storage || callback_storage->expired())
    return;
  std::unique_lock<std::mutex> lock(retirement_mutex);
  body_finished = true;
  retirement_changed.notify_all();
  retirement_changed.wait(lock, [] { return release_body; });
}
int callback_retirement() {
  Control control;
  auto operations = registry(&control);
  WorkflowDocument document;
  document.nodes = {{1, "source", {}, {}}};
  document.outputs = {{"result", 1, "value"}};
  GraphContext graph(document);
  auto compiled = take(Compiler(operations).compile(graph));
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 0;
  ExecutionContext execution(operations, config);
  for (bool cancel : {false, true}) {
    body_finished = release_body = false;
    callback_storage = &control.source;
    execution_testing::ExecutionTestHooks hooks;
    hooks.callback_body_finished = hold_callback_owner;
    execution_testing::install_execution_test_hooks(&hooks);
    CancellationSource cancellation;
    auto pending = std::async(std::launch::async, [&] {
      return execution.execute(compiled.plan.value(), {}, cancellation.token());
    });
    bool entered = false;
    {
      std::unique_lock<std::mutex> lock(retirement_mutex);
      entered = retirement_changed.wait_for(lock, std::chrono::seconds(5),
                                            [] { return body_finished; });
    }
    const bool returned_early =
        pending.wait_for(std::chrono::milliseconds(20)) ==
        std::future_status::ready;
    if (cancel)
      cancellation.cancel();
    {
      std::lock_guard<std::mutex> lock(retirement_mutex);
      release_body = true;
    }
    retirement_changed.notify_all();
    auto result = pending.get();
    execution_testing::install_execution_test_hooks(nullptr);
    callback_storage = nullptr;
    PS_CHECK(entered && !returned_early);
    PS_CHECK(!control.source.expired() || cancel);
    PS_CHECK(cancel
                 ? !result.ok() && result.status().code == ErrorCode::Cancelled
                 : result.ok());
    result = Result<ExecutionResult>(ExecutionResult{});
    PS_CHECK(control.source.expired());
    auto recovered = execution.execute(compiled.plan.value());
    PS_CHECK(recovered.ok() &&
             multi_result::number(recovered.value().results.at("result")) == 2);
  }
  PS_CHECK(take(execution.resource_budget())
               .statistics()
               .live[ResourceKind::Queue] == 0);
  return 0;
}
struct Scratch {
  std::uint64_t bytes;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto scratch = phase.allocator.allocate(bytes);
    if (!scratch.ok())
      return Result<ResultProgramPoll>(scratch.status());
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));

    auto output = take(phase.resources.allocator().allocate(8));
    const double number = 5;
    std::memcpy(output.data(), &number, 8);
    check(builder.publish_tensor(0, Region::whole({1}), {0, {8}},
                                 std::move(output).freeze()));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  } catch (const multi_result::Failure& failure) {
    return Result<ResultProgramPoll>(failure.status);
  }
};
int scratch_liveness() {
  auto operations = std::make_shared<OperationRegistry>();
  for (unsigned bytes : {32, 33}) {
    OperationDefinition op;
    op.key = bytes == 32 ? "scratch" : "excess";
    op.traits.cacheable = false;
    op.traits.workspace_bytes = 32;
    op.traits.outputs = {multi_result::output("value")};
    // The live continuation owns eight bytes alongside the 32-byte
    // workspace and eight-byte Result backing.
    op.traits.outputs[0].continuation_bytes = sizeof(Scratch);
    op.start_result = [bytes](const auto&, const auto& allocator) {
      return ResultContinuation::make<Scratch>(allocator, Scratch{bytes});
    };
    check(operations->register_operation(std::move(op)));
  }
  check(operations->freeze());
  for (const auto& key : {"scratch", "excess"}) {
    WorkflowDocument document;
    document.nodes = {{1, key, {}, {}}};
    document.outputs = {{"result", 1, "value"}};
    GraphContext graph(document);
    auto plan = take(Compiler(operations).compile(graph));
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = 48;
    ExecutionContext exact(operations, config);
    auto result = exact.execute(plan.plan.value());
    const auto stats = take(exact.resource_budget()).statistics();
    if (std::string(key) == "scratch") {
      PS_CHECK(result.ok() &&
               multi_result::number(result.value().results.at("result")) == 5);
      PS_CHECK(stats.peak[ResourceKind::Payload] == 48);
    } else {
      PS_CHECK(!result.ok() &&
               result.status().code == ErrorCode::ResourceExhausted);
    }
    PS_CHECK(stats.live[ResourceKind::Queue] == 0);
    config.maximum_live_bytes = 47;
    ExecutionContext insufficient(operations, config);
    auto failed = insufficient.execute(plan.plan.value());
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::ResourceExhausted);
  }
  return 0;
}
int graph_liveness() {
  // Four live Result backings plus the one executing continuation.
  constexpr auto peak_payload = sizeof(Program) + 4 * sizeof(double);
  Control control;
  auto operations = registry(&control);
  WorkflowDocument document;
  document.nodes = {
      {1, "source", {}, {}},
      {2, "left", {WorkflowNodeOutput{1, "value"}}, {}},
      {3, "right", {WorkflowNodeOutput{1, "value"}}, {}},
      {4,
       "join",
       {WorkflowNodeOutput{2, "value"}, WorkflowNodeOutput{3, "value"}},
       {}}};
  document.outputs = {{"result", 4, "value"}};
  GraphContext graph(document);
  auto plan = take(Compiler(operations).compile(graph));
  ResultRef retained;
  ResourceBudget root;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 2;
    config.result_cache_bytes = 0;
    config.maximum_live_bytes = peak_payload;
    ExecutionContext execution(operations, config);
    root = take(execution.resource_budget());
    ExecutionOptions options;
    options.maximum_parallelism = 2;
    auto result = execution.execute(plan.plan.value(), {}, {}, options);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && control.source.expired() && control.peak == 1);
    PS_CHECK(result.value().diagnostics.peak_active_tasks == 1);
    PS_CHECK(root.statistics().peak[ResourceKind::Payload] == peak_payload);
    retained = result.value().results.at("result");
    PS_CHECK(multi_result::number(retained) == 7 && retained.owned_by(root));
    auto held = execution.execute(plan.plan.value(), {}, {}, options);
    PS_CHECK(!held.ok() && held.status().code == ErrorCode::ResourceExhausted);
    retained = {};
    result = Result<ExecutionResult>(ExecutionResult{});
    auto recovered = execution.execute(plan.plan.value(), {}, {}, options);
    PS_CHECK(recovered.ok());
    retained = recovered.value().results.at("result");
    PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  }
  PS_CHECK(multi_result::number(retained) == 7 && retained.owned_by(root));
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 8);
  retained = {};
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = 0;
  config.maximum_live_bytes = peak_payload - 1;
  ExecutionContext insufficient(operations, config);
  auto failed = insufficient.execute(plan.plan.value());
  PS_CHECK(!failed.ok() &&
           failed.status().code == ErrorCode::ResourceExhausted);
  PS_CHECK(control.active == 0 && control.source.expired());
  PS_CHECK(take(insufficient.resource_budget())
               .statistics()
               .live[ResourceKind::Queue] == 0);
  return 0;
}
}  // namespace
int main() try {
  PS_CHECK(callback_retirement() == 0);
  PS_CHECK(graph_liveness() == 0);
  PS_CHECK(scratch_liveness() == 0);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
