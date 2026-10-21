#include <algorithm>
#include <array>
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
  struct Pending {
    const ps_kernel_invocation_v8* invocation;
    const ps_kernel_completion_v8* completion;
  };
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Pending> pending;
  unsigned active = 0, peak = 0, inline_calls = 0, cpu_calls = 0, releases = 0;
  ps::ErrorCode rejection = ps::ErrorCode::Ok;
  bool malformed_accept = false;
  Probe() { pending.reserve(8); }
  Pending next() {
    std::unique_lock<std::mutex> lock(mutex);
    if (!changed.wait_for(lock, std::chrono::seconds(5),
                          [&] { return !pending.empty(); }))
      throw std::runtime_error("asynchronous task did not submit");
    auto item = pending.front();
    pending.erase(pending.begin());
    return item;
  }
};
void write(const ps_kernel_invocation_v8* invocation) {
  auto* output = static_cast<std::uint8_t*>(invocation->outputs[0].allocation);
  const auto* input =
      invocation->input_count
          ? static_cast<const std::uint8_t*>(invocation->inputs[0].allocation)
          : nullptr;
  for (unsigned i = 0; i < 4; ++i)
    output[i] = (input ? input[i] : 0) + 1;
  auto* scratch = static_cast<std::uint8_t*>(invocation->scratch[0].allocation);
  scratch[15] = 42;
}
void finish(Probe* probe, Probe::Pending pending,
            ps::ErrorCode code = ps::ErrorCode::Ok) {
  if (code == ps::ErrorCode::Ok)
    write(pending.invocation);
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    --probe->active;
  }
  const auto status = ps::kernel_failure(code);
  if (pending.completion->report_timing) {
    // Synthetic clock data exercises transport only, not device performance.
    const ps_kernel_device_timing_v8 timing{
        PS_KERNEL_CLOCK_VULKAN_TICKS_V8, 40, 9, 100, 150, 2.0};
    pending.completion->report_timing(pending.completion->context,
                                      pending.completion->key, &timing);
  }
  // Neither borrowed pointer may be touched after finish returns.
  pending.completion->finish(pending.completion->context,
                             pending.completion->key, &status);
}
ps::OperationDefinition operation(const std::string& name, bool immediate,
                                  const std::shared_ptr<Probe>& probe) {
  auto definition = ps::test::fixed_fixture::operation(name, immediate ? 1 : 0);
  definition.traits.supports_gpu = true;
  definition.lower = [](const ps::OperationLowering& query) {
    ps::OperationProgramBuilder builder(query);
    if (!query.requests[0].empty()) {
      ps::physical::Placement placement;
      placement.backend = ps::physical::Backend::Gpu;
      placement.profile = "controlled-async-test";
      auto task =
          builder.add_task("increment", "async-test-v1", placement, {16, 0, 0});
      if (!task.ok())
        return ps::Result<ps::OperationProgram>(task.status());
      if (!query.inputs.empty()) {
        auto status = builder.read_input(task.value(), 0, 0, 0);
        if (!status.ok())
          return ps::Result<ps::OperationProgram>(status);
      }
      auto status = builder.write_output(task.value(), 0, 0, 0);
      if (!status.ok())
        return ps::Result<ps::OperationProgram>(status);
    }
    return builder.finish();
  };
  definition.prepare_kernel = [probe, immediate](
                                  const auto& kernel, const auto& placement,
                                  const auto& query, const ps::ResourceSet&,
                                  const ps_native_gpu_service_v2*)
      -> ps::Result<ps_prepared_kernel_v8> {
    if (kernel.symbol != "increment" || kernel.contract != "async-test-v1" ||
        placement.backend != ps::physical::Backend::Gpu ||
        placement.domain != ps::physical::Domain::Host ||
        query.reads.size() != (immediate ? 1U : 0U) ||
        query.writes.size() != 1 ||
        query.scratch != std::array<std::uint64_t, 3>{16, 0, 0})
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::InvalidArgument,
           "invalid asynchronous test signature"});
    const auto valid = [](const auto& view) {
      return view.domain == ps::physical::Domain::Host && view.bytes == 4 &&
             view.layout.byte_offset == 0 && view.layout.rank == 1 &&
             view.layout.element_type == ps::ElementType::UInt8 &&
             view.layout.shape[0] == 4 && view.layout.origin[0] == 0 &&
             view.layout.strides[0] == 1;
    };
    if (!valid(query.writes[0]) || (immediate && !valid(query.reads[0])))
      return ps::Result<ps_prepared_kernel_v8>(
          {ps::ErrorCode::InvalidArgument, "invalid asynchronous test views"});
    struct State {
      std::shared_ptr<Probe> probe;
      bool immediate;
    };
    ps_prepared_kernel_v8 result{};
    result.struct_size = sizeof(result);
    result.abi_version = 8;
    result.context = new State{probe, immediate};
    result.release = [](const void* raw) {
      const auto* state = static_cast<const State*>(raw);
      {
        std::lock_guard<std::mutex> lock(state->probe->mutex);
        ++state->probe->releases;
        state->probe->changed.notify_all();
      }
      delete state;
    };
    result.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                           ps_kernel_status_v8* status) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      {
        std::lock_guard<std::mutex> lock(state.probe->mutex);
        ++state.probe->cpu_calls;
      }
      write(invocation);
      *status = {};
    };
    result.submit_gpu = [](const ps_kernel_invocation_v8* invocation,
                           const ps_kernel_completion_v8* completion) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      auto& probe = *state.probe;
      ps_kernel_submission_v8 result{};
      {
        std::lock_guard<std::mutex> lock(probe.mutex);
        if (probe.rejection != ps::ErrorCode::Ok) {
          result.status = ps::kernel_failure(probe.rejection, "rejected");
          return result;
        }
        result.accepted = 1;
        result.ticket = 900;  // Backend tickets need not equal SubmissionId.
        if (probe.malformed_accept)
          result.status = ps::kernel_failure(ps::ErrorCode::OperationFailed);
        if (!state.immediate) {
          probe.peak = std::max(probe.peak, ++probe.active);
          probe.pending.push_back({invocation, completion});
          probe.changed.notify_all();
          return result;
        }
        ++probe.inline_calls;
      }
      write(invocation);
      const ps_kernel_status_v8 status{};
      completion->finish(completion->context, completion->key, &status);
      return result;
    };
    return ps::Result<ps_prepared_kernel_v8>(result);
  };
  return definition;
}
int samples(const ps::ExecutionResult& result) {
  auto value = result.results.at("value");
  auto descriptor = value.descriptor();
  PS_REQUIRE_OK(descriptor);
  for (unsigned i = 0; i < 4; ++i) {
    std::uint8_t byte = 0;
    PS_REQUIRE_OK(value.read_tensor(descriptor.value(), 0, {i}, &byte, 1));
    PS_CHECK(byte == 2);
  }
  PS_CHECK(result.diagnostics.completed_tasks == 2);
  return 0;
}
}  // namespace

