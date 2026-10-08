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
  check(builder.bind_descriptor_relation(
      take(ResultRelation::cartesian(phase.resources, 1, {}))));
  const std::uint8_t value = 7;
  check(builder.publish_tensor(
      0, Region::whole({1}), ByteView(&value, 1),
      take(ResultRelation::cartesian(phase.resources, 1, {})),
      {true, true, true, true}));
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
  static int tile(void* raw, const ps_cpu_tile_v1* tile) noexcept {
    return range(raw, tile->begin[0], tile->end[0], tile->slot);
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++*polls;
    if (probe->enabled) {
      Work work{&phase, probe};
      int status = 1;
      if (probe->tiles && phase.cpu_tiles) {
        const ps_cpu_tile_stage_v1 stage{sizeof(ps_cpu_tile_stage_v1),
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
struct BatchIo {
  std::atomic<unsigned>* polls;
  std::atomic<unsigned>* live;
  TemporaryStorage* retained;
  bool malformed;
  unsigned stage = 0;
  BatchIo(std::atomic<unsigned>* count, std::atomic<unsigned>* instances,
          TemporaryStorage* temporary, bool invalid)
      : polls(count), live(instances), retained(temporary), malformed(invalid) {
    ++*live;
  }
  ~BatchIo() { --*live; }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++*polls;
    using Poll = Result<ResultProgramPoll>;
    switch (stage++) {
      case 0:
        return Poll(ResultProgramNeed{{}, {ResultCreateTemporary{}}});
      case 1:
        if (phase.io.size() != 1)
          return Poll(Status{ErrorCode::OperationFailed, "create reply count"});
        *retained = std::get<TemporaryStorage>(phase.io.at(0));
        if (malformed)
          return Poll(
              ResultProgramNeed{{},
                                {ResultExtendTemporary{*retained, 1},
                                 ResultReadTemporary{*retained, 0, 0}}});
        return Poll(ResultProgramNeed{{},
                                      {ResultExtendTemporary{*retained, 1},
                                       ResultExtendTemporary{*retained, 2},
                                       ResultExtendTemporary{*retained, 3}}});
      case 2:
        if (phase.io.size() != 3 || retained->size() != 6 ||
            std::get<std::uint64_t>(phase.io.at(0)) != 0 ||
            std::get<std::uint64_t>(phase.io.at(1)) != 1 ||
            std::get<std::uint64_t>(phase.io.at(2)) != 3)
          return Poll(Status{ErrorCode::OperationFailed, "batch replayed"});
        return Poll(
            ResultProgramNeed{{}, {ResultReadTemporary{*retained, 0, 6}}});
      case 3: {
        if (phase.io.size() != 1)
          return Poll(Status{ErrorCode::OperationFailed, "read reply count"});
        auto bytes = std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0))
                         ->bytes();
        if (bytes.size() != 6)
          return Poll(Status{ErrorCode::OperationFailed, "read size"});
        for (auto byte : bytes)
          if (byte)
            return Poll(Status{ErrorCode::OperationFailed, "read contents"});
        return sample(phase);
      }
      default:
        return Poll(Status{ErrorCode::OperationFailed, "extra poll"});
    }
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
        need.tensors.push_back({port, 0, take(Footprint::all({1})), 9});
      return Result<ResultProgramPoll>(std::move(need));
    }
    std::uint8_t left = 0, right = 0;
    check(phase.read_tensor(0, 0, {0}, &left, 1));
    check(phase.read_tensor(1, 0, {0}, &right, 1));
    const std::uint8_t value = left + right;
    auto builder = take(ResultBuilder::start(phase.resources,
                                             *phase.query.output.result_schema,
                                             phase.query.semantic_key));
    auto descriptor = take(ResultRelation::unite(
        phase.resources,
        {take(ResultRelation::cartesian(
             phase.resources, 1,
             {0, 8, 0, 1, ResultSupportTarget::Descriptor, 0})),
         take(ResultRelation::cartesian(
             phase.resources, 1,
             {1, 8, 0, 1, ResultSupportTarget::Descriptor, 0}))}));
    check(builder.bind_descriptor_relation(std::move(descriptor)));
    auto samples = take(ResultRelation::unite(
        phase.resources,
        {take(ResultRelation::cartesian(
             phase.resources, 1, {0, 1, 0, 1, ResultSupportTarget::Tensor, 0})),
         take(ResultRelation::cartesian(
             phase.resources, 1,
             {1, 1, 0, 1, ResultSupportTarget::Tensor, 0}))}));
    check(builder.publish_tensor(0, Region::whole({1}), ByteView(&value, 1),
                                 std::move(samples), {true, true, true, true}));
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
  TemporaryStorage temporary;
  explicit Fixture(unsigned worker_mode = 0, unsigned io_mode = 0,
                   bool dependency = false) {
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
    out.output_schema.kind = OperationPortKind::Result;
    out.output_schema.result_schema_id = schema.id;
    out.output_schema.result_schema_version = 1;
    out.result_schema = schema;
    out.region_rule = OperationRegionRule::Whole;
    out.dependency_version = 2;
    out.continuation_bytes = io_mode ? sizeof(BatchIo) : sizeof(Source);
    out.maximum_dependency_stages = io_mode ? 4 : 1;
    source.start_result = [this, io_mode](const auto&, const auto& allocator) {
      ++starts;
      if (io_mode)
        return ResultContinuation::make<BatchIo>(allocator, &polls, &live,
                                                 &temporary, io_mode == 2);
      return ResultContinuation::make<Source>(allocator, &polls, &live, &probe);
    };
    if (dependency) {
      OperationDefinition consumer;
      consumer.key = "consumer";
      consumer.traits = source.traits;
      consumer.traits.input_count = 2;
      OperationPortConstraint input;
      input.kind = OperationPortKind::Result;
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
    config.cpu_workers = probe.enabled ? 2 : 1;
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
    plan = take(Compiler(registry).compile(*graph)).plan;
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
bool handoff_entered = false, handoff_release = false;
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
void hold_handoff() noexcept {
  std::unique_lock<std::mutex> lock(gate_mutex);
  handoff_entered = true;
  gate_changed.notify_all();
  if (!gate_changed.wait_for(lock, std::chrono::seconds(10),
                             [] { return handoff_release; }))
    timed_out = true;
}
std::atomic<unsigned> handoffs{0};
void reject_handoff() {
  ++handoffs;
  throw std::bad_alloc();
}
void hold_two_retirements() noexcept {
  std::unique_lock<std::mutex> lock(gate_mutex);
  const auto phase = ++retired_bodies;
  if (phase != 4 && phase != 5)
    return;
  (phase == 4 ? entered : handoff_entered) = true;
  gate_changed.notify_all();
  if (!gate_changed.wait_for(lock, std::chrono::seconds(10), [phase] {
        return phase == 4 ? release : handoff_release;
      }))
    timed_out = true;
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
  Fixture fixture;
  auto frozen = take(fixture.context->freeze(fixture.plan, {}));
  selected_retirement = 2;
  entered = release = timed_out = false;
  retired_bodies = 0;
  execution_testing::ExecutionTestHooks hooks;
  hooks.callback_body_finished = hold_retirement;
  execution_testing::install_execution_test_hooks(&hooks);
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
  bool body;
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    body = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                 [] { return entered; });
  }
  auto peer = std::async(std::launch::async, [&] {
    return fixture.context->execute(frozen, {}, peer_options);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!fixture.context->cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool shared =
      fixture.context->cache_statistics().shared_computations != 0;
  cancellation.cancel();
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  auto cancelled = first.get();
  auto completed = peer.get();
  execution_testing::install_execution_test_hooks(nullptr);
  PS_CHECK(body && shared && !timed_out);
  PS_CHECK(cancelled.status().code == ErrorCode::Cancelled && completed.ok());
  PS_CHECK(cancelled_publications == 0 && peer_publications == 1);
  PS_CHECK(fixture.starts == 1 && fixture.polls == 1 && fixture.live == 0);
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
int shared_worker_cancellation() {
  for (unsigned mode : {1U, 2U}) {
    for (bool with_peer : {false, true}) {
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
      std::future<Result<ExecutionResult>> peer;
      bool shared = !with_peer;
      if (with_peer) {
        peer = std::async(std::launch::async,
                          [&] { return fixture.context->execute(frozen); });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!fixture.context->cache_statistics().shared_computations &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::yield();
        shared = fixture.context->cache_statistics().shared_computations != 0;
      }
      cancellation.cancel();
      {
        std::lock_guard<std::mutex> lock(mutex);
        resume_work = true;
      }
      changed.notify_all();
      auto cancelled = first.get();
      auto completed =
          with_peer ? peer.get() : Result<ExecutionResult>(ExecutionResult{});
      PS_CHECK(entered_now && shared && !timeout);
      PS_CHECK(cancelled.status().code == ErrorCode::Cancelled &&
               completed.ok());
      PS_CHECK(fixture.probe.units == (with_peer ? 64U : 0U) &&
               fixture.starts == 1 && fixture.polls == 1);
      PS_CHECK(fixture.live == 0 &&
               fixture.context->cache_statistics().in_flight == 0);
      if (with_peer) {
        const auto& output = completed.value().results.at("result");
        std::uint8_t value = 0;
        PS_CHECK(
            output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1)
                .ok() &&
            value == 7);
      }
      completed = Result<ExecutionResult>(ExecutionResult{});
      PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
               fixture.root.statistics().live[ResourceKind::Payload] == 0);
    }
  }
  return 0;
}
int need_batch_progress() {
  for (unsigned mode : {1U, 2U}) {
    Fixture fixture(0, mode);
    auto result = fixture.context->execute(fixture.plan);
    PS_CHECK(fixture.starts == 1 && fixture.live == 0);
    if (mode == 1) {
      PS_CHECK(result.ok() && fixture.polls == 4 &&
               fixture.temporary.size() == 6);
      const auto& output = result.value().results.at("result");
      std::uint8_t value = 0;
      PS_CHECK(output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1)
                   .ok() &&
               value == 7);
    } else {
      PS_CHECK(result.status().code == ErrorCode::InvalidArgument &&
               fixture.polls == 2 && fixture.temporary.size() == 0);
      PS_CHECK(fixture.root.statistics().issued.io_bytes == 0);
    }
    result = Result<ExecutionResult>(ExecutionResult{});
    fixture.temporary = {};
    const auto live = fixture.root.statistics().live;
    PS_CHECK(live[ResourceKind::Queue] == 0 &&
             live[ResourceKind::Payload] == 0 &&
             live[ResourceKind::Files] == 0 && live[ResourceKind::Disk] == 0);
  }
  return 0;
}
std::atomic<unsigned> completed_io{0};
unsigned io_failure_kind = 0;
void fail_io_reply() {
  if (++completed_io != 2)
    return;
  std::unique_lock<std::mutex> lock(gate_mutex);
  entered = true;
  gate_changed.notify_all();
  timed_out = !gate_changed.wait_for(lock, std::chrono::seconds(10),
                                     [] { return release; });
  if (!io_failure_kind)
    throw std::bad_alloc();
  throw std::runtime_error("I/O reply failure");
}
int shared_need_failure() {
  for (unsigned failure : {0U, 1U}) {
    Fixture fixture(0, 1);
    completed_io = 0;
    io_failure_kind = failure;
    entered = release = timed_out = false;
    execution_testing::ExecutionTestHooks hooks;
    hooks.structured_io_completed = fail_io_reply;
    execution_testing::install_execution_test_hooks(&hooks);
    auto frozen = take(fixture.context->freeze(fixture.plan, {}));
    auto first = std::async(std::launch::async,
                            [&] { return fixture.context->execute(frozen); });
    bool reached;
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      reached = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                      [] { return entered; });
    }
    auto peer = std::async(std::launch::async,
                           [&] { return fixture.context->execute(frozen); });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fixture.context->cache_statistics().shared_computations &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool shared =
        fixture.context->cache_statistics().shared_computations != 0;
    {
      std::lock_guard<std::mutex> lock(gate_mutex);
      release = true;
    }
    gate_changed.notify_all();
    auto failed = first.get();
    auto observed = peer.get();
    execution_testing::install_execution_test_hooks(nullptr);
    const auto expected =
        failure ? ErrorCode::OperationFailed : ErrorCode::ResourceExhausted;
    PS_CHECK(reached && shared && !timed_out);
    if (failed.status().code != expected ||
        observed.status().code != expected || completed_io != 2 ||
        fixture.temporary.size() != 1)
      std::cerr << "I/O failure first="
                << static_cast<unsigned>(failed.status().code)
                << " peer=" << static_cast<unsigned>(observed.status().code)
                << " actions=" << completed_io
                << " bytes=" << fixture.temporary.size() << '\n';
    PS_CHECK(failed.status().code == expected &&
             observed.status().code == expected);
    PS_CHECK(completed_io == 2 && fixture.temporary.size() == 1 &&
             fixture.starts == 1 && fixture.polls == 2 && fixture.live == 0);
    fixture.temporary = {};
    const auto live = fixture.root.statistics().live;
    PS_CHECK(live[ResourceKind::Queue] == 0 &&
             live[ResourceKind::Payload] == 0 &&
             live[ResourceKind::Files] == 0 && live[ResourceKind::Disk] == 0);
    PS_CHECK(fixture.context->cache_statistics().in_flight == 0);
  }
  return 0;
}
int pending_dependency() {
  for (unsigned mode : {0U, 1U, 2U, 3U, 4U}) {
    Fixture fixture(0, 0, true);
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
    std::future<Result<ExecutionResult>> peer;
    CancellationSource peer_cancel;
    const bool has_peer = mode == 1 || mode == 4;
    bool shared = !has_peer;
    if (has_peer) {
      peer = std::async(std::launch::async, [&] {
        return fixture.context->execute(frozen, peer_cancel.token());
      });
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!fixture.context->cache_statistics().shared_computations &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
      shared = fixture.context->cache_statistics().shared_computations != 0;
    }
    if (has_peer || mode == 2)
      cancellation.cancel();
    const bool detached =
        !has_peer ||
        first.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    bool last_waiter_drains = true;
    if (mode == 4) {
      peer_cancel.cancel();
      last_waiter_drains = peer.wait_for(std::chrono::milliseconds(20)) !=
                           std::future_status::ready;
    }
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
    auto surviving =
        has_peer ? peer.get() : Result<ExecutionResult>(ExecutionResult{});
    execution_testing::install_execution_test_hooks(nullptr);
    PS_CHECK(reached && shared && !timed_out && !prematurely_returned);
    PS_CHECK(detached && last_waiter_drains);
    PS_CHECK(before.work == after.work && before.stages == after.stages &&
             source_polls == 1 && consumer_polls == 1);
    PS_CHECK(has_peer || mode == 2
                 ? initial.status().code == ErrorCode::Cancelled
                 : initial.ok());
    if (mode == 1 && !surviving.ok())
      std::cerr << "dependency peer code="
                << static_cast<unsigned>(surviving.status().code)
                << " node=" << surviving.status().detail.node_id
                << " message=" << surviving.status().message << '\n';
    PS_CHECK(mode != 1 || surviving.ok());
    PS_CHECK(mode != 4 || surviving.status().code == ErrorCode::Cancelled);
    if (mode != 2 && mode != 4) {
      auto& output =
          (mode == 1 ? surviving : initial).value().results.at("result");
      std::uint8_t value = 0;
      PS_CHECK(output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1)
                   .ok() &&
               value == 14);
    }
    PS_CHECK(fixture.starts == 1 && fixture.polls == 1 &&
             fixture.consumer_starts == 1 &&
             fixture.consumer_polls == (mode == 2 || mode == 4 ? 1 : 2));
    initial = Result<ExecutionResult>(ExecutionResult{});
    surviving = Result<ExecutionResult>(ExecutionResult{});
    PS_CHECK(fixture.live == 0 &&
             fixture.context->cache_statistics().in_flight == 0);
    PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
             fixture.root.statistics().live[ResourceKind::Payload] == 0);
  }
  return 0;
}
int detached_context_shutdown() {
  Fixture fixture(0, 0, true);
  auto handle = take(fixture.context->open_demand(fixture.plan, {}));
  auto frozen = take(handle.freeze());
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
  auto peer = std::async(std::launch::async, [&] {
    return handle.request({{"result", take(Footprint::all({1}))}});
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!fixture.context->cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool shared = fixture.context->cache_statistics().shared_computations;
  cancellation.cancel();
  const bool detached =
      first.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  // The first call must be retired before destroying its context concurrently.
  std::future<void> closing;
  bool draining = false;
  if (detached) {
    closing = std::async(std::launch::async, [&] { fixture.context.reset(); });
    draining = closing.wait_for(std::chrono::milliseconds(20)) !=
               std::future_status::ready;
  }
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  const auto first_result = first.get();
  const auto peer_result = peer.get();
  if (closing.valid())
    closing.get();
  execution_testing::install_execution_test_hooks(nullptr);
  PS_CHECK(reached && shared && detached && draining && !timed_out);
  PS_CHECK(first_result.status().code == ErrorCode::Cancelled &&
           peer_result.status().code == ErrorCode::Cancelled);
  PS_CHECK(fixture.starts == 1 && fixture.polls == 1 &&
           fixture.consumer_starts == 1 && fixture.consumer_polls == 1 &&
           fixture.live == 0);
  PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
           fixture.root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
int final_peer_before_handoff() {
  Fixture fixture;
  auto frozen = take(fixture.context->freeze(fixture.plan, {}));
  selected_retirement = 2;
  entered = release = timed_out = handoff_entered = handoff_release = false;
  retired_bodies = 0;
  execution_testing::ExecutionTestHooks hooks;
  hooks.callback_body_finished = hold_retirement;
  hooks.structured_handoff_ready = hold_handoff;
  execution_testing::install_execution_test_hooks(&hooks);
  CancellationSource first_cancel, peer_cancel;
  auto first = std::async(std::launch::async, [&] {
    return fixture.context->execute(frozen, first_cancel.token());
  });
  bool reached;
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    reached = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                    [] { return entered; });
  }
  auto peer = std::async(std::launch::async, [&] {
    return fixture.context->execute(frozen, peer_cancel.token());
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!fixture.context->cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool shared = fixture.context->cache_statistics().shared_computations;
  first_cancel.cancel();
  bool handing_off;
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    handing_off = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                        [] { return handoff_entered; });
  }
  peer_cancel.cancel();
  const bool peer_left =
      peer.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    handoff_release = true;
  }
  gate_changed.notify_all();
  const bool first_drains = first.wait_for(std::chrono::milliseconds(20)) !=
                            std::future_status::ready;
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  const auto first_result = first.get();
  const auto peer_result = peer.get();
  execution_testing::install_execution_test_hooks(nullptr);
  PS_CHECK(reached && shared && handing_off && peer_left && first_drains &&
           !timed_out);
  PS_CHECK(first_result.status().code == ErrorCode::Cancelled &&
           peer_result.status().code == ErrorCode::Cancelled);
  PS_CHECK(fixture.starts == 1 && fixture.polls == 1 && fixture.live == 0 &&
           fixture.context->cache_statistics().in_flight == 0);
  PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
           fixture.root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
int rejected_handoff_drains_multiple_phases() {
  Fixture fixture(0, 0, true);
  auto frozen = take(fixture.context->freeze(fixture.plan, {}));
  entered = release = timed_out = handoff_entered = handoff_release = false;
  retired_bodies = 0;
  handoffs = 0;
  execution_testing::ExecutionTestHooks hooks;
  hooks.callback_body_finished = hold_two_retirements;
  hooks.structured_handoff_ready = reject_handoff;
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
  auto peer = std::async(std::launch::async,
                         [&] { return fixture.context->execute(frozen); });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!fixture.context->cache_statistics().shared_computations &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool shared = fixture.context->cache_statistics().shared_computations;
  cancellation.cancel();
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!handoffs && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const bool rejected = handoffs == 1;
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    release = true;
  }
  gate_changed.notify_all();
  bool second_phase;
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    second_phase = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                         [] { return handoff_entered; });
  }
  const bool retained = first.wait_for(std::chrono::milliseconds(20)) !=
                        std::future_status::ready;
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    handoff_release = true;
  }
  gate_changed.notify_all();
  auto initial = first.get();
  auto surviving = peer.get();
  execution_testing::install_execution_test_hooks(nullptr);
  PS_CHECK(reached && shared && rejected && second_phase && retained &&
           !timed_out && handoffs == 1);
  PS_CHECK(initial.status().code == ErrorCode::Cancelled && surviving.ok());
  const auto& output = surviving.value().results.at("result");
  std::uint8_t value = 0;
  PS_CHECK(
      output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1).ok() &&
      value == 14);
  surviving = Result<ExecutionResult>(ExecutionResult{});
  PS_CHECK(fixture.starts == 1 && fixture.polls == 1 &&
           fixture.consumer_starts == 1 && fixture.consumer_polls == 2 &&
           fixture.live == 0 &&
           fixture.context->cache_statistics().in_flight == 0);
  PS_CHECK(fixture.root.statistics().live[ResourceKind::Queue] == 0 &&
           fixture.root.statistics().live[ResourceKind::Payload] == 0);
  return 0;
}
struct JointProtocolProbe {
  std::atomic<unsigned> live{0}, starts{0}, polls{0}, singles{0}, members{0};
  bool reject = true;
  std::uint32_t carrier = UINT32_MAX, rejected = UINT32_MAX;
};
struct JointProtocolProgram {
  JointProtocolProbe* probe;
  explicit JointProtocolProgram(JointProtocolProbe* observed)
      : probe(observed) {
    ++probe->live;
    ++probe->starts;
  }
  ~JointProtocolProgram() { --probe->live; }
  Result<ResourceVector<ResultJointOutcome>> poll(
      const ResultJointPhase& phase) {
    ++probe->polls;
    probe->members = phase.members.size();
    probe->carrier = phase.members.front()->query.output_index;
    ResourceVector<ResultJointOutcome> outcomes{
        ResourceAllocator<ResultJointOutcome>(
            phase.members.front()->resources)};
    for (std::size_t i = 0; i < phase.members.size(); ++i) {
      const auto& member = *phase.members[i];
      auto key = take(result_atom_key(member.query));
      if (probe->reject && i + 1 == phase.members.size()) {
        probe->rejected = member.query.output_index;
        outcomes.push_back(
            {key, Result<ResultProgramPoll>(
                      Status{ErrorCode::InvalidArgument,
                             "noncarrier protocol failure",
                             FailureReason::InvalidAssociation,
                             {FailureOrigin::Protocol, FailureScope::Group}})});
      } else {
        outcomes.push_back({key, sample(member)});
      }
    }
    return Result<ResourceVector<ResultJointOutcome>>(std::move(outcomes));
  }
};
struct JointProtocolSingle {
  JointProtocolProbe* probe;
  explicit JointProtocolSingle(JointProtocolProbe* observed)
      : probe(observed) {}
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    ++probe->singles;
    return sample(phase);
  }
};
int joint_protocol_survives_external_stop() {
  for (unsigned stop : {0U, 1U, 2U}) {
    JointProtocolProbe probe;
    auto registry = std::make_shared<OperationRegistry>();
    SchemaTemplate schema;
    schema.id = "test.joint_protocol";
    ResultTensorSpec tensor;
    tensor.key = "sample";
    tensor.descriptor = {ElementType::UInt8, {1}};
    schema.tensors.push_back(tensor);
    OperationDefinition operation;
    operation.key = "joint_protocol";
    operation.traits.input_count = 0;
    operation.traits.joint_contract = 1;
    operation.traits.joint_continuation_bytes = sizeof(JointProtocolProgram);
    operation.traits.outputs.resize(2);
    for (unsigned i = 0; i < 2; ++i) {
      auto& output = operation.traits.outputs[i];
      output.key = i ? "right" : "left";
      output.output_schema.kind = OperationPortKind::Result;
      output.output_schema.result_schema_id = schema.id;
      output.output_schema.result_schema_version = schema.version;
      output.result_schema = schema;
      output.region_rule = OperationRegionRule::Whole;
      output.dependency_version = 2;
      output.continuation_bytes = sizeof(JointProtocolSingle);
      output.maximum_dependency_stages = 1;
    }
    operation.start_result = [&probe](const auto&, const auto& allocator) {
      return ResultContinuation::make<JointProtocolSingle>(allocator, &probe);
    };
    operation.start_result_joint = [&probe](const auto&,
                                            const auto& allocator) {
      return ResultJointContinuation::make<JointProtocolProgram>(allocator,
                                                                 &probe);
    };
    check(registry->register_operation(std::move(operation)));
    check(registry->freeze());
    WorkflowDocument document;
    document.nodes = {{1, "joint_protocol", {}, {}}};
    document.outputs = {{"left", 1, "left"}, {"right", 1, "right"}};
    GraphContext graph(document);
    auto plan = take(Compiler(registry).compile(graph)).plan;
    ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.result_cache_bytes = 0;
    ExecutionContext execution(registry, config);
    auto root = take(execution.resource_budget());
    selected_retirement = 2;
    entered = release = timed_out = false;
    retired_bodies = 0;
    execution_testing::ExecutionTestHooks hooks;
    hooks.callback_body_finished = hold_retirement;
    execution_testing::install_execution_test_hooks(&hooks);
    CancellationSource cancellation;
    auto pending = std::async(std::launch::async, [&] {
      return execution.execute(plan, {}, cancellation.token());
    });
    bool reached;
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      reached = gate_changed.wait_for(lock, std::chrono::seconds(5),
                                      [] { return entered; });
    }
    if (stop != 1)
      cancellation.cancel();
    if (stop != 0)
      graph.replace(document);
    const bool drained_early =
        pending.wait_for(std::chrono::milliseconds(20)) ==
        std::future_status::ready;
    const auto held_live = probe.live.load();
    {
      std::lock_guard<std::mutex> lock(gate_mutex);
      release = true;
    }
    gate_changed.notify_all();
    auto failed = pending.get();
    execution_testing::install_execution_test_hooks(nullptr);
    PS_CHECK(reached && !timed_out && !drained_early && held_live == 1);
    PS_CHECK(probe.members == 2 && probe.carrier != probe.rejected &&
             probe.starts == 1 && probe.polls == 1 && probe.singles == 0);
    PS_CHECK(failed.status().code == ErrorCode::InvalidArgument &&
             failed.status().reason == FailureReason::InvalidAssociation &&
             failed.status().detail.origin == FailureOrigin::Protocol &&
             failed.status().detail.scope == FailureScope::Group &&
             failed.status().detail.node_id == 1 &&
             failed.status().message == "noncarrier protocol failure");
    PS_CHECK(probe.live == 0 &&
             root.statistics().live[ResourceKind::Queue] == 0 &&
             root.statistics().live[ResourceKind::Payload] == 0);
    probe.reject = false;
    auto current = take(Compiler(registry).compile(graph)).plan;
    auto recovered = execution.execute(current);
    PS_CHECK(recovered.ok() && recovered.value().results.size() == 2 &&
             probe.live == 0 && probe.starts == 2 && probe.singles == 0);
    for (const auto& output : recovered.value().results) {
      std::uint8_t value = 0;
      PS_CHECK(
          output.second
              .read_tensor(take(output.second.descriptor()), 0, {0}, &value, 1)
              .ok() &&
          value == 7);
    }
    recovered = Result<ExecutionResult>(ExecutionResult{});
    PS_CHECK(root.statistics().live[ResourceKind::Queue] == 0 &&
             root.statistics().live[ResourceKind::Payload] == 0);
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
    PS_CHECK(shared_worker_cancellation() == 0);
    PS_CHECK(need_batch_progress() == 0);
    PS_CHECK(shared_need_failure() == 0);
    PS_CHECK(pending_dependency() == 0);
    PS_CHECK(detached_context_shutdown() == 0);
    PS_CHECK(final_peer_before_handoff() == 0);
    PS_CHECK(rejected_handoff_drains_multiple_phases() == 0);
    PS_CHECK(joint_protocol_survives_external_stop() == 0);
    return 0;
  } catch (const std::exception& error) {
    execution_testing::install_execution_test_hooks(nullptr);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
