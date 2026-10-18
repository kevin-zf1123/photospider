#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "execution/execution_test_hooks.hpp"
#include "execution/result_subscription.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
template <class T>
T take(Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void check(Status status) {
  if (!status.ok())
    throw std::runtime_error(status.message);
}
Result<ResultProgramPoll> sample(const ResultProgramPhase& phase) {
  auto builder = take(ResultBuilder::start(phase.resources,
                                           *phase.query.output.result_schema,
                                           phase.query.semantic_key));
  const std::uint8_t value = 7;
  check(builder.publish_tensor(0, Region::whole({1}), ByteView(&value, 1)));
  return Result<ResultProgramPoll>(
      ResultPublication{take(builder.seal()), true});
}
struct WorkerProbe {
  bool enabled = false, tiles = false;
  std::atomic<unsigned> units{0};
  std::function<void()> gate;
};
struct Source {
  std::atomic<unsigned>* polls;
  std::atomic<unsigned>* live;
  WorkerProbe* probe;
  Source(std::atomic<unsigned>* count, std::atomic<unsigned>* instances,
         WorkerProbe* worker_probe)
      : polls(count), live(instances), probe(worker_probe) {
    ++*live;
  }
  ~Source() { --*live; }
  struct Work {
    const ResultProgramPhase* phase;
    WorkerProbe* probe;
  };
  static int range(void* raw, std::uint64_t begin, std::uint64_t end,
                   std::uint32_t) noexcept {
    auto& work = *static_cast<Work*>(raw);
    try {
      if (work.probe->gate)
        work.probe->gate();
      auto status = work.phase->consume_work(end - begin);
      if (!status.ok())
        return status.code == ErrorCode::Cancelled ? 2 : 1;
      work.probe->units += static_cast<unsigned>(end - begin);
      return 0;
    } catch (...) {
      return 1;
    }
  }
  static int tile(void* raw, const ps_cpu_tile_v2* tile) noexcept {
    return range(raw, tile->begin[0], tile->end[0], tile->slot);
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++*polls;
    if (!probe->enabled && probe->gate)
      probe->gate();
    if (probe->enabled) {
      Work work{&phase, probe};
      int status = 1;
      if (probe->tiles && phase.cpu_tiles) {
        const ps_cpu_tile_stage_v2 stage{sizeof(ps_cpu_tile_stage_v2),
                                         {64, 1, 1},
                                         {8, 1, 1},
                                         2};
        status =
            phase.cpu_tiles->run(phase.cpu_tiles->context, &stage, tile, &work);
      } else if (!probe->tiles && phase.cpu_parallel) {
        status = phase.cpu_parallel->run(phase.cpu_parallel->context, 64, 8, 2,
                                         range, &work);
      }
      if (status)
        return Result<ResultProgramPoll>(
            Status{ErrorCode::OperationFailed, {}});
    }
    return sample(phase);
  }
};
struct Consumer {
  std::atomic<unsigned>* polls;
  std::atomic<unsigned>* live;
  bool supplied = false;
  Consumer(std::atomic<unsigned>* count, std::atomic<unsigned>* instances)
      : polls(count), live(instances) {
    ++*live;
  }
  ~Consumer() { --*live; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++*polls;
    if (!supplied) {
      supplied = true;
      ResultProgramNeed need;
      for (std::uint32_t port : {0U, 1U})
        need.tiles.push_back(
            {port, 0, false, take(TileSet::range(phase.resources, 0, 1))});
      return Result<ResultProgramPoll>(std::move(need));
    }
    std::uint8_t left = 0, right = 0;
    check(phase.read_tensor(0, 0, {0}, &left, 1));
    check(phase.read_tensor(1, 0, {0}, &right, 1));
    const std::uint8_t value = left + right;
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    check(builder.publish_tensor(0, Region::whole({1}), ByteView(&value, 1)));
    return Result<ResultProgramPoll>(
        ResultPublication{take(builder.seal()), true});
  }
};
struct Fixture {
  std::atomic<unsigned> starts{0}, polls{0}, live{0}, consumer_starts{0},
      consumer_polls{0};
  std::shared_ptr<OperationRegistry> registry =
      std::make_shared<OperationRegistry>();
  std::unique_ptr<ExecutionContext> context;
  std::unique_ptr<GraphContext> graph;
  ExecutionPlan plan;
  ResourceBudget root;
  WorkerProbe probe;
  explicit Fixture(unsigned worker_mode = 0, bool dependency = false,
                   unsigned workers = 1) {
    probe.enabled = worker_mode != 0;
    probe.tiles = worker_mode == 2;
    SchemaTemplate schema;
    schema.id = "test.submission";
    ResultTensorSpec tensor;
    tensor.key = "sample";
    tensor.descriptor = {ElementType::UInt8, {1}};
    schema.tensors.push_back(tensor);
    OperationDefinition source;
    source.key = "source";
    source.traits.input_count = 0;
    source.traits.cpu_staged_tiles = probe.tiles;
    auto& out = source.traits.outputs[0];

    out.output_schema.result_schema_id = schema.id;
    out.output_schema.result_schema_version = 1;
    out.result_schema = schema;
    out.continuation_bytes = sizeof(Source);
    out.maximum_dependency_stages = 1;
    source.start_result = [this](const auto&, const auto& allocator) {
      ++starts;
      return ResultContinuation::make<Source>(allocator, &polls, &live, &probe);
    };
    if (dependency) {
      OperationDefinition consumer;
      consumer.key = "consumer";
      consumer.traits = source.traits;
      consumer.traits.input_count = 2;
      OperationPortConstraint input;

      input.result_schema_id = schema.id;
      input.result_schema_version = schema.version;
      consumer.traits.input_schema = {input, input};
      consumer.traits.outputs[0].continuation_bytes = sizeof(Consumer);
      consumer.traits.outputs[0].maximum_dependency_stages = 2;
      consumer.start_result = [this](const auto&, const auto& allocator) {
        ++consumer_starts;
        return ResultContinuation::make<Consumer>(allocator, &consumer_polls,
                                                  &live);
      };
      check(registry->register_operation(std::move(consumer)));
    }
    check(registry->register_operation(std::move(source)));
    check(registry->freeze());
    ExecutionContextConfig config;
    config.cpu_workers = probe.enabled ? 2 : workers;
    config.maximum_queued_tasks = 1;
    config.result_cache_bytes = 0;
    config.managed_resources = ResourceLimits{};
    context = std::make_unique<ExecutionContext>(registry, config);
    root = take(context->resource_budget());
    WorkflowDocument document;
    document.nodes = {{1, "source", {}, {}}};
    document.outputs = {{"result", 1, "value"}};
    if (dependency) {
      document.nodes.push_back(
          {2,
           "consumer",
           {WorkflowNodeOutput{1, "value"}, WorkflowNodeOutput{1, "value"}},
           {}});
      document.outputs = {{"result", 2, "value"}};
    }
    graph = std::make_unique<GraphContext>(document);
    plan = take(Compiler(registry).compile(*graph)).plan.value();
  }
};
std::atomic<unsigned> submissions{0};
unsigned selected_submission = 0;
execution_testing::CallbackSubmitAction selected_action;
execution_testing::CallbackSubmitAction submit_action(Backend) {
  return ++submissions == selected_submission
             ? selected_action
             : execution_testing::CallbackSubmitAction::Proceed;
}
int dropped_submission() {
  Fixture fixture;
  using execution_testing::CallbackSubmitAction;
  for (auto action : {CallbackSubmitAction::Drop, CallbackSubmitAction::Reject,
                      CallbackSubmitAction::ThrowBadAlloc,
                      CallbackSubmitAction::ThrowNullDiagnostic}) {
    for (unsigned phase : {1U, 2U}) {
      submissions = 0;
      selected_submission = phase;
      selected_action = action;
      const auto starts = fixture.starts.load(), polls = fixture.polls.load();
      execution_testing::ExecutionTestHooks hooks;
      hooks.callback_submit_action = submit_action;
      execution_testing::install_execution_test_hooks(&hooks);
      auto result = fixture.context->execute(fixture.plan);
      execution_testing::install_execution_test_hooks(nullptr);
      const auto expected =
          action == CallbackSubmitAction::Drop ? ErrorCode::Cancelled
          : action == CallbackSubmitAction::ThrowNullDiagnostic
              ? ErrorCode::Internal
              : ErrorCode::ResourceExhausted;
      PS_CHECK(!result.ok() && result.status().code == expected);
      if (action == CallbackSubmitAction::ThrowNullDiagnostic)
        PS_CHECK(result.status().message.empty());
      PS_CHECK(fixture.starts == starts + phase - 1 && fixture.polls == polls);
      PS_CHECK(fixture.live == 0);
      PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
               fixture.root.statistics().live[ResourceKind::Payload] == 0);
      auto recovered = fixture.context->execute(fixture.plan);
      PS_CHECK(recovered.ok());
      const auto& output = recovered.value().results.at("result");
      std::uint8_t value = 0;
      PS_CHECK(output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1)
                   .ok() &&
               value == 7);
    }
  }
  return 0;
}
std::mutex gate_mutex;
std::condition_variable gate_changed;
bool entered = false, release = false, timed_out = false;
unsigned retired_bodies = 0, selected_retirement = 0;
void hold_retirement() noexcept {
  std::unique_lock<std::mutex> lock(gate_mutex);
  if (++retired_bodies != selected_retirement)
    return;
  entered = true;
  gate_changed.notify_all();
  timed_out = !gate_changed.wait_for(lock, std::chrono::seconds(10),
                                     [] { return release; });
}
int retirement_notification() {
  Fixture fixture;
  for (unsigned phase : {1U, 2U}) {
    for (bool cancel : {false, true}) {
      selected_retirement = phase;
      const auto starts = fixture.starts.load(), polls = fixture.polls.load();
      entered = release = timed_out = false;
      retired_bodies = 0;
      execution_testing::ExecutionTestHooks hooks;
      hooks.callback_body_finished = hold_retirement;
      execution_testing::install_execution_test_hooks(&hooks);
      CancellationSource cancellation;
      std::atomic<unsigned> publications{0};
      ExecutionOptions options;
      options.result_publication = [&](ValueRef, const ResultRef&) {
        ++publications;
        return Status::success();
      };
      auto pending = std::async(std::launch::async, [&] {
        return fixture.context->execute(fixture.plan, {}, cancellation.token(),
                                        options);
      });
      bool body;
      {
        std::unique_lock<std::mutex> lock(gate_mutex);
        body = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                     [] { return entered; });
      }
      const auto before_wait = fixture.root.statistics().issued;
      if (cancel)
        cancellation.cancel();
      const bool returned = pending.wait_for(std::chrono::milliseconds(20)) ==
                            std::future_status::ready;
      const auto after_wait = fixture.root.statistics().issued;
      const auto delivered = publications.load();
      const auto live = fixture.live.load();
      const auto held_starts = fixture.starts.load();
      const auto held_polls = fixture.polls.load();
      {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
      }
      gate_changed.notify_all();
      auto result = pending.get();
      execution_testing::install_execution_test_hooks(nullptr);
      PS_CHECK(body && !timed_out && !returned && delivered == 0 && live == 1);
      PS_CHECK(held_starts == starts + 1 && held_polls == polls + phase - 1);
      PS_CHECK(before_wait.work == after_wait.work &&
               before_wait.stages == after_wait.stages);
      PS_CHECK(fixture.live == 0);
      PS_CHECK(cancel ? result.status().code == ErrorCode::Cancelled
                      : result.ok());
      PS_CHECK(publications == (cancel ? 0 : 1));
      result = Result<ExecutionResult>(ExecutionResult{});
      PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
               fixture.root.statistics().live[ResourceKind::Payload] == 0);
    }
  }
  return 0;
}
int cancelled_subscription() {
  Fixture fixture(0, false, 2);
  auto frozen = take(fixture.context->freeze(fixture.plan, {}));
  std::mutex mutex;
  std::condition_variable changed;
  unsigned entered_runs = 0;
  bool released_runs = false, gate_timeout = false;
  fixture.probe.gate = [&] {
    std::unique_lock<std::mutex> lock(mutex);
    ++entered_runs;
    changed.notify_all();
    if (!changed.wait_for(lock, std::chrono::seconds(10),
                          [&] { return released_runs; }))
      gate_timeout = true;
  };
  CancellationSource cancellation;
  std::atomic<unsigned> cancelled_publications{0}, peer_publications{0};
  ExecutionOptions first_options, peer_options;
  first_options.result_publication = [&](ValueRef, const ResultRef&) {
    ++cancelled_publications;
    return Status::success();
  };
  peer_options.result_publication = [&](ValueRef, const ResultRef&) {
    ++peer_publications;
    return Status::success();
  };
  auto first = std::async(std::launch::async, [&] {
    return fixture.context->execute(frozen, cancellation.token(),
                                    first_options);
  });
  bool first_entered;
  {
    std::unique_lock<std::mutex> lock(mutex);
    first_entered = changed.wait_for(lock, std::chrono::seconds(5),
                                     [&] { return entered_runs == 1; });
  }
  auto peer = std::async(std::launch::async, [&] {
    return fixture.context->execute(frozen, {}, peer_options);
  });
  bool both_entered;
  {
    std::unique_lock<std::mutex> lock(mutex);
    both_entered = changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return entered_runs == 2; });
  }
  cancellation.cancel();
  {
    std::lock_guard<std::mutex> lock(mutex);
    released_runs = true;
  }
  changed.notify_all();
  auto cancelled = first.get();
  auto completed = peer.get();
  PS_CHECK(first_entered && both_entered && !gate_timeout);
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled && completed.ok());
  PS_CHECK(cancelled_publications == 0 && peer_publications == 1);
  PS_CHECK(fixture.starts == 2 && fixture.polls == 2 && fixture.live == 0);
  const auto& output = completed.value().results.at("result");
  std::uint8_t value = 0;
  PS_CHECK(
      output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1).ok() &&
      value == 7);
  completed = Result<ExecutionResult>(ExecutionResult{});
  PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
           fixture.root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