int exercise(const std::shared_ptr<Probe>& probe) {
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation("async", false, probe)));
  PS_REQUIRE_OK(registry->register_operation(operation("inline", true, probe)));
  PS_REQUIRE_OK(registry->freeze());
  ps::WorkflowDocument document;
  document.nodes = {{1, "async", {}, {}},
                    {2, "inline", {ps::WorkflowNodeOutput{1, "out0"}}, {}}};
  document.outputs = {{"value", 2, "out0"}};
  ps::GraphContext graph(document);
  ps::PlanningOptions planning;
  planning.tile_extent = 4;
  planning.execution_mode = ps::ExecutionMode::NativeGpu;
  auto compiled = ps::Compiler(registry).compile(graph, planning);
  PS_REQUIRE_OK(compiled);
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_gpu_submissions = 1;
  auto context = std::make_unique<ps::ExecutionContext>(registry, config);
  auto prepared = context->prepare(compiled.value().plan.value());
  PS_REQUIRE_OK(prepared);
  // One context credit bounds two concurrent Runs; inline completion is legal.
  ps::ExecutionOptions trace;
  trace.trace_capacity = 2;
  auto first = context->submit(prepared.value(), {}, {}, trace);
  auto second = context->submit(prepared.value());
  PS_REQUIRE_OK(first);
  PS_REQUIRE_OK(second);
  // Both coordinators must have attempted admission while the sole credit is
  // held. The waiting Run precedes the holder's newly ready inline successor.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool waiting = false;
  while (
      !(waiting = first.value().observation().snapshot().coordinator_waits &&
                  second.value().observation().snapshot().coordinator_waits) &&
      std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  finish(probe.get(), probe->next());
  auto next = probe->next();
  bool fair = false;
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    fair = probe->inline_calls == 0;
  }
  finish(probe.get(), next);
  auto a = first.value().join(), b = second.value().join();
  PS_REQUIRE_OK(a);
  PS_REQUIRE_OK(b);
  PS_CHECK(waiting && fair);
  PS_CHECK(samples(a.value()) == 0 && samples(b.value()) == 0);
  PS_CHECK(probe->peak == 1 && probe->inline_calls == 2);
  for (const auto* result : {&a.value(), &b.value()}) {
    const auto& gpu = result->diagnostics.backends[1];
    PS_CHECK(gpu.submitted == 2 && gpu.completed == 2 && gpu.rejected == 0 &&
             gpu.peak_inflight == 1 && gpu.inflight == 0 &&
             result->diagnostics.backends[0].submitted == 0);
  }
  std::array<ps::ExecutionTaskTiming, 2> timings;
  PS_CHECK(first.value().observation().copy_task_timings(timings.data(), 2) ==
           2);
  PS_CHECK(timings[0].device.clock_instance == 9 &&
           timings[0].device.begin_tick == 100 &&
           timings[0].device.end_tick == 150 &&
           timings[0].device.tick_period_ns == 2.0);
  PS_CHECK(timings[0].cpu_start_ns == 0 &&
           timings[0].submit_ns <= timings[0].completion_ns &&
           timings[0].completion_ns <= timings[0].consumed_ns &&
           timings[0].consumed_ns <= timings[1].ready_ns);
  PS_CHECK(timings[1].device.clock_domain == PS_KERNEL_CLOCK_UNAVAILABLE_V8);
  PS_CHECK(second.value().observation().copy_task_timings(timings.data(), 2) ==
           0);

  // Cancel stops younger work and waits for accepted borrowing to end.
  auto cancelled = context->submit(prepared.value(), {}, {}, trace);
  PS_REQUIRE_OK(cancelled);
  auto pending = probe->next();
  const auto inline_before = probe->inline_calls;
  cancelled.value().request_cancel();
  auto joining =
      std::async(std::launch::async, [&] { return cancelled.value().join(); });
  PS_CHECK(joining.wait_for(std::chrono::milliseconds(20)) ==
           std::future_status::timeout);
  PS_CHECK(probe->releases == 0);
  auto draining = cancelled.value().observation().snapshot();
  const auto drain_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!draining.abort_drain &&
         std::chrono::steady_clock::now() < drain_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    draining = cancelled.value().observation().snapshot();
  }
  finish(probe.get(), pending);
  PS_CHECK(joining.get().status().code == ps::ErrorCode::Cancelled);
  PS_CHECK(probe->inline_calls == inline_before);
  PS_CHECK(cancelled.value().observation().copy_task_timings(timings.data(),
                                                             2) == 1);
  PS_CHECK(cancelled.value().observation().snapshot().call_timing_count == 0);
  PS_CHECK(timings[0].device.clock_instance == 9 &&
           timings[0].completion_ns <= timings[0].consumed_ns);
  const auto drained = cancelled.value().observation().snapshot();
  PS_CHECK(draining.abort_drain && !draining.abort_drain->finished &&
           draining.abort_drain->inflight_at_abort == 1 &&
           draining.backends[1].inflight == 1);
  PS_CHECK(
      drained.abort_drain && drained.abort_drain->finished &&
      drained.abort_drain->completed_ns > drained.abort_drain->started_ns &&
      drained.backends[1].inflight == 0 && drained.backends[1].completed == 1);

  for (auto code :
       {ps::ErrorCode::OperationFailed, ps::ErrorCode::BackendUnavailable}) {
    probe->rejection = code;
    auto rejected = context->submit(prepared.value(), {}, {}, trace);
    PS_REQUIRE_OK(rejected);
    auto rejected_result = rejected.value().join();
    PS_CHECK(rejected_result.status().code == code &&
             rejected_result.status().message == "rejected");
    PS_CHECK(probe->cpu_calls == 0 && probe->inline_calls == inline_before);
    PS_CHECK(rejected.value().observation().copy_task_timings(timings.data(),
                                                              2) == 1);
    PS_CHECK(timings[0].status == code && timings[0].cpu_start_ns == 0 &&
             timings[0].device.clock_domain == PS_KERNEL_CLOCK_UNAVAILABLE_V8 &&
             timings[0].submit_ns <= timings[0].completion_ns &&
             timings[0].completion_ns <= timings[0].consumed_ns);
    const auto gpu = rejected.value().observation().snapshot().backends[1];
    PS_CHECK(gpu.submitted == 1 && gpu.rejected == 1 && gpu.completed == 1 &&
             gpu.inflight == 0 && gpu.peak_inflight == 1);
  }
  probe->rejection = ps::ErrorCode::Ok;

  // Device failure completes the accepted task and stops its dependent.
  auto failed = context->submit(prepared.value());
  PS_REQUIRE_OK(failed);
  finish(probe.get(), probe->next(), ps::ErrorCode::BackendUnavailable);
  PS_CHECK(failed.value().join().status().code ==
           ps::ErrorCode::BackendUnavailable);
  PS_CHECK(probe->cpu_calls == 0 && probe->inline_calls == inline_before);

  {
    ps::WorkflowDocument independent;
    independent.nodes = {{1, "async", {}, {}},
                         {2, "async", {}, {}},
                         {3, "async", {}, {}}};
    independent.outputs = {{"a", 1, "out0"},
                           {"b", 2, "out0"},
                           {"c", 3, "out0"}};
    ps::GraphContext independent_graph(independent);
    auto independent_plan =
        ps::Compiler(registry).compile(independent_graph, planning);
    PS_REQUIRE_OK(independent_plan);
    auto parallel_config = config;
    parallel_config.maximum_gpu_submissions = 2;
    ps::ExecutionContext parallel(registry, parallel_config);
    auto linked = parallel.prepare(independent_plan.value().plan.value());
    PS_REQUIRE_OK(linked);
    auto run = parallel.submit(linked.value());
    PS_REQUIRE_OK(run);
    auto broken = probe->next();
    auto draining = probe->next();
    finish(probe.get(), broken, ps::ErrorCode::BackendUnavailable);
    auto joined =
        std::async(std::launch::async, [&] { return run.value().join(); });
    PS_CHECK(joined.wait_for(std::chrono::milliseconds(20)) ==
             std::future_status::timeout);
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      PS_CHECK(probe->pending.empty() && probe->active == 1 &&
               probe->cpu_calls == 0 && probe->releases == 0);
    }
    finish(probe.get(), draining);
    PS_CHECK(joined.get().status().code == ps::ErrorCode::BackendUnavailable);
  }

  // A malformed accepted response cannot release owners before completion.
  probe->malformed_accept = true;
  auto malformed = context->submit(prepared.value());
  PS_REQUIRE_OK(malformed);
  pending = probe->next();
  auto malformed_join =
      std::async(std::launch::async, [&] { return malformed.value().join(); });
  PS_CHECK(malformed_join.wait_for(std::chrono::milliseconds(20)) ==
           std::future_status::timeout);
  finish(probe.get(), pending);
  PS_CHECK(malformed_join.get().status().code == ps::ErrorCode::Internal);
  probe->malformed_accept = false;

  // Foreign completion keys must not settle an armed invocation.
  auto foreign = context->submit(prepared.value());
  PS_REQUIRE_OK(foreign);
  pending = probe->next();
  auto bad_key = pending.completion->key;
  ++bad_key.activation;
  const ps_kernel_status_v8 ok{};
  pending.completion->finish(pending.completion->context, bad_key, &ok);
  auto foreign_join =
      std::async(std::launch::async, [&] { return foreign.value().join(); });
  PS_CHECK(foreign_join.wait_for(std::chrono::milliseconds(20)) ==
           std::future_status::timeout);
  finish(probe.get(), pending);
  PS_CHECK(foreign_join.get().status().code == ps::ErrorCode::Internal);

  // A timing event cannot discharge borrowing. Foreign/duplicate records
  // abort only after the original submission reaches terminal completion.
  for (bool duplicate : {false, true}) {
    auto timed = context->submit(prepared.value(), {}, {}, trace);
    PS_REQUIRE_OK(timed);
    pending = probe->next();
    auto key = pending.completion->key;
    if (!duplicate)
      ++key.activation;
    const ps_kernel_device_timing_v8 timing{
        PS_KERNEL_CLOCK_VULKAN_TICKS_V8, 40, 9, 100, 150, 2.0};
    pending.completion->report_timing(pending.completion->context, key,
                                      &timing);
    if (duplicate)
      pending.completion->report_timing(pending.completion->context, key,
                                        &timing);
    auto timed_join =
        std::async(std::launch::async, [&] { return timed.value().join(); });
    const auto waiting = timed_join.wait_for(std::chrono::milliseconds(20));
    finish(probe.get(), pending);
    PS_CHECK(waiting == std::future_status::timeout);
    PS_CHECK(timed_join.get().status().code == ps::ErrorCode::Internal);
  }

  // All failure paths return context credit for the next Run.
  auto recovery = context->submit(prepared.value());
  PS_REQUIRE_OK(recovery);
  finish(probe.get(), probe->next());
  auto recovered = recovery.value().join();
  PS_REQUIRE_OK(recovered);
  PS_CHECK(samples(recovered.value()) == 0);

  auto shutdown = context->submit(prepared.value());
  PS_REQUIRE_OK(shutdown);
  pending = probe->next();
  auto destroy = std::async(std::launch::async, [&] { context.reset(); });
  PS_CHECK(destroy.wait_for(std::chrono::milliseconds(20)) ==
           std::future_status::timeout);
  finish(probe.get(), pending);
  destroy.get();
  PS_CHECK(shutdown.value().join().status().code == ps::ErrorCode::Cancelled);
  PS_CHECK(probe->active == 0 && probe->releases == 3);
  // Eight output bytes plus one 16-byte GPU scratch lane are admitted together.
  config.maximum_live_bytes = 23;
  {
    ps::ExecutionContext small(registry, config);
    auto small_prepared = small.prepare(compiled.value().plan.value());
    PS_REQUIRE_OK(small_prepared);
    const auto before = probe->inline_calls;
    auto refused = small.submit(small_prepared.value());
    PS_REQUIRE_OK(refused);
    PS_CHECK(refused.value().join().status().code ==
             ps::ErrorCode::ResourceExhausted);
    PS_CHECK(probe->pending.empty() && probe->active == 0 &&
             probe->inline_calls == before);
  }
  config.maximum_gpu_submissions = 0;
  ps::ExecutionContext disabled(registry, config);
  PS_CHECK(disabled.prepare(compiled.value().plan.value()).status().code ==
           ps::ErrorCode::BackendUnavailable);
  PS_CHECK(samples(a.value()) == 0 && samples(b.value()) == 0);
  return 0;
}

