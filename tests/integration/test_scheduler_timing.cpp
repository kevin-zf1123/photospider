#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "execution/callback_queue_timing.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
int observe(bool enabled) {
  using namespace ps;  // NOLINT(build/namespaces)
  std::mutex mutex;
  std::condition_variable changed;
  bool released = false;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  auto operations = std::make_shared<OperationRegistry>();
  OperationDefinition operation;
  operation.key = "test.queue_timing";
  operation.traits.input_count = 1;
  operation.traits.input_schema.resize(1);
  operation.traits.cacheable = false;
  operation.traits.outputs[0].output_element_type = ElementType::Int64;
  operation.traits.outputs[0].shape_rule =
      OperationShapeRule::PreserveFirstInput;
  operation.callback = [&](const OperationInvocation& call) -> Result<Value> {
    std::int64_t value = 0;
    std::memcpy(&value, call.inputs[0].bytes().data(), sizeof(value));
    if (value == 0) {
      entered.set_value();
      std::unique_lock<std::mutex> lock(mutex);
      if (!changed.wait_for(lock, std::chrono::seconds(3),
                            [&] { return released; }))
        return Result<Value>(
            Status{ErrorCode::OperationFailed, "test release timeout"});
    }
    return Result<Value>(call.inputs[0]);
  };
  PS_CHECK(operations->register_operation(std::move(operation)).ok());
  PS_CHECK(operations->freeze().ok());
  WorkflowDocument document;
  document.inputs = {{1,
                      "input",
                      {ElementType::Int64, {1}},
                      Region::whole({1}),
                      {0, {8}},
                      {}}};
  document.nodes = {{1, "test.queue_timing", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"output", 1, "value"}};
  GraphContext graph(document);
  auto plan = Compiler(operations).compile(graph).take_value();
  ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.maximum_queued_tasks = 1;
  config.collect_scheduler_timing = enabled;
  ExecutionContext context(operations, config);
  const auto run = [&](std::int64_t value) {
    auto bytes = MutableValue::allocate({ElementType::Int64, {1}},
                                        Region::whole({1}), BufferAllocator())
                     .take_value();
    std::memcpy(bytes.data(), &value, sizeof(value));
    return context.execute(
        plan.plan, {{{"input", std::move(bytes).publish().take_value()}}});
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
      if (pending.cpu.accepted_callbacks == 2)
        break;
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
  }
  ErrorCode rejected = ErrorCode::Ok;
  if (enabled && pending.cpu.accepted_callbacks == 2)
    rejected = run(2).status().code;
  {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
  }
  changed.notify_all();
  const auto a = first.get(), b = second.get();
  PS_CHECK(started && a.ok() && b.ok());
  std::int64_t value = -1;
  std::memcpy(&value, b.value().values.at("output").bytes().data(),
              sizeof(value));
  PS_CHECK(value == 1);
  const auto stats = context.scheduler_statistics();
  PS_CHECK(stats.enabled == enabled && !stats.cpu.saturated &&
           !stats.gpu.saturated);
  PS_CHECK(stats.gpu.accepted_callbacks == 0 &&
           stats.gpu.started_callbacks == 0 && stats.gpu.submission_ns == 0 &&
           stats.gpu.queue_wait_ns == 0);
  if (enabled) {
    PS_CHECK(rejected == ErrorCode::ResourceExhausted);
    PS_CHECK(pending.cpu.accepted_callbacks == 2 &&
             pending.cpu.started_callbacks == 1);
    PS_CHECK(stats.cpu.accepted_callbacks == 2 &&
             stats.cpu.started_callbacks == 2);
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
  PS_CHECK(observe(false) == 0);
  PS_CHECK(observe(true) == 0);
  return 0;
}