int subscription_closure() {
  Fixture fixture;
  auto computed = take(fixture.context->execute(fixture.plan));
  const auto& output = computed.results.at("result");
  std::mutex mutex;
  std::condition_variable changed;
  bool entered_callback = false, leave_callback = false, timeout = false;
  std::atomic<unsigned> delivered{0};
  auto capture = std::make_shared<int>(7);
  std::weak_ptr<int> retained = capture;
  execution_internal::ResultSubscription subscription(
      fixture.root, {},
      [capture = std::move(capture), &mutex, &changed, &entered_callback,
       &leave_callback, &timeout, &delivered](ValueRef, const ResultRef&) {
        ++delivered;
        std::unique_lock<std::mutex> lock(mutex);
        entered_callback = true;
        changed.notify_all();
        timeout = !changed.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return leave_callback; });
        return Status::success();
      });
  subscription.initialize(1);
  auto notification = std::async(std::launch::async, [&] {
    return subscription.notify(0, {1, 0}, output, 1);
  });
  bool entered_now;
  {
    std::unique_lock<std::mutex> lock(mutex);
    entered_now = changed.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return entered_callback; });
  }
  auto closed = std::async(std::launch::async, [&] { subscription.close(); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (subscription.enabled() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool closing = !subscription.enabled();
  const bool prematurely_closed =
      closed.wait_for(std::chrono::milliseconds(20)) ==
      std::future_status::ready;
  const bool retained_while_active = !retained.expired();
  auto suppressed = subscription.notify(0, {1, 0}, output, 2);
  {
    std::lock_guard<std::mutex> lock(mutex);
    leave_callback = true;
  }
  changed.notify_all();
  auto notified = notification.get();
  closed.get();
  PS_CHECK(entered_now && closing && !timeout && !prematurely_closed &&
           retained_while_active && retained.expired());
  PS_CHECK(suppressed.ok() && notified.ok() && delivered == 1);
  PS_CHECK(!subscription.has_callback() && subscription.status().ok());
  subscription.close();
  PS_CHECK(subscription.notify(0, {1, 0}, output, 3).ok() && delivered == 1);
  return 0;
}
int subscription_revisions_and_failures() {
  Fixture fixture;
  auto computed = take(fixture.context->execute(fixture.plan));
  const auto& output = computed.results.at("result");
  CancellationSource cancellation;
  unsigned delivered = 0;
  execution_internal::ResultSubscription subscription(
      fixture.root, cancellation.token(), [&](ValueRef, const ResultRef&) {
        ++delivered;
        return Status::success();
      });
  subscription.initialize(2);
  for (auto entry : {std::make_pair(0U, 1U), std::make_pair(0U, 1U),
                     std::make_pair(1U, 1U), std::make_pair(0U, 2U)})
    PS_CHECK(
        subscription
            .notify(entry.first, {entry.first + 1, 0}, output, entry.second)
            .ok());
  PS_CHECK(delivered == 3);
  cancellation.cancel();
  PS_CHECK(subscription.notify(1, {2, 0}, output, 2).ok() && delivered == 3);
  subscription.close();
  for (unsigned failure : {0U, 1U, 2U}) {
    delivered = 0;
    execution_internal::ResultSubscription failing(
        fixture.root, {}, [&](ValueRef, const ResultRef&) -> Status {
          ++delivered;
          if (failure == 1)
            throw std::bad_alloc();
          if (failure == 2)
            throw std::runtime_error("observer failure");
          return Status{ErrorCode::OperationFailed, "observer failure"};
        });
    failing.initialize(1);
    const auto expected = failure == 1 ? ErrorCode::ResourceExhausted
                                       : ErrorCode::OperationFailed;
    PS_CHECK(failing.notify(0, {1, 0}, output, 1).code == expected);
    PS_CHECK(failing.notify(0, {1, 0}, output, 2).ok() && delivered == 1);
    PS_CHECK(!failing.enabled() && failing.status().code == expected &&
             failing.code() == expected);
    failing.close();
  }
  return 0;
}
int worker_cancellation() {
  for (unsigned mode : {1U, 2U}) {
    {
      Fixture fixture(mode);
      std::mutex mutex;
      std::condition_variable changed;
      bool entered_work = false, resume_work = false, timeout = false;
      fixture.probe.gate = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        entered_work = true;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(10),
                              [&] { return resume_work; }))
          timeout = true;
      };
      auto frozen = take(fixture.context->freeze(fixture.plan, {}));
      CancellationSource cancellation;
      auto first = std::async(std::launch::async, [&] {
        return fixture.context->execute(frozen, cancellation.token());
      });
      bool entered_now;
      {
        std::unique_lock<std::mutex> lock(mutex);
        entered_now = changed.wait_for(lock, std::chrono::seconds(5),
                                       [&] { return entered_work; });
      }
      cancellation.cancel();
      {
        std::lock_guard<std::mutex> lock(mutex);
        resume_work = true;
      }
      changed.notify_all();
      auto cancelled = first.get();
      PS_CHECK(entered_now && !timeout);
      PS_CHECK(cancelled.status().code == ErrorCode::Cancelled);
      PS_CHECK(fixture.probe.units == 0U && fixture.starts == 1 &&
               fixture.polls == 1);
      PS_CHECK(fixture.live == 0);
      PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
               fixture.root.statistics().live[ResourceKind::Payload] == 0);
    }
  }
  return 0;
}
int pending_dependency() {
  for (unsigned mode : {0U, 2U, 3U}) {
    Fixture fixture(0, true);
    auto frozen = take(fixture.context->freeze(fixture.plan, {}));
    selected_retirement = 4;
    entered = release = timed_out = false;
    retired_bodies = 0;
    execution_testing::ExecutionTestHooks hooks;
    hooks.callback_body_finished = hold_retirement;
    execution_testing::install_execution_test_hooks(&hooks);
    CancellationSource cancellation;
    auto first = std::async(std::launch::async, [&] {
      return fixture.context->execute(frozen, cancellation.token());
    });
    bool reached;
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      reached = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                      [] { return entered; });
    }
    const auto before = fixture.root.statistics().issued;
    const bool prematurely_returned =
        first.wait_for(std::chrono::milliseconds(20)) ==
        std::future_status::ready;
    const auto after = fixture.root.statistics().issued;
    const auto source_polls = fixture.polls.load();
    const auto consumer_polls = fixture.consumer_polls.load();
    if (mode == 2)
      cancellation.cancel();
    const bool drains = first.wait_for(std::chrono::milliseconds(20)) !=
                        std::future_status::ready;
    if (mode == 3) {
      frozen = {};
      fixture.plan = {};
    }
    {
      std::lock_guard<std::mutex> lock(gate_mutex);
      release = true;
    }
    gate_changed.notify_all();
    auto initial = first.get();
    execution_testing::install_execution_test_hooks(nullptr);
    PS_CHECK(reached && !timed_out && !prematurely_returned);
    PS_CHECK(drains);
    PS_CHECK(before.work == after.work && before.stages == after.stages &&
             source_polls == 1 && consumer_polls == 1);
    PS_CHECK(mode == 2 ? initial.status().code == ErrorCode::Cancelled
                       : initial.ok());
    if (mode != 2) {
      auto& output = initial.value().results.at("result");
      std::uint8_t value = 0;
      PS_CHECK(output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1)
                   .ok() &&
               value == 14);
    }
    PS_CHECK(fixture.starts == 1 && fixture.polls == 1 &&
             fixture.consumer_starts == 1 &&
             fixture.consumer_polls == (mode == 2 ? 1 : 2));
    initial = Result<ExecutionResult>(ExecutionResult{});
    PS_CHECK(fixture.live == 0);
    PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
             fixture.root.statistics().live[ResourceKind::Payload] == 0);
  }
  return 0;
}
}  // namespace
int main() {
  try {
    PS_CHECK(dropped_submission() == 0);
    PS_CHECK(retirement_notification() == 0);
    PS_CHECK(cancelled_subscription() == 0);
    PS_CHECK(subscription_closure() == 0);
    PS_CHECK(subscription_revisions_and_failures() == 0);
    PS_CHECK(worker_cancellation() == 0);
    PS_CHECK(pending_dependency() == 0);
    return 0;
  } catch (const std::exception& error) {
    execution_testing::install_execution_test_hooks(nullptr);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
