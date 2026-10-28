#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/execution/execution.hpp"
#include "support/fixed_operation_fixture.hpp"
#include "support/status_check.hpp"

namespace {
using Clock = std::chrono::steady_clock;
constexpr auto timeout = std::chrono::seconds(5);
constexpr std::uint64_t host_limit = 16U * 1024U * 1024U;
template <class T>
T take(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message.c_str());
  return result.take_value();
}
void check(const ps::Status& status) {
  if (!status.ok())
    throw std::runtime_error(status.message.c_str());
}
struct ReleaseBarrier {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned arrived = 0;
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    ++arrived;
    changed.notify_all();
    if (!changed.wait_for(lock, timeout, [&] { return arrived == 2; }))
      std::terminate();
  }
};
struct Probe {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned prepared = 0, entered = 0;
  bool invoke_open = false, release_open = false, fail = false;
  unsigned blocked_release = 8, self_release = 8, exit_release = 8;
  bool exit_entered = false, exit_open = false, exit_done = false;
  std::function<void()> thread_exit;
  std::weak_ptr<ps::ExecutionContext> context;
  std::shared_ptr<ReleaseBarrier> release_barrier;
  std::array<bool, 8> releasing{}, released{};
  std::array<std::thread::id, 8> invoke_threads{}, release_threads{};
};
struct ThreadExit {
  std::function<void()> callback;
  ~ThreadExit() {
    if (callback)
      callback();
  }
};
ps::OperationDefinition operation(const std::shared_ptr<Probe>& probe) {
  auto definition = ps::test::fixed_fixture::operation("lifetime", 0);
  const auto prepare = definition.prepare_kernel;
  definition.prepare_kernel = [prepare, probe](
                                  const auto& kernel, const auto& placement,
                                  const auto& query, const auto& resources,
                                  const ps_native_gpu_service_v2* device)
      -> ps::Result<ps_prepared_kernel_v8> {
    auto base = prepare(kernel, placement, query, resources, device);
    if (!base.ok())
      return base;
    struct State {
      ps_prepared_kernel_v8 inner;
      std::shared_ptr<Probe> probe;
      unsigned index;
      std::shared_ptr<ps::ExecutionContext> context;
      ~State() {
        if (inner.release)
          inner.release(inner.context);
      }
    };
    const auto index = probe->prepared++;
    if (index >= probe->released.size())
      throw std::runtime_error("unexpected preparation count");
    auto raw = base.take_value();
    raw.context = new State{
        raw, probe, index,
        index == probe->self_release ? probe->context.lock() : nullptr};
    raw.release = [](const void* context) {
      const auto* state = static_cast<const State*>(context);
      auto probe = state->probe;
      const auto index = state->index;
      if (index == probe->exit_release) {
        static thread_local ThreadExit thread_exit;
        thread_exit.callback = probe->thread_exit;
      }
      {
        std::unique_lock<std::mutex> lock(probe->mutex);
        probe->release_threads[index] = std::this_thread::get_id();
        probe->releasing[index] = true;
        probe->changed.notify_all();
        if (index == probe->blocked_release &&
            !probe->changed.wait_for(lock, timeout,
                                     [&] { return probe->release_open; }))
          std::terminate();
      }
      if (probe->release_barrier)
        probe->release_barrier->wait();
      // Dropping the last public Context owner here is legal: invocation and
      // observer delivery have finished. No fixture mutex crosses destruction.
      delete state;
      {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->released[index] = true;
      }
      probe->changed.notify_all();
    };
    raw.invoke_cpu = [](const ps_kernel_invocation_v8* invocation,
                        ps_kernel_status_v8* status) {
      const auto& state =
          *static_cast<const State*>(invocation->prepared_context);
      auto& probe = *state.probe;
      {
        std::unique_lock<std::mutex> lock(probe.mutex);
        probe.invoke_threads[state.index] = std::this_thread::get_id();
        ++probe.entered;
        probe.changed.notify_all();
        const auto deadline = Clock::now() + timeout;
        while (!probe.invoke_open && !invocation->cancellation.is_cancelled(
                                         invocation->cancellation.context)) {
          if (Clock::now() >= deadline) {
            *status = ps::kernel_failure(ps::ErrorCode::OperationFailed,
                                         "invocation gate timed out");
            return;
          }
          probe.changed.wait_for(lock, std::chrono::milliseconds(1));
        }
      }
      if (probe.fail) {
        *status = ps::kernel_failure(ps::ErrorCode::OperationFailed,
                                     "injected kernel failure");
        return;
      }
      auto mapped = *invocation;
      mapped.prepared_context = state.inner.context;
      state.inner.invoke_cpu(&mapped, status);
    };
    return ps::Result<ps_prepared_kernel_v8>(raw);
  };
  return definition;
}
ps::WorkflowDocument document() {
  ps::WorkflowDocument result;
  result.nodes = {{1, "lifetime", {}, {}}};
  result.outputs = {{"value", 1, "out0"}};
  return result;
}
struct Fixture {
  std::shared_ptr<Probe> probe = std::make_shared<Probe>();
  std::shared_ptr<ps::OperationRegistry> registry =
      std::make_shared<ps::OperationRegistry>();
  ps::GraphContext graph{document()};
  ps::ExecutionPlan plan;
  Fixture() {
    check(registry->register_operation(operation(probe)));
    check(registry->freeze());
    ps::PlanningOptions planning;
    planning.tile_extent = 4;
    plan = take(ps::Compiler(registry).compile(graph, planning)).plan.value();
  }
  std::shared_ptr<ps::ExecutionContext> context() {
    ps::ExecutionContextConfig config;
    config.cpu_workers = 4;
    config.maximum_runs = 8;
    config.maximum_live_bytes = host_limit;
    ps::ResourceLimits limits;
    limits.capacity[ps::ResourceKind::Host] = host_limit;
    limits.capacity[ps::ResourceKind::Metadata] = host_limit;
    limits.cleanup = {};
    config.managed_resources = limits;
    return std::make_shared<ps::ExecutionContext>(registry, config);
  }
  bool entered(unsigned count) {
    std::unique_lock<std::mutex> lock(probe->mutex);
    return probe->changed.wait_for(lock, timeout,
                                   [&] { return probe->entered == count; });
  }
  void open() {
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->invoke_open = true;
    }
    probe->changed.notify_all();
  }
};
bool empty(const ps::ResourceBudget& root) {
  const auto stats = root.statistics();
  for (auto live : stats.live.values)
    if (live)
      return false;
  return stats.allocated_metadata_bytes == 0;
}
int immediately_reusable(unsigned count, unsigned mode, bool observe) {
  Fixture fixture;
  fixture.probe->fail = mode == 1;
  auto context = fixture.context();
  auto root = take(context->resource_budget());
  {
    ps::ExecutionOptions options;
    if (observe) {
      options.on_call_retired = [mode](const ps::CallRecord&) {
        if (mode == 3)
          throw std::runtime_error("injected observer failure");
      };
    }
    std::vector<ps::PreparedPlan> prepared;
    std::vector<ps::RunHandle> runs;
    for (unsigned i = 0; i < count; ++i) {
      prepared.push_back(take(context->prepare(fixture.plan)));
      runs.push_back(take(context->submit(prepared.back(), {}, {}, options)));
    }
    const bool started = fixture.entered(count);
    if (mode == 2)
      for (const auto& run : runs)
        run.request_cancel();
    fixture.open();
    for (const auto& run : runs) {
      const auto result = run.join();
      const auto expected = mode == 2   ? ps::ErrorCode::Cancelled
                            : mode != 0 ? ps::ErrorCode::OperationFailed
                                        : ps::ErrorCode::Ok;
      PS_CHECK(result.status().code == expected);
    }
    PS_CHECK(started);
  }
  context.reset();
  PS_CHECK(empty(root));
  // Account for the new lease itself so any residual coordinator reservation
  // makes this exact full-root reuse fail rather than hiding in spare capacity.
  auto full = root.reserve(ps::ResourceCapacity::host(
      host_limit - ps::ResourceBudget::lease_metadata_bytes()));
  PS_REQUIRE_OK(full);
  return 0;
}
int cleanup_barrier(unsigned blocked) {
  Fixture fixture;
  fixture.probe->blocked_release = blocked;
  auto context = fixture.context();
  auto root = take(context->resource_budget());
  std::vector<ps::PreparedPlan> prepared;
  std::vector<ps::RunHandle> runs;
  for (unsigned i = 0; i < 3; ++i) {
    prepared.push_back(take(context->prepare(fixture.plan)));
    runs.push_back(take(context->submit(prepared.back())));
  }
  const bool started = fixture.entered(3);
  runs.clear();
  prepared.clear();
  fixture.open();
  bool releasing;
  {
    std::unique_lock<std::mutex> lock(fixture.probe->mutex);
    releasing = fixture.probe->changed.wait_for(
        lock, timeout, [&] { return fixture.probe->releasing[blocked]; });
  }
  auto destroying =
      std::async(std::launch::async,
                 [context = std::move(context)]() mutable { context.reset(); });
  const bool waited = destroying.wait_for(std::chrono::milliseconds(30)) ==
                      std::future_status::timeout;
  {
    std::lock_guard<std::mutex> lock(fixture.probe->mutex);
    fixture.probe->release_open = true;
  }
  fixture.probe->changed.notify_all();
  destroying.get();
  PS_CHECK(started && releasing && waited);
  PS_CHECK(empty(root));
  for (unsigned i = 0; i < 3; ++i) {
    PS_CHECK(fixture.probe->released[i]);
    PS_CHECK(fixture.probe->release_threads[i] !=
             fixture.probe->invoke_threads[i]);
  }
  return 0;
}
int self_release(unsigned count) {
  Fixture fixture;
  const auto last = count - 1;
  fixture.probe->self_release = last;
  auto context = fixture.context();
  fixture.probe->context = context;
  auto root = take(context->resource_budget());
  std::vector<ps::PreparedPlan> prepared;
  std::vector<ps::RunHandle> runs;
  for (unsigned i = 0; i < count; ++i) {
    prepared.push_back(take(context->prepare(fixture.plan)));
    runs.push_back(take(context->submit(prepared.back())));
  }
  const bool started = fixture.entered(count);
  runs.clear();
  prepared.clear();
  context.reset();
  fixture.open();
  bool released;
  {
    std::unique_lock<std::mutex> lock(fixture.probe->mutex);
    released = fixture.probe->changed.wait_for(
        lock, timeout, [&] { return fixture.probe->released[last]; });
  }
  PS_CHECK(started && released && fixture.probe->context.expired());
  PS_CHECK(fixture.probe->release_threads[last] != std::this_thread::get_id());
  PS_CHECK(fixture.probe->release_threads[last] !=
           fixture.probe->invoke_threads[last]);
  // The currently executing provider destructor cannot join its own thread.
  // Its retained slot and final ContextState owner retire after it unwinds.
  const auto deadline = Clock::now() + timeout;
  while (!empty(root) && Clock::now() < deadline)
    std::this_thread::yield();
  PS_CHECK(empty(root));
  return 0;
}
int cross_context_release() {
  Fixture first, second;
  auto a = first.context(), b = second.context();
  auto a_root = take(a->resource_budget());
  auto b_root = take(b->resource_budget());
  first.probe->self_release = second.probe->self_release = 0;
  first.probe->context = b;
  second.probe->context = a;
  first.probe->release_barrier = second.probe->release_barrier =
      std::make_shared<ReleaseBarrier>();
  auto pa = take(a->prepare(first.plan));
  auto pb = take(b->prepare(second.plan));
  auto ra = take(a->submit(pa)), rb = take(b->submit(pb));
  const bool started = first.entered(1) && second.entered(1);
  pa = {};
  pb = {};
  ra = {};
  rb = {};
  a.reset();
  b.reset();
  first.open();
  second.open();
  bool released = true;
  for (const auto& probe : {first.probe, second.probe}) {
    std::unique_lock<std::mutex> lock(probe->mutex);
    released &= probe->changed.wait_for(lock, timeout,
                                        [&] { return probe->released[0]; });
  }
  PS_CHECK(started && released);
  PS_CHECK(first.probe->context.expired() && second.probe->context.expired());
  const auto deadline = Clock::now() + timeout;
  while ((!empty(a_root) || !empty(b_root)) && Clock::now() < deadline)
    std::this_thread::yield();
  PS_CHECK(empty(a_root) && empty(b_root));
  return 0;
}
int reuse_after_overflow(bool preparing) {
  Fixture fixture;
  auto context = fixture.context();
  auto root = take(context->resource_budget());
  const auto baseline = root.statistics().live[ps::ResourceKind::Host];
  std::vector<ps::PreparedPlan> prepared;
  prepared.push_back(take(context->prepare(fixture.plan)));
  const auto preparation_bytes =
      root.statistics().live[ps::ResourceKind::Host] - baseline;
  const auto preparation_peak =
      root.statistics().peak[ps::ResourceKind::Host] - baseline;
  std::uint64_t run_bytes;
  {
    auto run = take(context->submit(prepared[0]));
    const bool started = fixture.entered(1);
    run_bytes = root.statistics().peak[ps::ResourceKind::Host] - baseline -
                preparation_bytes;
    fixture.open();
    PS_REQUIRE_OK(run.join());
    PS_CHECK(started);
  }
  {
    std::lock_guard<std::mutex> lock(fixture.probe->mutex);
    fixture.probe->entered = 0;
    fixture.probe->invoke_open = false;
  }
  std::vector<ps::RunHandle> runs;
  for (unsigned i = 0; i < 4; ++i) {
    if (i)
      prepared.push_back(take(context->prepare(fixture.plan)));
    runs.push_back(take(context->submit(prepared[i])));
  }
  const bool started = fixture.entered(4);
  fixture.open();
  for (const auto& run : runs)
    PS_REQUIRE_OK(run.join());
  runs.clear();
  prepared.resize(preparing ? 0 : 1);
  const auto retained = baseline + (preparing ? 0 : preparation_bytes);
  const auto required = preparing ? preparation_peak : run_bytes;
  const auto capacity =
      ps::ResourceCapacity::host(host_limit - retained - required -
                                 ps::ResourceBudget::lease_metadata_bytes());
  ps::ResourceLease occupied;
  const auto deadline = Clock::now() + timeout;
  // Provider release can still be on a coordinator stack. Once only completed
  // overflow slots remain, there is room for this external reservation.
  do {
    auto lease = root.reserve(capacity);
    if (lease.ok())
      occupied = lease.take_value();
    else
      std::this_thread::yield();
  } while (!occupied.valid() && Clock::now() < deadline);
  bool reused = false;
  do {
    if (preparing) {
      auto next = context->prepare(fixture.plan);
      reused = next.ok();
      if (!reused)
        PS_CHECK(next.status().code == ps::ErrorCode::ResourceExhausted);
    } else {
      auto next = context->submit(prepared[0]);
      if (next.ok()) {
        PS_REQUIRE_OK(next.value().join());
        reused = true;
      } else {
        PS_CHECK(next.status().code == ps::ErrorCode::ResourceExhausted);
      }
    }
    if (!reused)
      std::this_thread::yield();
  } while (!reused && Clock::now() < deadline);
  PS_CHECK(started && occupied.valid() && reused);
  occupied = {};
  prepared.clear();
  context.reset();
  PS_CHECK(empty(root));
  return 0;
}
int thread_exit_reentry(bool preparing) {
  Fixture fixture;
  auto context = fixture.context();
  auto root = take(context->resource_budget());
  fixture.probe->exit_release = 2;
  fixture.probe->thread_exit = [&] {
    {
      std::unique_lock<std::mutex> lock(fixture.probe->mutex);
      fixture.probe->exit_entered = true;
      fixture.probe->changed.notify_all();
      if (!fixture.probe->changed.wait_for(
              lock, timeout, [&] { return fixture.probe->exit_open; }))
        std::terminate();
    }
    // One-shot serve() has returned, but this thread has not exited. Reaping
    // it while holding the Context mutex would deadlock this legal reentry.
    auto prepared = context->prepare(fixture.plan);
    if (!prepared.ok())
      std::terminate();
  };
  std::vector<ps::PreparedPlan> prepared;
  std::vector<ps::RunHandle> runs;
  for (unsigned i = 0; i < 3; ++i) {
    prepared.push_back(take(context->prepare(fixture.plan)));
    runs.push_back(take(context->submit(prepared.back())));
  }
  const bool started = fixture.entered(3);
  auto retained = prepared[0];
  prepared.clear();
  runs.clear();
  fixture.open();
  {
    std::unique_lock<std::mutex> lock(fixture.probe->mutex);
    if (!fixture.probe->changed.wait_for(
            lock, timeout, [&] { return fixture.probe->exit_entered; }))
      std::terminate();
  }
  auto admission = std::async(std::launch::async, [&] {
    if (preparing)
      return context->prepare(fixture.plan).ok();
    auto next = context->submit(retained);
    return next.ok() && next.value().join().ok();
  });
  const bool waited = admission.wait_for(std::chrono::milliseconds(30)) ==
                      std::future_status::timeout;
  {
    std::lock_guard<std::mutex> lock(fixture.probe->mutex);
    fixture.probe->exit_open = true;
  }
  fixture.probe->changed.notify_all();
  if (admission.wait_for(timeout) != std::future_status::ready) {
    std::fputs("coordinator thread-exit reentry timed out\n", stderr);
    std::terminate();
  }
  PS_CHECK(started && waited && admission.get());
  fixture.probe->thread_exit = {};
  retained = {};
  context.reset();
  PS_CHECK(empty(root));
  return 0;
}
int cross_context_thread_exit() {
  Fixture first, second;
  auto a = first.context(), b = second.context();
  auto a_root = take(a->resource_budget());
  auto b_root = take(b->resource_budget());
  auto barrier = std::make_shared<ReleaseBarrier>();
  for (auto* fixture : {&first, &second}) {
    fixture->probe->exit_release = 2;
    fixture->probe->thread_exit = [&, fixture] {
      barrier->wait();
      auto& other = fixture == &first ? second : first;
      auto& target = fixture == &first ? b : a;
      // Both one-shot threads are idle in their TLS destructors. Internal
      // admission must leave them tracked, rather than join each other.
      if (!target->prepare(other.plan).ok())
        std::terminate();
      {
        std::lock_guard<std::mutex> lock(fixture->probe->mutex);
        fixture->probe->exit_done = true;
      }
      fixture->probe->changed.notify_all();
    };
  }
  std::vector<ps::PreparedPlan> prepared;
  std::vector<ps::RunHandle> runs;
  for (unsigned i = 0; i < 3; ++i) {
    prepared.push_back(take(a->prepare(first.plan)));
    runs.push_back(take(a->submit(prepared.back())));
    prepared.push_back(take(b->prepare(second.plan)));
    runs.push_back(take(b->submit(prepared.back())));
  }
  const bool started = first.entered(3) && second.entered(3);
  prepared.clear();
  runs.clear();
  first.open();
  second.open();
  for (const auto& probe : {first.probe, second.probe}) {
    std::unique_lock<std::mutex> lock(probe->mutex);
    if (!probe->changed.wait_for(lock, timeout,
                                 [&] { return probe->exit_done; })) {
      std::fputs("cross-Context thread-exit reentry timed out\n", stderr);
      std::terminate();
    }
  }
  PS_CHECK(started);
  a.reset();
  b.reset();
  first.probe->thread_exit = second.probe->thread_exit = {};
  PS_CHECK(empty(a_root) && empty(b_root));
  return 0;
}
int retained_owners() {
  Fixture fixture;
  fixture.open();
  auto context = fixture.context();
  auto root = take(context->resource_budget());
  auto prepared = take(context->prepare(fixture.plan));
  auto run = take(context->submit(prepared));
  auto result = take(run.join());
  context.reset();
  std::uint8_t value = 0;
  const auto& output = result.results.at("value");
  check(output.read_tensor(take(output.descriptor()), 0, {0}, &value, 1));
  PS_CHECK(value == 1 && !empty(root));
  prepared = {};
  run = {};
  result = {};
  PS_CHECK(empty(root));
  return 0;
}
}  // namespace
int main() {
  for (unsigned repeat = 0; repeat < 8; ++repeat) {
    for (unsigned count : {1U, 2U, 4U})
      for (bool observe : {false, true})
        for (unsigned mode = 0; mode < (observe ? 4U : 3U); ++mode)
          PS_CHECK(immediately_reusable(count, mode, observe) == 0);
    PS_CHECK(self_release(1) == 0);
    PS_CHECK(self_release(3) == 0);
  }
  PS_CHECK(cleanup_barrier(0) == 0);
  PS_CHECK(cleanup_barrier(2) == 0);
  PS_CHECK(cross_context_release() == 0);
  PS_CHECK(reuse_after_overflow(true) == 0);
  PS_CHECK(reuse_after_overflow(false) == 0);
  PS_CHECK(thread_exit_reentry(true) == 0);
  PS_CHECK(thread_exit_reentry(false) == 0);
  PS_CHECK(cross_context_thread_exit() == 0);
  PS_CHECK(retained_owners() == 0);
  return 0;
}
