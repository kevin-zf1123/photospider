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
  std::mutex mutex;
  std::condition_variable changed;
  bool overlap = true, left_entered = false, right_done = false;
  bool check_release = true, cancel = false;
  unsigned backed = 0;
  bool allocation_failed = false;
  std::weak_ptr<const CpuStorage> source;
  CancellationSource cancellation;
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
        need.tensors.push_back({port, 0, take(Footprint::all({1})), 1});
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
        if (control->check_release && !control->source.expired())
          return Result<ResultProgramPoll>(Status{
              ErrorCode::OperationFailed, "source retained after last use"});
      } else {
        std::unique_lock<std::mutex> lock(control->mutex);
        if (mode == 1) {
          control->left_entered = true;
          control->changed.notify_all();
          if (control->overlap &&
              !control->changed.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return control->right_done; }))
            return Result<ResultProgramPoll>(Status{
                ErrorCode::OperationFailed, "independent right did not run"});
        } else {
          if (control->overlap &&
              !control->changed.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return control->left_entered; }))
            return Result<ResultProgramPoll>(Status{
                ErrorCode::OperationFailed, "independent left did not run"});
          control->right_done = true;
          if (control->cancel)
            control->cancellation.cancel();
          control->changed.notify_all();
        }
        if (control->source.expired())
          return Result<ResultProgramPoll>(
              Status{ErrorCode::OperationFailed, "source released during use"});
        number += mode;
      }
    } else {
      ++control->sources;
    }
    check(phase.consume_work(1));
    auto builder = take(ResultBuilder::start(
        phase.resources, *phase.query.output.result_schema,
        phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    auto relation_lease = take(phase.resources.reserve(ResourceCapacity::host(
        2 * sizeof(ResultRelation), 2 * sizeof(ResultRelation))));
    std::vector<ResultRelation> relations;
    relations.reserve(2);
    for (unsigned port = 0; port < (mode == 3 ? 2U : mode ? 1U : 0U); ++port)
      relations.push_back(take(ResultRelation::cartesian(
          phase.resources, 1,
          {port, 1, 0, 1, ResultSupportTarget::Tensor, 0})));
    auto relation =
        relations.empty()
            ? take(ResultRelation::cartesian(phase.resources, 1, {}))
            : take(ResultRelation::unite(phase.resources, relations));
    auto made = phase.resources.allocator().allocate(sizeof(number));
    if (!made.ok()) {
      std::lock_guard<std::mutex> lock(control->mutex);
      control->allocation_failed = true;
      control->changed.notify_all();
      return Result<ResultProgramPoll>(made.status());
    }
    auto buffer = made.take_value();
    if ((mode == 1 || mode == 2) && control->overlap) {
      std::unique_lock<std::mutex> lock(control->mutex);
      ++control->backed;
      control->changed.notify_all();
      if (!control->changed.wait_for(lock, std::chrono::seconds(2), [&] {
            return control->backed == 2 || control->allocation_failed ||
                   control->cancellation.token().cancelled();
          }))
        return Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed, "parallel backing barrier"});
      if (control->cancellation.token().cancelled())
        return Result<ResultProgramPoll>(Status{ErrorCode::Cancelled, {}});
      if (control->allocation_failed)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::ResourceExhausted, {}});
    }
    std::memcpy(buffer.data(), &number, sizeof(number));
    auto storage = std::move(buffer).freeze();
    if (!mode)
      control->source = storage;
    check(builder.publish_tensor(0, Region::whole({1}), {0, {8}},
                                 std::move(storage), std::move(relation),
                                 {true, true, true, true}));
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
      input.kind = OperationPortKind::Result;
      input.tensor_key = "number";
    }
    operation.traits.outputs = {multi_result::output("value")};
    operation.traits.outputs[0].region_rule = OperationRegionRule::Whole;
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
  control.overlap = false;
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
      return execution.execute(compiled.plan, {}, cancellation.token());
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
    auto recovered = execution.execute(compiled.plan);
    PS_CHECK(recovered.ok() &&
             multi_result::number(recovered.value().results.at("result")) == 2);
  }
  for (unsigned i = 0; i < 100; ++i) {
    auto result = execution.execute(compiled.plan);
    PS_CHECK(result.ok());
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
    check(builder.bind_descriptor_relation(
        take(ResultRelation::cartesian(phase.resources, 1, {}))));
    auto output = take(phase.resources.allocator().allocate(8));
    const double number = 5;
    std::memcpy(output.data(), &number, 8);
    check(builder.publish_tensor(
        0, Region::whole({1}), {0, {8}}, std::move(output).freeze(),
        take(ResultRelation::cartesian(phase.resources, 1, {})),
        {true, true, true, true}));
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
    auto result = exact.execute(plan.plan);
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
    auto failed = insufficient.execute(plan.plan);
    PS_CHECK(!failed.ok() &&
             failed.status().code == ErrorCode::ResourceExhausted);
  }
  return 0;
}
int graph_liveness() {
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
    config.maximum_live_bytes = 72;
    ExecutionContext execution(operations, config);
    root = take(execution.resource_budget());
    ExecutionOptions options;
    options.maximum_parallelism = 2;
    options.enable_joint = false;
    auto result = execution.execute(plan.plan, {}, {}, options);
    if (!result.ok())
      std::cerr << result.status().message << '\n';
    PS_CHECK(result.ok() && control.source.expired() && control.peak == 2);
    PS_CHECK(result.value().diagnostics.peak_active_tasks <= 2);
    PS_CHECK(root.statistics().peak[ResourceKind::Payload] == 72);
    retained = result.value().results.at("result");
    PS_CHECK(multi_result::number(retained) == 7 && retained.owned_by(root));
    control.left_entered = control.right_done = false;
    control.backed = 0;
    control.allocation_failed = false;
    auto held = execution.execute(plan.plan, {}, {}, options);
    PS_CHECK(!held.ok() && held.status().code == ErrorCode::ResourceExhausted);
    retained = {};
    result = Result<ExecutionResult>(ExecutionResult{});
    control.left_entered = control.right_done = false;
    control.backed = 0;
    control.allocation_failed = false;
    auto recovered = execution.execute(plan.plan, {}, {}, options);
    PS_CHECK(recovered.ok());
    retained = recovered.value().results.at("result");
    PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0);
  }
  PS_CHECK(multi_result::number(retained) == 7 && retained.owned_by(root));
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 8);
  retained = {};
  PS_CHECK(root.statistics().live[ResourceKind::Payload] == 0);
  control.left_entered = control.right_done = false;
  control.backed = 0;
  control.allocation_failed = false;
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.result_cache_bytes = 0;
  config.maximum_live_bytes = 71;
  ExecutionContext insufficient(operations, config);
  auto failed = insufficient.execute(plan.plan);
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
