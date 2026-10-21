#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/execution/execution.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/status_check.hpp"

namespace {
struct Probe {
  std::mutex mutex;
  std::condition_variable changed;
  bool release_old = false, old_started = false, overlap = false;
  bool failed = false, gate = true;
  unsigned active = 0, peak = 0;
  std::vector<std::string> ended;
  std::string failure_message = std::string(200, 'D');
};
ps::OperationDefinition op(const std::string& name, unsigned inputs,
                           const std::shared_ptr<Probe>& probe,
                           bool effect = false) {
  auto definition = ps::test::fixed_fixture::operation(name, inputs, effect);
  if (name == "A" || name == "C") {
    auto lower = definition.lower;
    definition.lower = [lower](const ps::OperationLowering& query) {
      auto lowered = lower(query);
      if (!lowered.ok())
        return lowered;
      auto graph = lowered.take_value();
      graph.program.calls[0].returns = graph.outputs;
      auto valid = ps::physical::finalize_program(&graph.program);
      if (!valid.ok())
        return ps::Result<ps::OperationProgram>(valid);
      return ps::Result<ps::OperationProgram>(std::move(graph));
    };
  }
  auto prepare = definition.prepare_kernel;
  definition.prepare_kernel = [prepare, probe, name](
                                  const auto& kernel, const auto& placement,
                                  const auto& query, const auto& resources,
                                  const ps_native_gpu_service_v2* device)
      -> ps::Result<ps_prepared_kernel_v8> {
    if (query.tile_extent != 4)
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::TypeMismatch, "preparation lost Plan tile extent"});
    auto base = prepare(kernel, placement, query, resources, device);
    if (!base.ok())
      return base;
    struct State {
      ps_kernel_cpu_invoke_v8 invoke;
      std::shared_ptr<Probe> probe;
      std::string name;
    };
    auto value = base.take_value();
    value.context = new State{value.invoke_cpu, probe, name};
    value.release = [](const void* context) {
      delete static_cast<const State*>(context);
    };
    value.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                          ps_kernel_status_v8* status) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      auto& probe = *state.probe;
      {
        std::unique_lock<std::mutex> lock(probe.mutex);
        probe.peak = std::max(probe.peak, ++probe.active);
        if (state.name == "A") {
          probe.old_started = true;
          probe.changed.notify_all();
          while (
              !probe.changed.wait_for(lock, std::chrono::milliseconds(2), [&] {
                return !probe.gate || probe.release_old ||
                       invocation->cancellation.is_cancelled(
                           invocation->cancellation.context);
              })) {
          }
        }
      }
      state.invoke(invocation, status);
      {
        std::lock_guard<std::mutex> lock(probe.mutex);
        if (state.name == "D") {
          probe.overlap = probe.old_started && probe.active == 2;
          probe.release_old = true;
          if (probe.failed)
            *status = ps::kernel_failure(ps::ErrorCode::OperationFailed,
                                         probe.failure_message.c_str());
          probe.changed.notify_all();
        }
        probe.ended.push_back(state.name);
        --probe.active;
        probe.changed.notify_all();
      }
    };
    return ps::Result<ps_prepared_kernel_v8>(value);
  };
  return definition;
}
ps::WorkflowDocument document() {
  ps::WorkflowDocument result;
  result.nodes = {
      {1, "A", {}, {}},
      {2, "C", {ps::WorkflowNodeOutput{1, "out0"}}, {}},
      {3, "B", {}, {}},
      {4, "D", {ps::WorkflowNodeOutput{3, "out0"}}, {}},
      {5,
       "E",
       {ps::WorkflowNodeOutput{2, "out0"}, ps::WorkflowNodeOutput{4, "out0"}},
       {}},
      {6, "B", {}, {}}};
  result.outputs = {{"result", 5, "out0"}};
  return result;
}
int observer_queue_failure(
    const std::shared_ptr<ps::OperationRegistry>& registry,
    const ps::ExecutionPlan& plan, const std::shared_ptr<Probe>& probe,
    const ps::ExecutionContextConfig& config) {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false;
  std::atomic<unsigned> delivered{0};
  std::weak_ptr<int> retained;
  ps::ExecutionContext context(registry, config);
  auto root = context.resource_budget();
  PS_REQUIRE_OK(root);
  auto prepared = context.prepare(plan);
  PS_REQUIRE_OK(prepared);
  probe->ended.clear();
  {
    auto capture = std::make_shared<int>(17);
    retained = capture;
    ps::ExecutionOptions options;
    options.on_call_retired = [&, capture = std::move(capture)](
                                  const ps::CallRecord&) {
      ++delivered;
      {
        std::unique_lock<std::mutex> lock(probe->mutex);
        if (!probe->changed.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return probe->ended.size() == 5; }))
          throw std::runtime_error("observer blocked task drain");
      }
      std::unique_lock<std::mutex> lock(mutex);
      entered = true;
      changed.notify_all();
      if (!changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return released; }))
        throw std::runtime_error("observer release timeout");
      throw std::runtime_error("first observer");
    };
    auto submitted = context.submit(prepared.value(), {}, {}, options);
    PS_REQUIRE_OK(submitted);
    options.on_call_retired = {};
    auto run = submitted.take_value();
    auto waiting = std::async(std::launch::async, [run] { return run.join(); });
    bool started;
    {
      std::unique_lock<std::mutex> lock(mutex);
      started = changed.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return entered; });
    }
    const bool blocked = waiting.wait_for(std::chrono::milliseconds(20)) ==
                         std::future_status::timeout;
    const bool capture_alive = !retained.expired();
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    changed.notify_all();
    auto result = waiting.get();
    PS_CHECK(started && blocked && capture_alive);
    PS_CHECK(!result.ok() &&
             result.status().code == ps::ErrorCode::OperationFailed &&
             result.status().detail.origin == ps::FailureOrigin::Observer &&
             result.status().detail.node_id == 1 &&
             result.status().detail.call_sequence == 0);
    PS_CHECK(delivered == 1 && probe->active == 0 && probe->ended.size() == 5);
  }
  PS_CHECK(retained.expired());
  PS_CHECK(root.value().statistics().live[ps::ResourceKind::Payload] == 0);
  PS_CHECK(root.value().statistics().live[ps::ResourceKind::Queue] ==
           config.maximum_queued_tasks);
  return 0;
}
int stop_before_final_success() {
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(
      ps::test::fixed_fixture::operation("final", 0)));
  PS_REQUIRE_OK(registry->freeze());
  ps::WorkflowDocument document;
  document.nodes = {{1, "final", {}, {}}};
  document.outputs = {{"value", 1, "out0"}};
  ps::ExecutionContext context(registry);
  ps::PlanningOptions planning;
  planning.tile_extent = 4;
  for (unsigned mode = 0; mode < 3; ++mode) {
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph, planning);
    PS_REQUIRE_OK(compiled);
    auto prepared = context.prepare(compiled.value().plan.value());
    PS_REQUIRE_OK(prepared);
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    ps::ExecutionOptions options;
    options.on_call_retired = [&](const ps::CallRecord& record) {
      if (record.outcome != ps::CallOutcome::Succeeded)
        throw std::runtime_error("final call did not succeed");
      std::unique_lock<std::mutex> lock(mutex);
      entered = true;
      changed.notify_all();
      if (!changed.wait_for(lock, std::chrono::seconds(5),
                            [&] { return released; }))
        throw std::runtime_error("final observer timeout");
    };
    auto submitted = context.submit(prepared.value(), {}, {}, options);
    PS_REQUIRE_OK(submitted);
    auto joining = std::async(std::launch::async,
                              [&] { return submitted.value().join(); });
    bool started;
    {
      std::unique_lock<std::mutex> lock(mutex);
      started = changed.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return entered; });
    }
    const auto waiting = joining.wait_for(std::chrono::milliseconds(20));
    const bool cancelled = mode == 1 || submitted.value().request_cancel();
    if (mode)
      graph.replace(document);
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    changed.notify_all();
    auto result = joining.get();
    PS_CHECK(started && waiting == std::future_status::timeout && cancelled);
    PS_CHECK(result.status().code ==
             (mode == 1 ? ps::ErrorCode::Stale : ps::ErrorCode::Cancelled));
    auto current = ps::Compiler(registry).compile(graph, planning);
    PS_REQUIRE_OK(current);
    auto linked = context.prepare(current.value().plan.value());
    PS_REQUIRE_OK(linked);
    auto recovery = context.submit(linked.value());
    PS_REQUIRE_OK(recovery);
    auto recovered = recovery.value().join();
    PS_REQUIRE_OK(recovered);
    PS_CHECK(recovered.value().diagnostics.completed_tasks == 1);
  }
  return 0;
}
}  // namespace