int stopped_gpu_with_cpu_progress(bool cancel) {
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<ps::OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation("async", false, probe)));
  PS_REQUIRE_OK(registry->register_operation(
      ps::test::fixed_fixture::operation("cpu", 0)));
  PS_REQUIRE_OK(registry->freeze());
  ps::WorkflowDocument document;
  document.nodes = {{1, "async", {}, {}}};
  document.outputs = {{"value", 1, "out0"}};
  ps::GraphContext graph(document);
  ps::PlanningOptions options;
  options.tile_extent = 4;
  options.execution_mode = ps::ExecutionMode::NativeGpu;
  auto compiled = ps::Compiler(registry).compile(graph, options);
  PS_REQUIRE_OK(compiled);
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_gpu_submissions = 1;
  ps::ExecutionContext context(registry, config);
  auto prepared = context.prepare(compiled.value().plan.value());
  PS_REQUIRE_OK(prepared);
  ps::WorkflowDocument cpu_document;
  cpu_document.nodes = {{1, "cpu", {}, {}}};
  cpu_document.outputs = {{"value", 1, "out0"}};
  ps::GraphContext cpu_graph(cpu_document);
  auto cpu_options = options;
  cpu_options.execution_mode = ps::ExecutionMode::CpuExact;
  auto cpu_plan = ps::Compiler(registry).compile(cpu_graph, cpu_options);
  PS_REQUIRE_OK(cpu_plan);
  auto cpu_prepared = context.prepare(cpu_plan.value().plan.value());
  PS_REQUIRE_OK(cpu_prepared);
  auto run = context.submit(prepared.value());
  PS_REQUIRE_OK(run);
  auto pending = probe->next();
  if (cancel)
    run.value().request_cancel();
  else
    graph.replace(document);
  auto joining =
      std::async(std::launch::async, [&] { return run.value().join(); });
  const auto waiting = joining.wait_for(std::chrono::milliseconds(20));
  // One accepted GPU submission consumes all GPU credits. CPU work in the
  // same context must still finish while that submission is being drained.
  auto sentinel = std::async(std::launch::async, [&] {
    auto submitted = context.submit(cpu_prepared.value());
    return submitted.ok() ? submitted.value().join()
                          : ps::Result<ps::ExecutionResult>(submitted.status());
  });
  const auto cpu_ready = sentinel.wait_for(std::chrono::seconds(5));
  const auto still_waiting = joining.wait_for(std::chrono::milliseconds(0));
  const auto retained = probe->releases == 0;
  finish(probe.get(), pending);
  auto stopped = joining.get();
  auto cpu_result = sentinel.get();
  PS_CHECK(waiting == std::future_status::timeout &&
           still_waiting == std::future_status::timeout && retained);
  PS_CHECK(cpu_ready == std::future_status::ready);
  PS_REQUIRE_OK(cpu_result);
  const auto& cpu_output = cpu_result.value().results.at("value");
  auto cpu_facts = cpu_output.descriptor();
  PS_REQUIRE_OK(cpu_facts);
  for (unsigned i = 0; i < 4; ++i) {
    std::uint8_t value = 0;
    PS_REQUIRE_OK(cpu_output.read_tensor(cpu_facts.value(), 0, {i}, &value, 1));
    PS_CHECK(value == 1);
  }
  PS_CHECK(stopped.status().code ==
           (cancel ? ps::ErrorCode::Cancelled : ps::ErrorCode::Stale));
  if (!cancel)
    PS_CHECK(context.submit(prepared.value()).status().code ==
             ps::ErrorCode::Stale);

  // A new Plan on the same context reuses the returned GPU credit.
  auto current = ps::Compiler(registry).compile(graph, options);
  PS_REQUIRE_OK(current);
  auto linked = context.prepare(current.value().plan.value());
  PS_REQUIRE_OK(linked);
  auto recovery = context.submit(linked.value());
  PS_REQUIRE_OK(recovery);
  finish(probe.get(), probe->next());
  auto result = recovery.value().join();
  PS_REQUIRE_OK(result);
  PS_CHECK(result.value().diagnostics.completed_tasks == 1);
  const auto& output = result.value().results.at("value");
  auto descriptor = output.descriptor();
  PS_REQUIRE_OK(descriptor);
  for (unsigned i = 0; i < 4; ++i) {
    std::uint8_t byte = 0;
    PS_REQUIRE_OK(output.read_tensor(descriptor.value(), 0, {i}, &byte, 1));
    PS_CHECK(byte == 1);
  }
  PS_CHECK(probe->active == 0 && probe->cpu_calls == 0);
  return 0;
}

