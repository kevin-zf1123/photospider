#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <utility>

#include "execution/cpu_workers.hpp"
#include "execution/diagnostics.hpp"
#include "photospider/execution/execution.hpp"
#include "support/fixed_operation_fixture.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Gates {
  std::atomic<bool> release{false}, entered{false}, delivered{false},
      delivery{false};
};
template <class Predicate>
bool wait_until(Predicate predicate) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= end)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}
std::shared_ptr<OperationRegistry> registry(
    const std::shared_ptr<Gates>& gates) {
  auto registry = std::make_shared<OperationRegistry>();
  for (unsigned kind = 0; kind < 3; ++kind) {
    const bool slow = kind != 1;
    auto op = test::fixed_fixture::operation(kind == 0   ? "slow"
                                             : kind == 1 ? "fast"
                                                         : "slow_input",
                                             kind == 2 ? 1 : 0);
    auto base = op.prepare_kernel;
    op.prepare_kernel = [base, gates, slow](
                            const auto& kernel, const auto& placement,
                            const auto& query, const auto& resources,
                            const auto* device) {
      auto raw = base(kernel, placement, query, resources, device).take_value();
      struct State {
        ps_kernel_cpu_invoke_v8 invoke;
        std::shared_ptr<Gates> gates;
        bool slow;
      };
      raw.context = new State{raw.invoke_cpu, gates, slow};
      raw.release = [](const void* pointer) {
        delete static_cast<const State*>(pointer);
      };
      raw.invoke_cpu = [](const ps_kernel_invocation_v8* call,
                          ps_kernel_status_v8* status) {
        const auto& state = *static_cast<const State*>(call->prepared_context);
        if (state.slow) {
          state.gates->entered = true;
          while (!state.gates->release &&
                 !call->cancellation.is_cancelled(call->cancellation.context))
            std::this_thread::yield();
        }
        state.invoke(call, status);
      };
      return Result<ps_prepared_kernel_v8>(raw);
    };
    if (!registry->register_operation(std::move(op)).ok())
      return {};
  }
  if (!registry
           ->register_operation(test::fixed_fixture::operation("consumer", 1))
           .ok())
    return {};
  registry->freeze();
  return registry;
}
WorkflowDocument document() {
  WorkflowDocument doc;
  doc.nodes = {{1, "slow", {}, {}}, {2, "fast", {}, {}}};
  doc.outputs = {{"a", 1, "out0"}, {"b", 2, "out0"}};
  return doc;
}
int failed_admission() {
  auto gates = std::make_shared<Gates>();
  auto operations = registry(gates);
  GraphContext graph(document());
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionObservation observation;
  ResourceBudget root;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 2;
    config.maximum_live_bytes = 7;
    ExecutionContext context(operations, config);
    root = context.resource_budget().take_value();
    auto prepared = context.prepare(*compiled.value().plan);
    PS_REQUIRE_OK(prepared);
    auto run = context.submit(prepared.value());
    PS_REQUIRE_OK(run);
    observation = run.value().observation();
    auto result = run.value().join();
    PS_CHECK(!result.ok() &&
             result.status().code == ErrorCode::ResourceExhausted);
    PS_CHECK(!gates->entered);
  }
  const auto d = observation.snapshot();
  PS_CHECK(d.activations == 1 && d.allocation_count == 0 &&
           d.submitted_tasks == 0);
  PS_CHECK(d.block_admission_failures == 1 && d.last_block_admission_failure);
  const auto& failure = *d.last_block_admission_failure;
  PS_CHECK(failure.block == 0 && failure.activation == 1 &&
           failure.code == ErrorCode::ResourceExhausted);
  PS_CHECK(failure.capacity.exhausted == ResourceKind::Payload &&
           failure.capacity.requested[ResourceKind::Payload] == 8 &&
           failure.capacity.available[ResourceKind::Payload] == 7);
  PS_CHECK(root.statistics().live[ResourceKind::Metadata] > 0);
  PS_CHECK(wait_until([&] {
    return root.statistics().allocated_metadata_bytes ==
           ResourceBudget::lease_metadata_bytes() +
               sizeof(execution_internal::DiagnosticsState) +
               sizeof(core_internal::MetadataObservation);
  }));
  observation = {};
  for (auto live : root.statistics().live.values)
    PS_CHECK(live == 0);
  return 0;
}
int metadata_admission() {
  auto gates = std::make_shared<Gates>();
  gates->release = true;
  auto operations = registry(gates);
  GraphContext graph(document());
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  std::uint64_t peak = 0;
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  {
    ExecutionContext context(operations, config);
    auto prepared = context.prepare(*compiled.value().plan);
    PS_REQUIRE_OK(prepared);
    auto run = context.submit(prepared.value());
    PS_REQUIRE_OK(run);
    PS_REQUIRE_OK(run.value().join());
    peak = context.resource_budget()
               .value()
               .statistics()
               .peak[ResourceKind::Metadata];
  }
  config.managed_resources = ResourceLimits{};
  config.managed_resources->capacity[ResourceKind::Metadata] = peak - 1;
  ExecutionContext context(operations, config);
  auto prepared = context.prepare(*compiled.value().plan);
  PS_REQUIRE_OK(prepared);
  auto run = context.submit(prepared.value());
  PS_REQUIRE_OK(run);
  auto result = run.value().join();
  PS_CHECK(!result.ok() &&
           result.status().code == ErrorCode::ResourceExhausted);
  const auto d = run.value().observation().snapshot();
  PS_CHECK(d.submitted_tasks == 0 && d.allocation_count == 2 &&
           d.memory.live_bytes_by_domain[0] == 0 &&
           d.memory.reserved_bytes_by_domain[0] == 0 &&
           d.block_admission_failures == 1 && d.last_block_admission_failure);
  const auto& failure = *d.last_block_admission_failure;
  PS_CHECK(failure.capacity.exhausted == ResourceKind::Metadata &&
           failure.capacity.requested[ResourceKind::Metadata] >
               failure.capacity.available[ResourceKind::Metadata]);
  return 0;
}
int context_metadata() {
  ResourceBudget root;
  auto operations = registry(std::make_shared<Gates>());
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  config.maximum_queued_tasks = 19;
  config.maximum_runs = 7;
  GraphContext graph(document());
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  PreparedPlan retained;
  std::uint64_t before_close = 0;
  {
    ExecutionContext context(operations, config);
    root = context.resource_budget().take_value();
    const auto bytes = root.statistics().allocated_metadata_bytes;
    PS_CHECK(bytes >=
             ResourceBudget::lease_metadata_bytes() +
                 config.maximum_queued_tasks *
                     sizeof(execution_internal::CpuWork) +
                 config.cpu_workers * sizeof(std::thread) +
                 config.maximum_runs *
                     sizeof(std::weak_ptr<execution_internal::RunState>));
    PS_CHECK(bytes <= root.statistics().live[ResourceKind::Metadata]);
    auto prepared = context.prepare(*compiled.value().plan);
    PS_REQUIRE_OK(prepared);
    retained = prepared.take_value();
    before_close = root.statistics().allocated_metadata_bytes;
  }
  // Prepared state retains the closed context and its worker arrays; only
  // the detached Run directory has been freed by context destruction.
  PS_CHECK(root.statistics().allocated_metadata_bytes ==
           before_close -
               config.maximum_runs *
                   sizeof(std::weak_ptr<execution_internal::RunState>));
  retained = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  // Destruction of a worker pool releases its arrays even while its parent
  // reservation remains alive. The pool itself is on the stack here.
  auto reserved = root.reserve(ResourceCapacity::host(8192, 8192));
  PS_REQUIRE_OK(reserved);
  auto lease = reserved.take_value();
  const auto baseline = root.statistics().allocated_metadata_bytes;
  {
    execution_internal::CpuWorkers workers(2, 19, lease);
    PS_CHECK(root.statistics().allocated_metadata_bytes >=
             baseline + 19 * sizeof(execution_internal::CpuWork) +
                 2 * sizeof(std::thread));
    workers.shutdown();
    PS_CHECK(root.statistics().allocated_metadata_bytes > baseline);
  }
  PS_CHECK(root.statistics().allocated_metadata_bytes == baseline);
  lease = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  return 0;
}
int diagnostic_metadata() {
  auto gates = std::make_shared<Gates>();
  gates->release = true;
  auto operations = registry(gates);
  GraphContext graph(document());
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  for (std::uint32_t capacity : {0U, 7U}) {
    ResourceBudget root;
    ExecutionObservation observation;
    {
      ExecutionContextConfig config;
      config.cpu_workers = 1;
      ExecutionContext context(operations, config);
      root = context.resource_budget().take_value();
      auto prepared = context.prepare(*compiled.value().plan);
      PS_REQUIRE_OK(prepared);
      ExecutionOptions options;
      options.trace_capacity = capacity;
      auto submitted = context.submit(prepared.value(), {}, {}, options);
      PS_REQUIRE_OK(submitted);
      observation = submitted.value().observation();
      PS_REQUIRE_OK(submitted.value().join());
      PS_CHECK(root.statistics().allocated_metadata_bytes <=
               root.statistics().live[ResourceKind::Metadata]);
    }
    const auto expected =
        ResourceBudget::lease_metadata_bytes() +
        sizeof(execution_internal::DiagnosticsState) +
        sizeof(core_internal::MetadataObservation) +
        capacity * (sizeof(ExecutionTaskTiming) + sizeof(ExecutionCallTiming));
    PS_CHECK(wait_until([&] {
      return root.statistics().allocated_metadata_bytes == expected;
    }));
    const auto attributed = observation.snapshot().metadata;
    PS_CHECK(attributed.allocated_bytes == expected &&
             attributed.allocated_bytes <= attributed.reserved_bytes &&
             attributed.peak_allocated_bytes > attributed.allocated_bytes);
    auto alias = observation;
    observation = {};
    PS_CHECK(root.statistics().allocated_metadata_bytes == expected);
    std::thread release([copy = std::move(alias)]() mutable { copy = {}; });
    release.join();
    PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
    PS_CHECK(root.statistics().peak_allocated_metadata_bytes > expected);
  }
  return 0;
}
int exported_lifetime() {
  auto gates = std::make_shared<Gates>();
  auto operations = registry(gates);
  auto doc = document();
  doc.nodes = {{1, "fast", {}, {}},
               {2, "consumer", {WorkflowNodeOutput{1, "out0"}}, {}},
               {3, "slow", {}, {}}};
  doc.outputs = {{"b", 2, "out0"}, {"c", 3, "out0"}};
  GraphContext graph(doc);
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionObservation observation;
  ResultRef b, c;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 2;
    ExecutionContext context(operations, config);
    auto prepared = context.prepare(*compiled.value().plan);
    PS_REQUIRE_OK(prepared);
    auto submitted = context.submit(prepared.value());
    PS_REQUIRE_OK(submitted);
    auto run = submitted.take_value();
    observation = run.observation();
    const auto progressed = wait_until([&] {
      return gates->entered && observation.snapshot().completed_tasks == 2;
    });
    auto mid = observation.snapshot();
    gates->release = true;
    auto result = run.join();
    PS_REQUIRE_OK(result);
    PS_CHECK(progressed && mid.allocation_count == 3);
    PS_CHECK(mid.memory.reserved_bytes_by_domain[0] == 12 &&
             mid.memory.live_bytes_by_domain[0] == 8);
    b = result.value().results.at("b");
    c = result.value().results.at("c");
  }
  auto retained = observation.snapshot();
  PS_CHECK(retained.memory.reserved_bytes_by_domain[0] == 8 &&
           retained.memory.live_bytes_by_domain[0] == 8);
  b = {};
  PS_CHECK(observation.snapshot().memory.live_bytes_by_domain[0] == 4);
  c = {};
  auto released = observation.snapshot();
  PS_CHECK(released.memory.reserved_bytes_by_domain[0] == 0 &&
           released.memory.live_bytes_by_domain[0] == 0);
  PS_CHECK(released.memory.peak_reserved_bytes_by_domain[0] == 12 &&
           released.memory.peak_live_bytes_by_domain[0] == 12);
  return 0;
}
int wait_and_delivery(bool cancel) {
  auto gates = std::make_shared<Gates>();
  auto operations = registry(gates);
  GraphContext graph(document());
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContextConfig config;
  config.cpu_workers = 2;
  ExecutionContext context(operations, config);
  auto prepared = context.prepare(*compiled.value().plan);
  PS_REQUIRE_OK(prepared);
  ExecutionOptions options;
  options.trace_capacity = 2;
  options.on_call_retired = [gates](const CallRecord&) {
    gates->delivered = true;
    while (!gates->delivery)
      std::this_thread::yield();
  };
  auto submitted = context.submit(prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(submitted);
  auto run = submitted.take_value();
  auto observation = run.observation();
  const bool overlap = wait_until([&] {
    const auto d = observation.snapshot();
    std::array<ExecutionTaskTiming, 2> partial;
    const auto count = observation.copy_task_timings(partial.data(), 2);
    return gates->entered && d.submitted_tasks == 2 && d.completed_tasks == 1 &&
           d.current_head_wait_ns > 0 && count == 1 &&
           partial[0].source_call == 2;
  });
  if (cancel)
    run.request_cancel();
  else
    gates->release = true;
  bool retired = true, stable = true;
  if (!cancel) {
    retired =
        wait_until([&] { return observation.snapshot().retired_calls == 2; });
    auto before = observation.snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto after = observation.snapshot();
    stable = before.retirement_head_wait_ns == after.retirement_head_wait_ns;
  }
  gates->release = true;
  gates->delivery = true;
  auto result = run.join();
  const auto d = observation.snapshot();
  run = {};
  PS_CHECK(overlap && retired && stable);
  PS_CHECK(d.allocation_count == 2 && d.block_admission_failures == 0);
  PS_CHECK(d.current_head_wait_ns == 0 && d.completed_tasks == 2);
  PS_CHECK(d.backends[0].submitted == 2 && d.backends[0].completed == 2 &&
           d.backends[0].peak_inflight == 2 && d.backends[0].inflight == 0 &&
           d.backends[0].rejected == 0 && d.backends[1].submitted == 0);
  std::array<ExecutionTaskTiming, 2> tasks;
  std::array<ExecutionCallTiming, 2> calls;
  PS_CHECK(observation.copy_task_timings(tasks.data(), tasks.size()) == 2);
  PS_CHECK(d.task_timing_count == 2 && d.dropped_task_timings == 0);
  PS_CHECK(d.coordinator_waits == d.coordinator_wakeups &&
           d.coordinator_waits > 0);
  PS_CHECK(tasks[0].source_call == 2 && tasks[1].source_call == 1);
  for (const auto& task : tasks) {
    PS_CHECK(task.ready_ns <= task.submit_ns &&
             task.submit_ns <= task.cpu_start_ns &&
             task.cpu_start_ns <= task.completion_ns &&
             task.completion_ns <= task.consumed_ns);
    PS_CHECK(task.submit_ns - task.ready_ns == task.lane_wait_ns +
                                                   task.context_wait_ns +
                                                   task.coordinator_delay_ns);
    PS_CHECK(task.backend == physical::Backend::Cpu &&
             task.device.clock_domain == PS_KERNEL_CLOCK_UNAVAILABLE_V8);
  }
  PS_CHECK(d.cpu_invoke_ns == tasks[0].completion_ns - tasks[0].cpu_start_ns +
                                  tasks[1].completion_ns -
                                  tasks[1].cpu_start_ns);
  PS_CHECK(d.backends[0].inflight_ns ==
           tasks[0].consumed_ns - tasks[0].submit_ns + tasks[1].consumed_ns -
               tasks[1].submit_ns);
  if (cancel) {
    PS_CHECK(!result.ok() && result.status().code == ErrorCode::Cancelled);
    PS_CHECK(d.unfinished_head_wait_ns > 0 && d.retirement_head_wait_ns == 0);
    PS_CHECK(observation.copy_call_timings(calls.data(), calls.size()) == 0);
    PS_CHECK(d.abort_drain && d.abort_drain->finished &&
             d.abort_drain->activation == 1 && d.abort_drain->block == 0 &&
             d.abort_drain->completed_ns >= d.abort_drain->started_ns);
  } else {
    PS_REQUIRE_OK(result);
    PS_CHECK(!d.abort_drain);
    PS_CHECK(d.observer_deliveries == 2 && d.observer_invoke_ns > 0 &&
             d.observer_queue_wait_ns > 0);
    PS_CHECK(d.retirement_head_wait_ns > 0 &&
             d.peak_retirement_head_wait_ns > 0 &&
             d.unfinished_head_wait_ns == 0);
    PS_CHECK(result.value().diagnostics.retirement_head_wait_ns ==
             d.retirement_head_wait_ns);
    PS_CHECK(observation.copy_call_timings(calls.data(), calls.size()) == 2);
    PS_CHECK(calls[0].source_call == 1 && calls[1].source_call == 2 &&
             calls[0].retired_ns <= calls[1].retired_ns &&
             tasks[1].consumed_ns <= calls[0].retired_ns);
  }
  return 0;
}
int concurrent_metadata_and_context_wait() {
  auto gates = std::make_shared<Gates>();
  auto operations = registry(gates);
  WorkflowDocument doc;
  WorkflowInputDeclaration input;
  input.id = 1;
  input.name = "input";
  input.result_schema =
      std::make_shared<const SchemaTemplate>(test::fixed_fixture::schema());
  doc.inputs = {input};
  doc.nodes = {{1, "slow_input", {WorkflowInputReference{1}}, {}}};
  doc.outputs = {{"out", 1, "out0"}};
  GraphContext graph(doc);
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  ResourceBudget root;
  std::array<ExecutionObservation, 3> observations;
  std::array<ResultRef, 3> exports;
  {
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_queued_tasks = 1;
    ExecutionContext context(operations, config);
    root = context.resource_budget().take_value();
    auto prepared = context.prepare(*compiled.value().plan);
    PS_REQUIRE_OK(prepared);
    auto copied_preparation = prepared.value();
    auto made =
        ResultBuilder::start(root, test::fixed_fixture::schema(), "input");
    PS_REQUIRE_OK(made);
    auto builder = made.take_value();
    auto buffer = root.allocator().allocate(4).take_value();
    PS_REQUIRE_OK(builder.publish_tensor(0, Region::whole({4}), {0, {1}, {0}},
                                         std::move(buffer).freeze()));
    auto source = builder.seal();
    PS_REQUIRE_OK(source);
    ExecutionBindings bindings{{{"input", source.value()}}};
    const auto baseline = root.statistics();
    ExecutionOptions options;
    options.trace_capacity = 1;
    std::array<RunHandle, 3> runs;
    bool staged = true;
    for (unsigned i = 0; i < runs.size(); ++i) {
      auto submitted =
          context.submit(copied_preparation, bindings, {}, options);
      PS_REQUIRE_OK(submitted);
      runs[i] = submitted.take_value();
      observations[i] = runs[i].observation();
      staged &= wait_until([&] {
        const auto d = observations[i].snapshot();
        return d.activations == 1 && d.current_head_wait_ns > 0 &&
               (i == 2 ? d.submitted_tasks == 0 : d.submitted_tasks == 1);
      });
    }
    const auto a = observations[0].snapshot().metadata;
    const auto b = observations[1].snapshot().metadata;
    const auto c = observations[2].snapshot().metadata;
    const auto all = root.statistics();
    const bool separate =
        a.allocated_bytes && b.allocated_bytes && c.allocated_bytes &&
        all.allocated_metadata_bytes - baseline.allocated_metadata_bytes ==
            a.allocated_bytes + b.allocated_bytes + c.allocated_bytes &&
        all.live[ResourceKind::Metadata] -
                baseline.live[ResourceKind::Metadata] ==
            a.reserved_bytes + b.reserved_bytes + c.reserved_bytes &&
        all.live[ResourceKind::Payload] ==
            baseline.live[ResourceKind::Payload] + 12;
    gates->release = true;
    for (unsigned i = 0; i < runs.size(); ++i) {
      auto result = runs[i].join();
      PS_REQUIRE_OK(result);
      exports[i] = result.value().results.at("out");
    }
    PS_CHECK(staged && separate);
    const auto queued = observations[1].snapshot();
    const auto credit = observations[2].snapshot();
    PS_CHECK(queued.cpu_queue_wait_ns > 0 && credit.context_wait_ns > 0 &&
             credit.lane_wait_ns == 0 && credit.scratch_capacity_wait_ns == 0);
  }
  PS_CHECK(wait_until([&] {
    std::uint64_t total = 0;
    for (const auto& observation : observations)
      total += observation.snapshot().metadata.allocated_bytes;
    return total == root.statistics().allocated_metadata_bytes;
  }));
  const auto before = observations[0].snapshot().metadata;
  const auto untouched = observations[1].snapshot().metadata;
  std::thread release(
      [value = std::move(exports[0])]() mutable { value = {}; });
  release.join();
  PS_CHECK(observations[0].snapshot().metadata.allocated_bytes <
           before.allocated_bytes);
  PS_CHECK(observations[1].snapshot().metadata.allocated_bytes ==
           untouched.allocated_bytes);
  exports = {};
  observations = {};
  PS_CHECK(root.statistics().allocated_metadata_bytes == 0);
  return 0;
}
int lane_wait() {
  auto gates = std::make_shared<Gates>();
  auto operations = registry(gates);
  GraphContext graph(document());
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  ExecutionContext context(operations, config);
  auto prepared = context.prepare(*compiled.value().plan);
  PS_REQUIRE_OK(prepared);
  auto submitted = context.submit(prepared.value());
  PS_REQUIRE_OK(submitted);
  auto run = submitted.take_value();
  auto observation = run.observation();
  const bool blocked = wait_until([&] {
    auto d = observation.snapshot();
    return gates->entered && d.submitted_tasks == 1 && d.coordinator_waits > 0;
  });
  gates->release = true;
  PS_REQUIRE_OK(run.join());
  const auto d = observation.snapshot();
  PS_CHECK(blocked && d.lane_wait_ns > 0 && d.context_wait_ns == 0 &&
           d.scratch_capacity_wait_ns == 0 &&
           d.ready_wait_ns ==
               d.lane_wait_ns + d.context_wait_ns + d.coordinator_delay_ns);
  return 0;
}
int bounded_trace() {
  auto gates = std::make_shared<Gates>();
  gates->release = true;
  auto operations = registry(gates);
  auto doc = document();
  doc.nodes[1] = {2, "consumer", {WorkflowNodeOutput{1, "out0"}}, {}};
  GraphContext graph(doc);
  auto compiled = Compiler(operations).compile(graph);
  PS_REQUIRE_OK(compiled);
  ExecutionContext context(operations);
  auto prepared = context.prepare(*compiled.value().plan);
  PS_REQUIRE_OK(prepared);
  for (std::uint32_t capacity : {0, 1, 2}) {
    ExecutionOptions options;
    options.trace_capacity = capacity;
    auto run = context.submit(prepared.value(), {}, {}, options);
    PS_REQUIRE_OK(run);
    PS_REQUIRE_OK(run.value().join());
    auto observed = run.value().observation();
    const auto d = observed.snapshot();
    std::array<ExecutionTaskTiming, 2> tasks;
    std::array<ExecutionCallTiming, 2> calls;
    PS_CHECK(observed.copy_task_timings(nullptr, 2) == 0);
    PS_CHECK(observed.copy_task_timings(tasks.data(), 0) == 0);
    tasks[1].task = 123456;
    PS_CHECK(observed.copy_task_timings(tasks.data(), 1) ==
             (capacity ? 1U : 0U));
    PS_CHECK(tasks[1].task == 123456);
    PS_CHECK(observed.copy_task_timings(tasks.data(), tasks.size()) ==
             capacity);
    PS_CHECK(observed.copy_call_timings(calls.data(), calls.size()) ==
             capacity);
    PS_CHECK(d.task_timing_count == capacity &&
             d.call_timing_count == capacity);
    PS_CHECK(d.dropped_task_timings == (capacity == 1 ? 1U : 0U) &&
             d.dropped_call_timings == (capacity == 1 ? 1U : 0U));
    if (capacity == 2) {
      PS_CHECK(tasks[0].source_call == 1 && tasks[1].source_call == 2);
      PS_CHECK(tasks[0].consumed_ns <= tasks[1].ready_ns &&
               tasks[1].ready_ns <= tasks[1].submit_ns);
    }
  }
  return 0;
}
}  // namespace
int main() {
  return failed_admission() || metadata_admission() || context_metadata() ||
         diagnostic_metadata() || exported_lifetime() ||
         wait_and_delivery(false) || wait_and_delivery(true) ||
         concurrent_metadata_and_context_wait() || lane_wait() ||
         bounded_trace();
}