int main() {
  PS_CHECK(stop_before_final_success() == 0);
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(op("A", 0, probe)));
  PS_REQUIRE_OK(registry->register_operation(op("B", 0, probe)));
  PS_REQUIRE_OK(registry->register_operation(op("C", 1, probe)));
  PS_REQUIRE_OK(registry->register_operation(op("D", 1, probe)));
  PS_REQUIRE_OK(registry->register_operation(op("E", 2, probe, true)));
  PS_REQUIRE_OK(registry->freeze());
  ps::GraphContext graph(document());
  ps::Compiler compiler(registry);
  auto ir = compiler.analyze(graph.snapshot());
  PS_REQUIRE_OK(ir);
  ps::PlanningOptions planning;
  planning.tile_extent = 4;
  auto plan = compiler.plan(ir.value(), planning);
  PS_REQUIRE_OK(plan);
  ps::ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.maximum_queued_tasks = 1;
  ps::ExecutionContext context(registry, config);
  auto prepared = context.prepare(plan.value());
  PS_REQUIRE_OK(prepared);
  const auto prepared_count = ps::test::fixed_fixture::preparations.load();
  ps::ResultRef retired_a;
  std::vector<std::uint64_t> retired;
  std::vector<ps::CallOutcome> outcomes;
  ps::ExecutionOptions options;
  options.on_call_retired = [&](const ps::CallRecord& record) {
    if (record.source_call == 1) {
      if (record.values.size() != 1)
        throw std::runtime_error("missing call export");
      retired_a = record.values.begin()->second;
      std::unique_lock<std::mutex> lock(probe->mutex);
      if (!probe->changed.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return probe->ended.size() == 5; }))
        throw std::runtime_error("observer blocked completion ingestion");
    }
    retired.push_back(record.source_call);
    outcomes.push_back(record.outcome);
  };
  auto submitted = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(submitted);
  auto run = submitted.take_value();
  auto finished = run.join();
  PS_REQUIRE_OK(finished);
  PS_CHECK(probe->overlap && probe->peak == 2);
  PS_CHECK(retired == std::vector<std::uint64_t>({1, 2, 3, 4, 5, 6}));
  PS_CHECK(outcomes.back() == ps::CallOutcome::Elided);
  PS_CHECK(ps::test::fixed_fixture::preparations == prepared_count);
  PS_CHECK(finished.value().diagnostics.completed_tasks == 5);
  auto value = finished.value().results.at("result");
  auto descriptor = value.descriptor();
  PS_REQUIRE_OK(descriptor);
  for (unsigned i = 0; i < 4; ++i) {
    std::uint8_t byte = 0;
    PS_REQUIRE_OK(value.read_tensor(descriptor.value(), 0, {i}, &byte, 1));
    PS_CHECK(byte == 5);
  }
  auto retired_descriptor = retired_a.descriptor();
  PS_REQUIRE_OK(retired_descriptor);
  std::uint8_t retired_byte = 0;
  PS_REQUIRE_OK(retired_a.read_tensor(retired_descriptor.value(), 0, {0},
                                      &retired_byte, 1));
  PS_CHECK(retired_byte == 1);
  PS_CHECK(!run.request_cancel());
  PS_REQUIRE_OK(run.join());

  // The same independent CPU branches respect a positive per-Run lane bound.
  probe->gate = false;
  probe->peak = 0;
  probe->ended.clear();
  ps::ExecutionOptions serial_options;
  serial_options.maximum_parallelism = 1;
  auto serial = context.submit(prepared.value(), {}, {}, serial_options);
  PS_REQUIRE_OK(serial);
  auto serial_result = serial.value().join();
  PS_REQUIRE_OK(serial_result);
  PS_CHECK(serial_result.value().diagnostics.completed_tasks == 5);
  PS_CHECK(probe->peak == 1 && probe->active == 0 && probe->ended.size() == 5);
  auto serial_value = serial_result.value().results.at("result");
  auto serial_descriptor = serial_value.descriptor();
  PS_REQUIRE_OK(serial_descriptor);
  for (unsigned i = 0; i < 4; ++i) {
    std::uint8_t byte = 0;
    PS_REQUIRE_OK(
        serial_value.read_tensor(serial_descriptor.value(), 0, {i}, &byte, 1));
    PS_CHECK(byte == 5);
  }

  PS_CHECK(observer_queue_failure(registry, plan.value(), probe, config) == 0);

  // The final observer failure is settled before final RET can become success.
  options.on_call_retired = [](const ps::CallRecord& record) {
    if (record.source_call == 6)
      throw std::runtime_error("last observer");
  };
  auto rejected = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(rejected);
  auto rejected_result = rejected.value().join();
  PS_CHECK(!rejected_result.ok());
  PS_CHECK(rejected_result.status().code == ps::ErrorCode::OperationFailed);
  PS_CHECK(rejected_result.status().reason == ps::FailureReason::HostException);
  PS_CHECK(rejected_result.status().detail.origin ==
           ps::FailureOrigin::Observer);
  PS_CHECK(rejected_result.status().detail.call_sequence == 5);
  PS_CHECK(rejected_result.status().detail.node_id == 6);

  // Task failure retires the older prefix and the failed call, suppressing E.
  probe->failed = true;
  retired.clear();
  outcomes.clear();
  options.on_call_retired = [&](const ps::CallRecord& record) {
    retired.push_back(record.source_call);
    outcomes.push_back(record.outcome);
  };
  auto failed = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(failed);
  auto failed_result = failed.value().join();
  PS_CHECK(!failed_result.ok());
  PS_CHECK(failed_result.status().code == ps::ErrorCode::OperationFailed);
  PS_CHECK(failed_result.status().message == probe->failure_message);
  PS_CHECK(failed_result.status().detail.origin == ps::FailureOrigin::Backend);
  PS_CHECK(failed_result.status().detail.node_id == 4);
  PS_CHECK(failed_result.status().detail.task_id.has_value());
  PS_CHECK(retired == std::vector<std::uint64_t>({1, 2, 3, 4}));
  PS_CHECK(outcomes.back() == ps::CallOutcome::Failed);

  // Failed-call delivery can itself abort after the task failure is settled.
  // Keep that callback blocked until the coordinator has consumed all tasks.
  for (bool cancel_observer : {false, true}) {
    std::mutex observer_mutex;
    std::condition_variable observer_changed;
    bool entered = false, release = false;
    options.on_call_retired = [&](const ps::CallRecord& record) {
      if (record.outcome != ps::CallOutcome::Failed)
        return;
      std::unique_lock<std::mutex> lock(observer_mutex);
      entered = true;
      observer_changed.notify_all();
      if (!observer_changed.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return release; }))
        throw std::runtime_error("failed observer release timeout");
      throw std::runtime_error("failed call observer");
    };
    auto observed_failure = context.submit(prepared.value(), {}, {}, options);
    PS_REQUIRE_OK(observed_failure);
    bool observed;
    {
      std::unique_lock<std::mutex> lock(observer_mutex);
      observed = observer_changed.wait_for(lock, std::chrono::seconds(5),
                                           [&] { return entered; });
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool drained = false;
    do {
      const auto snapshot = observed_failure.value().observation().snapshot();
      drained = snapshot.retired_calls == 4 &&
                snapshot.completed_tasks == snapshot.submitted_tasks;
      if (!drained)
        std::this_thread::yield();
    } while (!drained && std::chrono::steady_clock::now() < deadline);
    if (cancel_observer)
      observed_failure.value().request_cancel();
    {
      std::lock_guard<std::mutex> lock(observer_mutex);
      release = true;
    }
    observer_changed.notify_all();
    auto terminal_failure = observed_failure.value().join();
    PS_CHECK(observed && drained && !terminal_failure.ok());
    if (cancel_observer) {
      PS_CHECK(terminal_failure.status().code == ps::ErrorCode::Cancelled);
    } else {
      PS_CHECK(terminal_failure.status().code ==
               ps::ErrorCode::OperationFailed);
      PS_CHECK(terminal_failure.status().reason ==
               ps::FailureReason::HostException);
      PS_CHECK(terminal_failure.status().detail.origin ==
               ps::FailureOrigin::Observer);
      PS_CHECK(terminal_failure.status().detail.node_id == 4);
    }
  }

  // Cancellation of an accepted, blocked kernel waits for its real return.
  probe->gate = true;
  probe->old_started = false;
  probe->release_old = false;
  probe->failed = false;
  ps::WorkflowDocument single;
  single.nodes = {{1, "A", {}, {}}};
  single.outputs = {{"result", 1, "out0"}};
  ps::GraphContext single_graph(single);
  auto single_ir = compiler.analyze(single_graph.snapshot());
  PS_REQUIRE_OK(single_ir);
  auto single_plan = compiler.plan(single_ir.value(), planning);
  PS_REQUIRE_OK(single_plan);
  auto single_prepared = context.prepare(single_plan.value());
  PS_REQUIRE_OK(single_prepared);
  auto active = context.submit(single_prepared.value());
  PS_REQUIRE_OK(active);
  bool started;
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    started = probe->changed.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return probe->old_started; });
  }
  active.value().request_cancel();
  auto active_result = active.value().join();
  PS_CHECK(started);
  PS_CHECK(active_result.status().code == ps::ErrorCode::Cancelled);
  PS_CHECK(probe->active == 0);

  // Context shutdown cancels and drains accepted work even with live handles.
  probe->old_started = false;
  auto transient = std::make_unique<ps::ExecutionContext>(registry, config);
  auto transient_prepared = transient->prepare(single_plan.value());
  PS_REQUIRE_OK(transient_prepared);
  auto transient_run = transient->submit(transient_prepared.value());
  PS_REQUIRE_OK(transient_run);
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    started = probe->changed.wait_for(lock, std::chrono::seconds(5),
                                      [&] { return probe->old_started; });
  }
  transient.reset();
  PS_CHECK(started);
  PS_CHECK(transient_run.value().join().status().code ==
           ps::ErrorCode::Cancelled);
  PS_CHECK(probe->active == 0);

  // A successful named Result owns its payload beyond context destruction.
  probe->gate = false;
  ps::ResultRef escaped;
  {
    ps::ExecutionContext temporary(registry, config);
    auto local_prepared = temporary.prepare(plan.value());
    PS_REQUIRE_OK(local_prepared);
    auto local_run = temporary.submit(local_prepared.value());
    PS_REQUIRE_OK(local_run);
    auto local_result = local_run.value().join();
    PS_REQUIRE_OK(local_result);
    escaped = local_result.value().results.at("result");
  }
  auto escaped_descriptor = escaped.descriptor();
  PS_REQUIRE_OK(escaped_descriptor);
  std::uint8_t escaped_byte = 0;
  PS_REQUIRE_OK(escaped.read_tensor(escaped_descriptor.value(), 0, {0},
                                    &escaped_byte, 1));
  PS_CHECK(escaped_byte == 5);

  ps::CancellationSource cancellation;
  cancellation.cancel();
  auto cancelled = context.submit(prepared.value(), {}, cancellation.token());
  PS_REQUIRE_OK(cancelled);
  PS_CHECK(cancelled.value().join().status().code == ps::ErrorCode::Cancelled);
  return 0;
}