int fair_failure_delivery() {
  using namespace ps;  // NOLINT(build/namespaces)
  auto probe = std::make_shared<Probe>();
  auto registry = std::make_shared<OperationRegistry>();
  PS_REQUIRE_OK(registry->register_operation(operation("first", false, probe)));
  PS_REQUIRE_OK(registry->register_operation(operation("later", false, probe)));
  PS_REQUIRE_OK(registry->freeze());
  WorkflowDocument single;
  single.nodes = {{1, "first", {}, {}}};
  single.outputs = {{"value", 1, "out0"}};
  auto multiple = single;
  multiple.nodes.push_back({2, "later", {}, {}});
  multiple.outputs.push_back({"later", 2, "out0"});
  GraphContext single_graph(single), multiple_graph(multiple);
  PlanningOptions planning;
  planning.tile_extent = 4;
  planning.execution_mode = ExecutionMode::NativeGpu;
  auto one = Compiler(registry).compile(single_graph, planning);
  auto two = Compiler(registry).compile(multiple_graph, planning);
  PS_REQUIRE_OK(one);
  PS_REQUIRE_OK(two);
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_gpu_submissions = 2;
  ExecutionContext context(registry, config);
  auto one_prepared = context.prepare(one.value().plan.value());
  auto two_prepared = context.prepare(two.value().plan.value());
  PS_REQUIRE_OK(one_prepared);
  PS_REQUIRE_OK(two_prepared);
  auto held = context.submit(one_prepared.value());
  PS_REQUIRE_OK(held);
  auto held_task = probe->next();
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> observer_entered{false};
  ExecutionOptions options;
  options.on_call_retired = [&](const CallRecord&) {
    observer_entered = true;
    released.wait();
    return Status::success();
  };
  auto failing = context.submit(two_prepared.value(), {}, {}, options);
  PS_REQUIRE_OK(failing);
  auto failed_task = probe->next();
  const auto wait_for = [](auto predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    return predicate();
  };
  const bool queued_failure = wait_for([&] {
    return failing.value().observation().snapshot().coordinator_waits > 0;
  });
  auto following = context.submit(one_prepared.value());
  PS_REQUIRE_OK(following);
  const bool queued_following = wait_for([&] {
    return following.value().observation().snapshot().coordinator_waits > 0;
  });
  finish(probe.get(), failed_task, ErrorCode::OperationFailed);
  const bool delivered = wait_for([&] { return observer_entered.load(); });
  bool progressed = false;
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    progressed = probe->changed.wait_for(
        lock, std::chrono::seconds(5), [&] { return !probe->pending.empty(); });
  }
  // Always release the observer and accepted tasks before asserting. A stale
  // admission head must fail this test without leaving a blocked destructor.
  release.set_value();
  finish(probe.get(), held_task);
  finish(probe.get(), probe->next());
  auto held_result = held.value().join();
  auto failed_result = failing.value().join();
  auto following_result = following.value().join();
  PS_REQUIRE_OK(held_result);
  PS_REQUIRE_OK(following_result);
  PS_CHECK(failed_result.status().code == ErrorCode::OperationFailed);
  PS_CHECK(queued_failure && queued_following && delivered && progressed);
  return 0;
}

