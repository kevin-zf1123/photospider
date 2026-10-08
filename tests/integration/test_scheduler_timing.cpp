#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(PHOTOSPIDER_TEST_QUEUE_METER)
#include "execution/callback_queue_timing.hpp"
#endif
#include "photospider/photospider.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
int observe(bool enabled) {
  using namespace ps;  // NOLINT(build/namespaces)
  struct Control {
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;
    std::promise<void> entered;
  } control;
  auto entered_future = control.entered.get_future();
  auto operations = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "test.queue_timing";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.cacheable = false;
  const auto schema = multi_result::schema(ElementType::Int64, {1});
  operation.traits.input_schema[0].kind = OperationPortKind::Result;
  operation.traits.input_schema[0].result_schema_id = std::string(schema.id);
  operation.traits.input_schema[0].result_schema_version = schema.version;
  operation.traits.outputs = {multi_result::output("value", schema)};
  struct Program {
    Control* control;
    multi_result::Program identity{0};
    explicit Program(Control* control) : control(control) {}
    Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
      if (identity.requested) {
        std::int64_t value = -1;
        const auto read = phase.read_tensor(0, 0, {0}, &value, sizeof(value));
        if (!read.ok())
          return Result<ResultProgramPoll>(read);
        if (value == 0) {
          control->entered.set_value();
          std::unique_lock<std::mutex> lock(control->mutex);
          if (!control->changed.wait_for(lock, std::chrono::seconds(3),
                                         [&] { return control->released; }))
            return Result<ResultProgramPoll>(
                Status{ErrorCode::OperationFailed, "test release timeout"});
        }
      }
      return identity.poll(phase);
    }
  };
  operation.start_result = [&](const auto&, const auto& allocator) {
    return ResultContinuation::make<Program>(allocator, &control);
  };
  PS_CHECK(operations->register_operation(std::move(operation)).ok());
  PS_CHECK(operations->freeze().ok());
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "input", schema)};
  document.nodes = {{1, "test.queue_timing", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"output", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(operations).compile(graph).take_value();
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_queued_tasks = 1;
  config.collect_scheduler_timing = enabled;
  ExecutionContext context(operations, config);
  const auto input_root = multi_result::take(context.resource_budget());
  const auto run = [&](std::int64_t value) {
    return context.execute(
        plan.plan,
        {{multi_result::binding(input_root, "input", value, schema)}});
  };
  auto first = std::async(std::launch::async, [&] { return run(0); });
  const bool started = entered_future.wait_for(std::chrono::seconds(2)) ==
                       std::future_status::ready;
  auto second = std::async(std::launch::async, [&] { return run(1); });
  SchedulerStatistics pending;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  if (enabled) {
    do {
      pending = context.scheduler_statistics();
      if (pending.cpu.accepted_callbacks == 4)
        break;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
  }
  ErrorCode rejected = ErrorCode::Ok;
  if (enabled && pending.cpu.accepted_callbacks == 4)
    rejected = run(2).status().code;
  {
    std::lock_guard<std::mutex> lock(control.mutex);
    control.released = true;
  }
  control.changed.notify_all();
  const auto a = first.get(), b = second.get();
  PS_CHECK(started && a.ok() && b.ok());
  std::int64_t value = -1;
  const auto& output = b.value().results.at("output");
  PS_CHECK(output
               .read_tensor(multi_result::take(output.descriptor()), 0, {0},
                            &value, sizeof(value))
               .ok());
  PS_CHECK(value == 1);
  const auto stats = context.scheduler_statistics();
  PS_CHECK(stats.enabled == enabled && !stats.cpu.saturated &&
           !stats.gpu.saturated);
  PS_CHECK(stats.gpu.accepted_callbacks == 0 &&
           stats.gpu.started_callbacks == 0 && stats.gpu.submission_ns == 0 &&
           stats.gpu.queue_wait_ns == 0);
  if (enabled) {
    PS_CHECK(rejected == ErrorCode::ResourceExhausted);
    // The first Run has Start/Need/compute; the second Start waits in the pool.
    PS_CHECK(pending.cpu.accepted_callbacks == 4 &&
             pending.cpu.started_callbacks == 3);
    PS_CHECK(stats.cpu.accepted_callbacks == 6 &&
             stats.cpu.started_callbacks == 6);
    PS_CHECK(stats.cpu.submission_ns > 0 && stats.cpu.queue_wait_ns > 0 &&
             stats.cpu.maximum_queue_wait_ns > 0);
    PS_CHECK(stats.cpu.queue_wait_ns >= stats.cpu.maximum_queue_wait_ns);
    PS_CHECK(stats.cpu.maximum_queued_callbacks >= 1 &&
             stats.cpu.maximum_queued_callbacks <= 2);
  } else {
    PS_CHECK(stats.cpu.accepted_callbacks == 0 &&
             stats.cpu.started_callbacks == 0 && stats.cpu.submission_ns == 0 &&
             stats.cpu.queue_wait_ns == 0 &&
             stats.cpu.maximum_queue_wait_ns == 0 &&
             stats.cpu.maximum_queued_callbacks == 0);
  }
  return 0;
}
}  // namespace
int main() {
#if defined(PHOTOSPIDER_TEST_QUEUE_METER)
  // Long-lived contexts saturate observations instead of wrapping a sum.
  ps::execution_internal::CallbackQueueMeter meter(true);
  const auto begin =
      ps::execution_internal::CallbackQueueMeter::Clock::time_point{};
  const auto end = begin + std::chrono::nanoseconds(INT64_MAX);
  for (unsigned i = 0; i < 3; ++i) {
    meter.published(begin, end, 1);
    meter.started(begin, end);
  }
  const auto saturated = meter.statistics();
  PS_CHECK(saturated.saturated && saturated.accepted_callbacks == 3 &&
           saturated.started_callbacks == 3 &&
           saturated.submission_ns == UINT64_MAX &&
           saturated.queue_wait_ns == UINT64_MAX &&
           saturated.maximum_queue_wait_ns ==
               static_cast<std::uint64_t>(INT64_MAX));
#endif
  PS_CHECK(observe(false) == 0);
  PS_CHECK(observe(true) == 0);
  return 0;
}