int ordered_abort_causes() {
  using namespace ps;  // NOLINT(build/namespaces)
  const auto wait_for = [](auto predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    return predicate();
  };
  for (bool device_first : {false, true}) {
    auto probe = std::make_shared<Probe>();
    auto registry = std::make_shared<OperationRegistry>();
    PS_REQUIRE_OK(
        registry->register_operation(operation("async", false, probe)));
    PS_REQUIRE_OK(registry->freeze());
    WorkflowDocument document;
    document.nodes = {{1, "async", {}, {}},
                      {2, "async", {}, {}},
                      {3, "async", {}, {}}};
    document.outputs = {{"a", 1, "out0"}, {"b", 2, "out0"}, {"c", 3, "out0"}};
    GraphContext graph(document);
    PlanningOptions planning;
    planning.tile_extent = 4;
    planning.execution_mode = ExecutionMode::NativeGpu;
    auto compiled = Compiler(registry).compile(graph, planning);
    PS_REQUIRE_OK(compiled);
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.maximum_gpu_submissions = 3;
    ExecutionContext context(registry, config);
    auto prepared = context.prepare(compiled.value().plan.value());
    PS_REQUIRE_OK(prepared);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> observer_entered{false};
    std::atomic<unsigned> callbacks{0};
    ExecutionOptions options;
    options.on_call_retired = [&](const CallRecord& record) {
      ++callbacks;
      if (record.source_call != 1)
        throw std::runtime_error("unexpected younger callback");
      observer_entered = true;
      released.wait();
      throw std::runtime_error("ordered observer failure");
    };
    auto submitted = context.submit(prepared.value(), {}, {}, options);
    PS_REQUIRE_OK(submitted);
    auto run = submitted.take_value();
    auto observation = run.observation();
    auto first = probe->next();
    auto broken = probe->next();
    auto held = probe->next();
    auto joining = std::async(std::launch::async, [run] { return run.join(); });
    finish(probe.get(), first);
    const bool entered = wait_for([&] { return observer_entered.load(); });
    bool first_observed = false, observer_consumed = true;
    if (device_first) {
      finish(probe.get(), broken, ErrorCode::BackendUnavailable);
      first_observed = wait_for([&] {
        const auto d = observation.snapshot();
        return d.abort_drain && d.completed_tasks == 2;
      });
      release.set_value();
      const bool delivered = wait_for(
          [&] { return observation.snapshot().observer_deliveries == 1; });
      // Delivery count follows settlement publication. A later coordinator
      // snapshot has polled that event while the last accepted task is held.
      const auto wakeups = observation.snapshot().coordinator_wakeups;
      observer_consumed =
          delivered && wait_for([&] {
            return observation.snapshot().coordinator_wakeups > wakeups;
          });
    } else {
      release.set_value();
      first_observed = wait_for(
          [&] { return observation.snapshot().abort_drain.has_value(); });
      finish(probe.get(), broken, ErrorCode::BackendUnavailable);
      observer_consumed =
          wait_for([&] { return observation.snapshot().completed_tasks == 2; });
    }
    const bool waiting = joining.wait_for(std::chrono::milliseconds(20)) ==
                         std::future_status::timeout;
    bool one_active = false;
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      one_active = probe->active == 1 && probe->pending.empty();
    }
    // Return every accepted borrowing before asserting or destroying context.
    finish(probe.get(), held);
    auto result = joining.get();
    const auto diagnostics = observation.snapshot();
    PS_CHECK(entered && first_observed && observer_consumed);
    PS_CHECK(waiting && one_active);
    PS_CHECK(!result.ok() && callbacks == 1);
    PS_CHECK(diagnostics.submitted_tasks == 3 &&
             diagnostics.completed_tasks == 3 &&
             diagnostics.retired_calls == 1);
    PS_CHECK(diagnostics.abort_drain && diagnostics.abort_drain->finished);
    PS_CHECK(diagnostics.backends[1].inflight == 0);
    if (device_first) {
      PS_CHECK(result.status().code == ErrorCode::BackendUnavailable);
      PS_CHECK(result.status().detail.origin == FailureOrigin::Backend);
      PS_CHECK(result.status().detail.node_id == 2);
    } else {
      PS_CHECK(result.status().code == ErrorCode::OperationFailed);
      PS_CHECK(result.status().reason == FailureReason::HostException);
      PS_CHECK(result.status().detail.origin == FailureOrigin::Observer);
      PS_CHECK(result.status().detail.node_id == 1);
      PS_CHECK(result.status().detail.call_sequence == 0);
    }
  }
  return 0;
}

int main() {
  PS_CHECK(ordered_abort_causes() == 0);
  PS_CHECK(fair_failure_delivery() == 0);
  PS_CHECK(stopped_gpu_with_cpu_progress(false) == 0);
  PS_CHECK(stopped_gpu_with_cpu_progress(true) == 0);
  auto probe = std::make_shared<Probe>();
  PS_CHECK(exercise(probe) == 0);
  std::unique_lock<std::mutex> lock(probe->mutex);
  PS_CHECK(probe->changed.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return probe->releases == 7; }));
  return 0;
}
