#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "scheduler/cpu_work_stealing_scheduler.hpp"
#include "scheduler/gpu_pipeline_scheduler.hpp"
#include "scheduler/scheduler_exception_test_hooks.hpp"

namespace ps::testing {
namespace {

/**
 * @brief Distinct resource-exhaustion exception used to verify identity.
 *
 * Catching this derived type after scheduler transport proves the original
 * `std::exception_ptr` was retained instead of constructing a replacement
 * `std::bad_alloc`.
 */
class TaggedBadAlloc final : public std::bad_alloc {
 public:
  /**
   * @brief Returns a stable diagnostic marker.
   *
   * @return Process-lifetime literal identifying this exact exception type.
   * @throws Nothing.
   */
  const char* what() const noexcept override {
    return "scheduler-tagged-bad-alloc";
  }
};

/**
 * @brief Distinct ordinary exception used by publication and reuse tests.
 */
class TaggedSchedulerError final : public std::runtime_error {
 public:
  /**
   * @brief Creates an ordinary exception with a stable marker.
   *
   * @throws std::bad_alloc if runtime-error message storage cannot allocate.
   */
  TaggedSchedulerError() : std::runtime_error("scheduler-tagged-error") {}
};

/**
 * @brief Lock-free barrier state owned by one deterministic publication test.
 *
 * The worker sets `arrived` after the consumer-visible flag changes and spins
 * until the test sets `release`. No allocation or mutex is used inside the
 * scheduler exception path.
 */
struct PublicationBarrier {
  /** @brief True after the publishing worker entered the test hook. */
  std::atomic<bool> arrived{false};
  /** @brief True when the worker may finish publishing/draining. */
  std::atomic<bool> release{false};
};

/**
 * @brief Barrier at the complete-worker, pre-running publication boundary.
 */
struct StartPublicationBarrier {
  /** @brief True after the complete member worker vector is installed. */
  std::atomic<bool> arrived{false};
  /** @brief True when start may release-publish public running state. */
  std::atomic<bool> release{false};
};

/**
 * @brief Barrier used inside the GPU CPU-worker predicate/wait handshake.
 *
 * The hook holds the shared RT/HP-CPU queue mutex while `release` is false.
 * This makes an HP submitter contend for the exact wait mutex and proves its
 * subsequent notify cannot be lost between predicate evaluation and sleep.
 */
struct CpuWaitBarrier {
  /** @brief True after the worker reached the pre-wait hook. */
  std::atomic<bool> arrived{false};
  /** @brief True when the worker may enter condition-variable wait. */
  std::atomic<bool> release{false};
};

/**
 * @brief Barrier inside CPU local enqueue/ready publication.
 * @note The hook holds both global predicate and target local queue mutexes.
 */
struct LocalReadyBarrier {
  /** @brief True after local enqueue but before ready-count publication. */
  std::atomic<bool> arrived{false};
  /** @brief True when enqueue may publish its matching ready count. */
  std::atomic<bool> release{false};
};

/**
 * @brief Blocks the first exception publisher at the visibility boundary.
 *
 * @param context Borrowed `PublicationBarrier` installed by the current test.
 * @return Nothing.
 * @throws Nothing.
 * @note The test always releases this spin barrier before joining workers.
 */
void block_after_flag_visible(void* context) noexcept {
  auto* barrier = static_cast<PublicationBarrier*>(context);
  barrier->arrived.store(true, std::memory_order_release);
  while (!barrier->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

/**
 * @brief Blocks exception publication before its consumer-visible flag store.
 * @param context Borrowed `PublicationBarrier` installed by the current test.
 * @return Nothing.
 * @throws Nothing.
 * @note The scheduler must retain its complete queue transaction gate here.
 */
void block_before_flag_visible(void* context) noexcept {
  auto* barrier = static_cast<PublicationBarrier*>(context);
  barrier->arrived.store(true, std::memory_order_release);
  while (!barrier->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

/**
 * @brief Blocks start after full worker installation but before public running.
 * @param context Borrowed `StartPublicationBarrier` owned by the test.
 * @return Nothing.
 * @throws Nothing.
 */
void block_before_running_visible(void* context) noexcept {
  auto* barrier = static_cast<StartPublicationBarrier*>(context);
  barrier->arrived.store(true, std::memory_order_release);
  while (!barrier->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

/**
 * @brief Blocks a GPU-pipeline CPU worker immediately before wait.
 * @param context Borrowed `CpuWaitBarrier` for the current test.
 * @return Nothing.
 * @throws Nothing.
 */
void block_before_cpu_wait(void* context) noexcept {
  auto* barrier = static_cast<CpuWaitBarrier*>(context);
  barrier->arrived.store(true, std::memory_order_release);
  while (!barrier->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

/**
 * @brief Blocks CPU local-ready publication at the reviewed race window.
 * @param context Borrowed `LocalReadyBarrier` owned by the current test.
 * @return Nothing.
 * @throws Nothing.
 */
void block_between_local_enqueue_and_ready(void* context) noexcept {
  auto* barrier = static_cast<LocalReadyBarrier*>(context);
  barrier->arrived.store(true, std::memory_order_release);
  while (!barrier->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}

/**
 * @brief Waits for an atomic test event with a bounded timeout.
 *
 * @param event Event flag published by a worker or test helper.
 * @param timeout Maximum duration to wait.
 * @return True when the event became visible before the deadline.
 * @throws Nothing.
 * @note A timeout prevents a broken scheduler from hanging CTest indefinitely.
 */
bool wait_for_event(const std::atomic<bool>& event,
                    std::chrono::milliseconds timeout) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!event.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

/**
 * @brief RAII owner for one installed scheduler publication hook.
 *
 * @note Destruction clears the global test seam; callers still release and
 * join the publishing worker before the hook storage itself goes out of scope.
 */
class ScopedPublicationHook final {
 public:
  /** @brief Scheduler-specific hook installer function type. */
  using Setter = void (*)(const SchedulerExceptionPublicationHook*) noexcept;

  /**
   * @brief Installs one borrowed publication hook for this guard lifetime.
   * @param setter Scheduler-specific hook installer retained by value.
   * @param hook Borrowed immutable hook that outlives this guard.
   * @throws Nothing.
   * @note The guard owns only the setter pointer, not hook or scheduler state.
   */
  ScopedPublicationHook(Setter setter,
                        const SchedulerExceptionPublicationHook* hook) noexcept
      : setter_(setter) {
    setter_(hook);
  }

  /**
   * @brief Clears the borrowed hook pointer without touching scheduler state.
   * @throws Nothing.
   * @note The publishing worker must already have left the hook callback.
   */
  ~ScopedPublicationHook() { setter_(nullptr); }

  ScopedPublicationHook(const ScopedPublicationHook&) = delete;
  ScopedPublicationHook& operator=(const ScopedPublicationHook&) = delete;

 private:
  /** @brief Scheduler-specific non-owning hook installation function. */
  Setter setter_;
};

/**
 * @brief RAII owner for one scheduler start-publication hook.
 */
class ScopedStartPublicationHook final {
 public:
  /** @brief Scheduler-specific start-hook installer function type. */
  using Setter = void (*)(const SchedulerStartPublicationHook*) noexcept;

  /**
   * @brief Installs one borrowed start-publication hook.
   * @param setter Scheduler-specific hook installer retained by value.
   * @param hook Borrowed hook that outlives this guard.
   * @return A guard that clears the hook at destruction.
   * @throws Nothing.
   */
  ScopedStartPublicationHook(Setter setter,
                             const SchedulerStartPublicationHook* hook) noexcept
      : setter_(setter) {
    setter_(hook);
  }

  /** @brief Clears the borrowed start hook after the start thread has joined.
   */
  ~ScopedStartPublicationHook() { setter_(nullptr); }

  ScopedStartPublicationHook(const ScopedStartPublicationHook&) = delete;
  ScopedStartPublicationHook& operator=(const ScopedStartPublicationHook&) =
      delete;

 private:
  /** @brief Scheduler-specific non-owning hook installation function. */
  Setter setter_;
};

/**
 * @brief RAII owner for the GPU-pipeline CPU-wait hook.
 * @note The worker is released before this guard is destroyed.
 */
class ScopedCpuWaitHook final {
 public:
  /**
   * @brief Installs one borrowed CPU-wait hook.
   * @param hook Hook storage that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedCpuWaitHook(const SchedulerCpuWaitHook* hook) noexcept {
    set_gpu_scheduler_cpu_wait_hook(hook);
  }

  /** @brief Clears the global BUILD_TESTING hook. */
  ~ScopedCpuWaitHook() { set_gpu_scheduler_cpu_wait_hook(nullptr); }

  ScopedCpuWaitHook(const ScopedCpuWaitHook&) = delete;
  ScopedCpuWaitHook& operator=(const ScopedCpuWaitHook&) = delete;
};

/**
 * @brief RAII owner for the CPU local-ready publication test hook.
 * @note The blocked submitter is released before guard destruction.
 */
class ScopedLocalReadyHook final {
 public:
  /**
   * @brief Installs one borrowed local-ready hook.
   * @param hook Hook storage that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedLocalReadyHook(
      const SchedulerCpuLocalReadyHook* hook) noexcept {
    set_cpu_scheduler_local_ready_hook(hook);
  }

  /** @brief Clears the global BUILD_TESTING hook. */
  ~ScopedLocalReadyHook() { set_cpu_scheduler_local_ready_hook(nullptr); }

  ScopedLocalReadyHook(const ScopedLocalReadyHook&) = delete;
  ScopedLocalReadyHook& operator=(const ScopedLocalReadyHook&) = delete;
};

/**
 * @brief Deterministic failure selection for one scheduler operation.
 */
struct FailureInjectionState {
  /** @brief Failure point selected by the current test. */
  SchedulerFailurePoint point = SchedulerFailurePoint::BatchQueuePush;
  /** @brief One-based attempt that throws. */
  std::size_t fail_on_attempt = 0;
  /** @brief Number of matching points observed. */
  std::atomic<std::size_t> matching_attempts{0};
  /** @brief True to inject a tagged system error instead of bad_alloc. */
  bool throw_system_error = false;
};

/**
 * @brief Throws the tagged allocation exception at one deterministic attempt.
 * @param context Borrowed `FailureInjectionState`.
 * @param point Scheduler mutation point about to execute.
 * @param attempt One-based point-local attempt supplied by the scheduler.
 * @throws TaggedBadAlloc or std::system_error according to test configuration.
 */
void inject_tagged_bad_alloc(void* context, SchedulerFailurePoint point,
                             std::size_t attempt) {
  auto* state = static_cast<FailureInjectionState*>(context);
  if (point != state->point) {
    return;
  }
  state->matching_attempts.fetch_add(1, std::memory_order_relaxed);
  if (attempt == state->fail_on_attempt) {
    if (state->throw_system_error) {
      throw std::system_error(
          std::make_error_code(std::errc::resource_unavailable_try_again),
          "scheduler-tagged-thread-error");
    }
    throw TaggedBadAlloc();
  }
}

/**
 * @brief RAII owner for one scheduler's throwing failure hook.
 */
class ScopedFailureHook final {
 public:
  /** @brief Scheduler-specific hook setter. */
  using Setter = void (*)(const SchedulerFailureInjectionHook*) noexcept;

  /**
   * @brief Installs a borrowed injection hook.
   * @param setter Scheduler-specific hook setter.
   * @param hook Borrowed hook that outlives this guard.
   * @throws Nothing.
   */
  ScopedFailureHook(Setter setter,
                    const SchedulerFailureInjectionHook* hook) noexcept
      : setter_(setter) {
    setter_(hook);
  }

  /** @brief Clears the hook after the injecting call has returned. */
  ~ScopedFailureHook() { setter_(nullptr); }

  ScopedFailureHook(const ScopedFailureHook&) = delete;
  ScopedFailureHook& operator=(const ScopedFailureHook&) = delete;

 private:
  /** @brief Non-owning scheduler-specific setter. */
  Setter setter_;
};

/**
 * @brief Counts executions reached through borrowed scheduler task handles.
 */
class CountingTaskExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Binds the externally owned execution counter.
   * @param executions Counter that outlives this executor.
   * @throws Nothing.
   */
  explicit CountingTaskExecutor(std::atomic<int>& executions) noexcept
      : executions_(executions) {}

  /**
   * @brief Records one borrowed-handle callback execution.
   * @param task_id Task id carried by the handle.
   * @throws Nothing.
   */
  void run_task(int task_id) override {
    (void)task_id;
    executions_.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  /** @brief Counter owned by the surrounding test. */
  std::atomic<int>& executions_;
};

/**
 * @brief Counts a handle execution and completes its scheduler batch.
 */
class CompletingTaskExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Binds the runtime completion API and external counter.
   * @param runtime Scheduler runtime that owns the active batch.
   * @param executions Counter that outlives this executor.
   * @throws Nothing.
   */
  CompletingTaskExecutor(SchedulerTaskRuntime& runtime,
                         std::atomic<int>& executions) noexcept
      : runtime_(runtime), executions_(executions) {}

  /**
   * @brief Records execution and decrements the active completion count.
   * @param task_id Dense task id carried by the borrowed handle.
   * @throws Nothing under the valid scheduler test lifecycle.
   */
  void run_task(int task_id) override {
    (void)task_id;
    executions_.fetch_add(1, std::memory_order_relaxed);
    runtime_.dec_tasks_to_complete();
  }

 private:
  /** @brief Borrowed scheduler completion API. */
  SchedulerTaskRuntime& runtime_;
  /** @brief Counter owned by the surrounding test. */
  std::atomic<int>& executions_;
};

/**
 * @brief Creates three valid handles borrowing one test executor.
 * @param executor Executor that outlives every returned handle.
 * @return Three handles with stable dense ids.
 * @throws std::bad_alloc if vector allocation fails.
 */
std::vector<TaskHandle> make_handles(TaskExecutor& executor) {
  return {{&executor, 0, 10}, {&executor, 1, 11}, {&executor, 2, 12}};
}

/** @brief Scheduler-specific throwing-hook setter function. */
using FailureHookSetter = decltype(&set_cpu_scheduler_failure_injection_hook);

/** @brief Scheduler-specific transaction snapshot reader function. */
using StateReader = decltype(&cpu_scheduler_transactional_snapshot);

/** @brief Scheduler-specific BUILD_TESTING epoch seeding function. */
using EpochSeeder = decltype(&set_cpu_scheduler_epoch_for_testing);

/**
 * @brief Compares every scheduler state field covered by strong rollback.
 * @param before Snapshot captured immediately before the injecting call.
 * @param after Snapshot captured immediately after the exception propagates.
 * @param label Stable queue-route label included in assertion traces.
 * @return Nothing.
 * @throws GTest assertions only.
 */
void expect_exact_transaction_state(
    const SchedulerTransactionalStateSnapshot& before,
    const SchedulerTransactionalStateSnapshot& after, const char* label) {
  SCOPED_TRACE(label);
  EXPECT_EQ(after.running, before.running);
  EXPECT_EQ(after.worker_loop_active, before.worker_loop_active);
  EXPECT_EQ(after.queued_tasks, before.queued_tasks);
  EXPECT_EQ(after.ready_tasks, before.ready_tasks);
  EXPECT_EQ(after.tasks_to_complete, before.tasks_to_complete);
  EXPECT_EQ(after.in_flight_tasks, before.in_flight_tasks);
  EXPECT_EQ(after.active_epoch, before.active_epoch);
  EXPECT_EQ(after.epoch_counter, before.epoch_counter);
  EXPECT_EQ(after.worker_threads, before.worker_threads);
  EXPECT_EQ(after.local_queues, before.local_queues);
  EXPECT_EQ(after.exception_claimed, before.exception_claimed);
  EXPECT_EQ(after.has_exception, before.has_exception);
  EXPECT_EQ(after.has_exception_ptr, before.has_exception_ptr);
  EXPECT_EQ(after.exception_epoch, before.exception_epoch);
  EXPECT_EQ(after.exception_cleanup_complete,
            before.exception_cleanup_complete);
}

/**
 * @brief Verifies null exception publication is an exact no-op.
 * @tparam Scheduler Concrete CPU or pipeline scheduler.
 * @param scheduler Scheduler started and stopped by this helper.
 * @param snapshot Scheduler-specific transactional state reader.
 * @param label Stable assertion trace label.
 * @return Nothing.
 * @throws GTest assertions only.
 * @note The public runtime contract requires null input to preserve queue,
 *       epoch, counter, and first-exception state.
 */
template <typename Scheduler>
void run_null_exception_noop_case(Scheduler& scheduler, StateReader snapshot,
                                  const char* label) {
  scheduler.start();
  const SchedulerTransactionalStateSnapshot before = snapshot(&scheduler);
  scheduler.set_exception(nullptr);
  const SchedulerTransactionalStateSnapshot after = snapshot(&scheduler);
  expect_exact_transaction_state(before, after, label);
  scheduler.shutdown();
}

/**
 * @brief Verifies callback/handle initial-count rejection is state preserving.
 * @tparam Scheduler Concrete CPU or pipeline scheduler.
 * @param scheduler Scheduler started and stopped by this helper.
 * @param snapshot Scheduler-specific transactional state reader.
 * @param label Stable assertion trace label.
 * @return Nothing.
 * @throws GTest assertions only.
 * @note Both routes are also exercised before start to enforce the shared
 *       running-lifecycle precondition.
 */
template <typename Scheduler>
void run_initial_count_validation_case(Scheduler& scheduler,
                                       StateReader snapshot,
                                       const char* label) {
  std::vector<SchedulerTaskRuntime::Task> stopped_callbacks;
  stopped_callbacks.emplace_back([] {});
  EXPECT_THROW(scheduler.submit_initial_tasks(std::move(stopped_callbacks), 1),
               std::logic_error);
  std::atomic<int> stopped_handle_executions{0};
  CountingTaskExecutor stopped_executor(stopped_handle_executions);
  EXPECT_THROW(
      scheduler.submit_initial_task_handles({{&stopped_executor, 0, 1}}, 1),
      std::logic_error);

  scheduler.start();
  const SchedulerTransactionalStateSnapshot before = snapshot(&scheduler);
  std::atomic<int> callback_executions{0};
  std::vector<SchedulerTaskRuntime::Task> negative_callbacks;
  negative_callbacks.emplace_back([&callback_executions] {
    callback_executions.fetch_add(1, std::memory_order_relaxed);
  });
  EXPECT_THROW(
      scheduler.submit_initial_tasks(std::move(negative_callbacks), -1),
      std::invalid_argument);
  std::vector<SchedulerTaskRuntime::Task> undercounted_callbacks;
  undercounted_callbacks.emplace_back([&callback_executions] {
    callback_executions.fetch_add(1, std::memory_order_relaxed);
  });
  EXPECT_THROW(
      scheduler.submit_initial_tasks(std::move(undercounted_callbacks), 0),
      std::invalid_argument);

  std::atomic<int> handle_executions{0};
  CountingTaskExecutor executor(handle_executions);
  EXPECT_THROW(scheduler.submit_initial_task_handles({{&executor, 0, 2}}, -1),
               std::invalid_argument);
  EXPECT_THROW(scheduler.submit_initial_task_handles({{&executor, 0, 2}}, 0),
               std::invalid_argument);

  const SchedulerTransactionalStateSnapshot after = snapshot(&scheduler);
  expect_exact_transaction_state(before, after, label);
  EXPECT_EQ(stopped_handle_executions.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(callback_executions.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(handle_executions.load(std::memory_order_relaxed), 0);
  scheduler.shutdown();
}

/**
 * @brief Verifies logical-count saturation, zero floor, and epoch wrap.
 * @tparam Scheduler Concrete CPU or pipeline scheduler.
 * @param scheduler Scheduler started and stopped by this helper.
 * @param snapshot Scheduler-specific state reader.
 * @param seed_epoch Test-only epoch boundary publisher.
 * @param label Stable assertion trace label.
 * @return Nothing.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_completion_counter_boundary_case(Scheduler& scheduler,
                                          StateReader snapshot,
                                          EpochSeeder seed_epoch,
                                          const char* label) {
  SCOPED_TRACE(label);
  scheduler.start();
  scheduler.submit_initial_tasks({}, 0, SchedulerTaskPriority::High);

  scheduler.dec_tasks_to_complete();
  scheduler.inc_tasks_to_complete(0);
  scheduler.inc_tasks_to_complete(-1);
  EXPECT_EQ(snapshot(&scheduler).tasks_to_complete, 0);

  scheduler.inc_tasks_to_complete(std::numeric_limits<int>::max());
  const SchedulerTransactionalStateSnapshot saturated = snapshot(&scheduler);
  EXPECT_EQ(saturated.tasks_to_complete, std::numeric_limits<int>::max());
  EXPECT_THROW(scheduler.inc_tasks_to_complete(1), std::overflow_error);
  EXPECT_EQ(snapshot(&scheduler).tasks_to_complete,
            std::numeric_limits<int>::max());

  scheduler.dec_tasks_to_complete();
  EXPECT_EQ(snapshot(&scheduler).tasks_to_complete,
            std::numeric_limits<int>::max() - 1LL);

  seed_epoch(&scheduler, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(scheduler.begin_new_epoch(), 1U);
  SchedulerTransactionalStateSnapshot wrapped = snapshot(&scheduler);
  EXPECT_EQ(wrapped.active_epoch, 1U);
  EXPECT_EQ(wrapped.epoch_counter, 1U);

  seed_epoch(&scheduler, std::numeric_limits<std::uint64_t>::max());
  scheduler.submit_initial_tasks({}, 0, SchedulerTaskPriority::High);
  wrapped = snapshot(&scheduler);
  EXPECT_EQ(wrapped.active_epoch, 1U);
  EXPECT_EQ(wrapped.epoch_counter, 1U);
  EXPECT_EQ(wrapped.tasks_to_complete, 0);
  scheduler.shutdown();
}

/**
 * @brief Proves an old callback cannot mutate a newly published batch count.
 * @tparam Scheduler Concrete one-worker CPU or pipeline scheduler.
 * @param scheduler Scheduler started and stopped by this helper.
 * @param snapshot Scheduler-specific state reader.
 * @param label Stable assertion trace label.
 * @return Nothing.
 * @throws GTest assertions only.
 * @note The first callback remains in flight while the second initial batch
 * publishes, directly exercising the validation/publication race boundary.
 */
template <typename Scheduler>
void run_stale_completion_counter_case(Scheduler& scheduler,
                                       StateReader snapshot,
                                       const char* label) {
  SCOPED_TRACE(label);
  std::atomic<bool> old_entered{false};
  std::atomic<bool> release_old{false};
  std::atomic<bool> old_finished{false};
  std::atomic<bool> current_entered{false};
  std::atomic<bool> release_current{false};

  scheduler.start();
  std::vector<SchedulerTaskRuntime::Task> old_batch;
  old_batch.emplace_back([&] {
    old_entered.store(true, std::memory_order_release);
    while (!release_old.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    scheduler.inc_tasks_to_complete(7);
    scheduler.dec_tasks_to_complete();
    old_finished.store(true, std::memory_order_release);
  });
  scheduler.submit_initial_tasks(std::move(old_batch), 1,
                                 SchedulerTaskPriority::High);
  const bool first_started =
      wait_for_event(old_entered, std::chrono::milliseconds(3000));

  if (first_started) {
    std::vector<SchedulerTaskRuntime::Task> current_batch;
    current_batch.emplace_back([&] {
      current_entered.store(true, std::memory_order_release);
      while (!release_current.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    });
    scheduler.submit_initial_tasks(std::move(current_batch), 2,
                                   SchedulerTaskPriority::High);
  }
  release_old.store(true, std::memory_order_release);
  const bool stale_returned =
      wait_for_event(old_finished, std::chrono::milliseconds(3000));
  const bool current_started =
      wait_for_event(current_entered, std::chrono::milliseconds(3000));
  const SchedulerTransactionalStateSnapshot after_stale = snapshot(&scheduler);
  release_current.store(true, std::memory_order_release);
  scheduler.shutdown();

  EXPECT_TRUE(first_started);
  EXPECT_TRUE(stale_returned);
  EXPECT_TRUE(current_started);
  EXPECT_EQ(after_stale.tasks_to_complete, 2);
}

/**
 * @brief Proves failed callback batches preserve every pre-call state field.
 *
 * @tparam Scheduler Concrete CPU or GPU-pipeline scheduler.
 * @param scheduler Scheduler started/stopped by this helper.
 * @param setter Scheduler-specific failure-hook setter.
 * @param snapshot Scheduler-specific state reader.
 * @param priority Initial queue route under test.
 * @param label Stable trace label.
 * @param force_gpu_route True to select the hardware-independent GPU lane.
 * @return Nothing.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_initial_callback_transaction_case(Scheduler& scheduler,
                                           FailureHookSetter setter,
                                           StateReader snapshot,
                                           SchedulerTaskPriority priority,
                                           const char* label,
                                           bool force_gpu_route = false) {
  scheduler.start();
  const SchedulerTransactionalStateSnapshot before = snapshot(&scheduler);
  std::atomic<int> failed_executions{0};
  std::vector<SchedulerTaskRuntime::Task> failed_tasks;
  for (int index = 0; index < 3; ++index) {
    failed_tasks.emplace_back(
        [&] { failed_executions.fetch_add(1, std::memory_order_relaxed); });
  }
  FailureInjectionState failure;
  failure.fail_on_attempt = 2;
  const SchedulerFailureInjectionHook hook{&failure, inject_tagged_bad_alloc};
  bool exact_exception = false;
  if (force_gpu_route) {
    set_gpu_scheduler_force_gpu_route(true);
  }
  {
    ScopedFailureHook guard(setter, &hook);
    try {
      scheduler.submit_initial_tasks(std::move(failed_tasks), 3, priority);
    } catch (const TaggedBadAlloc&) {
      exact_exception = true;
    } catch (...) {
      exact_exception = false;
    }
  }
  if (force_gpu_route) {
    set_gpu_scheduler_force_gpu_route(false);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const SchedulerTransactionalStateSnapshot after = snapshot(&scheduler);
  EXPECT_TRUE(exact_exception);
  EXPECT_EQ(failed_executions.load(std::memory_order_relaxed), 0);
  expect_exact_transaction_state(before, after, label);

  std::atomic<int> retry_executions{0};
  std::vector<SchedulerTaskRuntime::Task> retry_tasks;
  retry_tasks.emplace_back([&] {
    retry_executions.fetch_add(1, std::memory_order_relaxed);
    scheduler.dec_tasks_to_complete();
  });
  scheduler.submit_initial_tasks(std::move(retry_tasks), 1, priority);
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_EQ(retry_executions.load(std::memory_order_relaxed), 1);
  scheduler.shutdown();
  std::cout << "scheduler_callback_transaction_trace scheduler=" << label
            << " exact_exception=" << exact_exception
            << " failed_callbacks=" << failed_executions.load()
            << " retry_callbacks=" << retry_executions.load() << '\n';
}

/**
 * @brief Proves enqueue failure does not reset a consumed exception batch.
 *
 * @tparam Scheduler Concrete CPU or GPU-pipeline scheduler.
 * @param scheduler Scheduler started/stopped by this helper.
 * @param setter Scheduler-specific failure-hook setter.
 * @param snapshot Scheduler-specific state reader.
 * @param label Stable trace label.
 * @return Nothing.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_exception_state_rollback_case(Scheduler& scheduler,
                                       FailureHookSetter setter,
                                       StateReader snapshot,
                                       const char* label) {
  scheduler.start();
  std::vector<SchedulerTaskRuntime::Task> failing_batch;
  failing_batch.emplace_back([] { throw TaggedBadAlloc(); });
  scheduler.submit_initial_tasks(std::move(failing_batch), 1,
                                 SchedulerTaskPriority::High);
  EXPECT_THROW(scheduler.wait_for_completion(), TaggedBadAlloc);

  const SchedulerTransactionalStateSnapshot before = snapshot(&scheduler);
  EXPECT_TRUE(before.exception_claimed);
  EXPECT_FALSE(before.has_exception);
  EXPECT_FALSE(before.has_exception_ptr);
  EXPECT_TRUE(before.exception_cleanup_complete);
  EXPECT_EQ(before.exception_epoch, before.active_epoch);

  std::atomic<int> executions{0};
  CountingTaskExecutor executor(executions);
  FailureInjectionState failure;
  failure.fail_on_attempt = 2;
  const SchedulerFailureInjectionHook hook{&failure, inject_tagged_bad_alloc};
  bool exact_exception = false;
  {
    ScopedFailureHook guard(setter, &hook);
    try {
      scheduler.submit_initial_task_handles(make_handles(executor), 3,
                                            SchedulerTaskPriority::High);
    } catch (const TaggedBadAlloc&) {
      exact_exception = true;
    } catch (...) {
      exact_exception = false;
    }
  }
  const SchedulerTransactionalStateSnapshot after = snapshot(&scheduler);
  EXPECT_TRUE(exact_exception);
  EXPECT_EQ(executions.load(std::memory_order_relaxed), 0);
  expect_exact_transaction_state(before, after, label);

  std::atomic<int> retry_executions{0};
  CompletingTaskExecutor retry_executor(scheduler, retry_executions);
  scheduler.submit_initial_task_handles({{&retry_executor, 0, 90}}, 1,
                                        SchedulerTaskPriority::High);
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_EQ(retry_executions.load(std::memory_order_relaxed), 1);
  scheduler.shutdown();
  std::cout << "scheduler_exception_state_rollback_trace scheduler=" << label
            << " exact_exception=" << exact_exception << " claimed_preserved="
            << (after.exception_claimed == before.exception_claimed)
            << " exception_epoch_preserved="
            << (after.exception_epoch == before.exception_epoch) << '\n';
}

/**
 * @brief Proves a publisher cannot write exception state after a new commit.
 *
 * @tparam Scheduler Concrete CPU or GPU-pipeline scheduler.
 * @param scheduler Scheduler started/stopped by this helper.
 * @param setter Scheduler-specific exception-hook setter.
 * @param snapshot Scheduler-specific state reader.
 * @param label Stable trace label.
 * @return Nothing.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_cross_epoch_publication_gate_case(Scheduler& scheduler,
                                           ScopedPublicationHook::Setter setter,
                                           StateReader snapshot,
                                           const char* label) {
  PublicationBarrier barrier;
  const SchedulerExceptionPublicationHook hook{&barrier, nullptr,
                                               block_before_flag_visible};
  ScopedPublicationHook hook_guard(setter, &hook);
  scheduler.start();

  std::vector<SchedulerTaskRuntime::Task> first_batch;
  first_batch.emplace_back([] { throw TaggedBadAlloc(); });
  scheduler.submit_initial_tasks(std::move(first_batch), 1,
                                 SchedulerTaskPriority::High);
  const bool publisher_blocked =
      wait_for_event(barrier.arrived, std::chrono::milliseconds(3000));

  std::atomic<bool> submit_started{false};
  std::atomic<bool> submit_returned{false};
  std::atomic<int> second_completed{0};
  std::thread submitter([&] {
    submit_started.store(true, std::memory_order_release);
    std::vector<SchedulerTaskRuntime::Task> second_batch;
    second_batch.emplace_back([&] {
      second_completed.fetch_add(1, std::memory_order_relaxed);
      scheduler.dec_tasks_to_complete();
    });
    scheduler.submit_initial_tasks(std::move(second_batch), 1,
                                   SchedulerTaskPriority::High);
    submit_returned.store(true, std::memory_order_release);
  });
  const bool submitter_started =
      wait_for_event(submit_started, std::chrono::milliseconds(3000));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool returned_before_release =
      submit_returned.load(std::memory_order_acquire);
  barrier.release.store(true, std::memory_order_release);
  const bool returned_after_release =
      wait_for_event(submit_returned, std::chrono::milliseconds(3000));
  if (!returned_after_release) {
    scheduler.shutdown();
  }
  submitter.join();
  if (returned_after_release) {
    EXPECT_NO_THROW(scheduler.wait_for_completion());
  }
  const SchedulerTransactionalStateSnapshot settled = snapshot(&scheduler);
  scheduler.shutdown();

  EXPECT_TRUE(publisher_blocked);
  EXPECT_TRUE(submitter_started);
  EXPECT_FALSE(returned_before_release);
  EXPECT_TRUE(returned_after_release);
  EXPECT_EQ(second_completed.load(std::memory_order_relaxed), 1);
  EXPECT_FALSE(settled.exception_claimed);
  EXPECT_FALSE(settled.has_exception);
  EXPECT_FALSE(settled.has_exception_ptr);
  EXPECT_EQ(settled.exception_epoch, 0u);
  EXPECT_FALSE(settled.exception_cleanup_complete);
  std::cout << "scheduler_cross_epoch_gate_trace scheduler=" << label
            << " publisher_blocked=" << publisher_blocked
            << " submitter_blocked=" << !returned_before_release
            << " second_completed=" << second_completed.load()
            << " exception_claimed=" << settled.exception_claimed << '\n';
}

/**
 * @brief Proves public running remains false until all workers are installed.
 *
 * @tparam Scheduler Concrete CPU or GPU-pipeline scheduler.
 * @param scheduler Scheduler started/stopped by this helper.
 * @param setter Scheduler-specific start-hook setter.
 * @param snapshot Scheduler-specific state reader.
 * @param expected_workers Complete configured worker count.
 * @param label Stable trace label.
 * @return Nothing.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_start_publication_case(Scheduler& scheduler,
                                ScopedStartPublicationHook::Setter setter,
                                StateReader snapshot,
                                std::size_t expected_workers,
                                const char* label) {
  StartPublicationBarrier barrier;
  const SchedulerStartPublicationHook hook{&barrier,
                                           block_before_running_visible};
  ScopedStartPublicationHook hook_guard(setter, &hook);
  std::exception_ptr start_error;
  std::thread starter([&] {
    try {
      scheduler.start();
    } catch (...) {
      start_error = std::current_exception();
    }
  });
  const bool reached =
      wait_for_event(barrier.arrived, std::chrono::milliseconds(3000));
  const SchedulerTransactionalStateSnapshot staged =
      reached ? snapshot(&scheduler) : SchedulerTransactionalStateSnapshot{};
  const bool public_running_at_barrier = scheduler.is_running();
  const bool runtime_running_at_barrier = scheduler.is_running();
  barrier.release.store(true, std::memory_order_release);
  starter.join();

  const SchedulerTransactionalStateSnapshot committed = snapshot(&scheduler);
  EXPECT_TRUE(reached);
  EXPECT_FALSE(public_running_at_barrier);
  EXPECT_FALSE(runtime_running_at_barrier);
  EXPECT_FALSE(staged.running);
  EXPECT_TRUE(staged.worker_loop_active);
  EXPECT_EQ(staged.worker_threads, expected_workers);
  EXPECT_FALSE(static_cast<bool>(start_error));
  EXPECT_TRUE(committed.running);
  EXPECT_TRUE(committed.worker_loop_active);
  EXPECT_EQ(committed.worker_threads, expected_workers);

  std::atomic<int> completed{0};
  std::vector<SchedulerTaskRuntime::Task> tasks;
  tasks.emplace_back([&] {
    completed.fetch_add(1, std::memory_order_relaxed);
    scheduler.dec_tasks_to_complete();
  });
  scheduler.submit_initial_tasks(std::move(tasks), 1);
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);
  scheduler.shutdown();
  std::cout << "scheduler_start_publication_trace scheduler=" << label
            << " running_at_barrier=" << public_running_at_barrier
            << " staged_workers=" << staged.worker_threads
            << " committed_running=" << committed.running << '\n';
}

/**
 * @brief Exercises repeated initial-handle rollback and immediate reuse.
 *
 * @tparam Scheduler Concrete built-in scheduler.
 * @param scheduler Scheduler started and stopped by this helper.
 * @param setter Scheduler-specific failure hook setter.
 * @param snapshot Scheduler-specific transactional state reader.
 * @param priority Queue route under test.
 * @param label Stable trace label.
 * @param force_gpu_route True only for the hardware-independent GPU lane case.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_initial_handle_transaction_case(Scheduler& scheduler,
                                         FailureHookSetter setter,
                                         StateReader snapshot,
                                         SchedulerTaskPriority priority,
                                         const char* label,
                                         bool force_gpu_route = false) {
  constexpr int kRounds = 5;
  scheduler.start();
  for (int round = 0; round < kRounds; ++round) {
    const SchedulerTransactionalStateSnapshot before = snapshot(&scheduler);
    std::atomic<int> failed_executions{0};
    bool exact_exception = false;
    {
      CountingTaskExecutor failed_executor(failed_executions);
      FailureInjectionState failure;
      failure.fail_on_attempt = 2;
      const SchedulerFailureInjectionHook hook{&failure,
                                               inject_tagged_bad_alloc};
      if (force_gpu_route) {
        set_gpu_scheduler_force_gpu_route(true);
      }
      {
        ScopedFailureHook guard(setter, &hook);
        try {
          scheduler.submit_initial_task_handles(make_handles(failed_executor),
                                                3, priority);
        } catch (const TaggedBadAlloc&) {
          exact_exception = true;
        } catch (...) {
          exact_exception = false;
        }
      }
      if (force_gpu_route) {
        set_gpu_scheduler_force_gpu_route(false);
      }
      EXPECT_EQ(failure.matching_attempts.load(), 2u);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const SchedulerTransactionalStateSnapshot after = snapshot(&scheduler);
    EXPECT_TRUE(exact_exception);
    EXPECT_EQ(failed_executions.load(), 0);
    EXPECT_EQ(after.queued_tasks, before.queued_tasks);
    EXPECT_EQ(after.ready_tasks, before.ready_tasks);
    EXPECT_EQ(after.tasks_to_complete, before.tasks_to_complete);
    EXPECT_EQ(after.in_flight_tasks, before.in_flight_tasks);
    EXPECT_EQ(after.active_epoch, before.active_epoch);
    EXPECT_EQ(after.worker_threads, before.worker_threads);

    std::atomic<int> retry_executions{0};
    CompletingTaskExecutor retry_executor(scheduler, retry_executions);
    scheduler.submit_initial_task_handles({{&retry_executor, 0, 20}}, 1,
                                          priority);
    EXPECT_NO_THROW(scheduler.wait_for_completion());
    EXPECT_EQ(retry_executions.load(), 1);
    std::cout << "scheduler_batch_transaction_trace scheduler=" << label
              << " round=" << round << " exact_exception=" << exact_exception
              << " failed_callbacks=" << failed_executions.load()
              << " queued_after_failure=" << after.queued_tasks
              << " ready_after_failure=" << after.ready_tasks
              << " retry_callbacks=" << retry_executions.load() << '\n';
  }
  scheduler.shutdown();
}

/**
 * @brief Exercises ready-handle batch rollback without starting a new epoch.
 *
 * @tparam Scheduler Concrete built-in scheduler.
 * @param scheduler Scheduler started and stopped by this helper.
 * @param setter Scheduler-specific failure hook setter.
 * @param snapshot Scheduler-specific transactional state reader.
 * @param priority Queue route under test.
 * @param label Stable trace label.
 * @param force_gpu_route True only for the GPU queue test lane.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_ready_handle_transaction_case(Scheduler& scheduler,
                                       FailureHookSetter setter,
                                       StateReader snapshot,
                                       SchedulerTaskPriority priority,
                                       const char* label,
                                       bool force_gpu_route = false) {
  constexpr int kRounds = 3;
  scheduler.start();
  for (int round = 0; round < kRounds; ++round) {
    scheduler.submit_initial_tasks({}, 0);
    const SchedulerTransactionalStateSnapshot before = snapshot(&scheduler);
    std::atomic<int> failed_executions{0};
    bool exact_exception = false;
    {
      CountingTaskExecutor failed_executor(failed_executions);
      FailureInjectionState failure;
      failure.fail_on_attempt = 2;
      const SchedulerFailureInjectionHook hook{&failure,
                                               inject_tagged_bad_alloc};
      if (force_gpu_route) {
        set_gpu_scheduler_force_gpu_route(true);
      }
      {
        ScopedFailureHook guard(setter, &hook);
        try {
          scheduler.submit_ready_task_handles_any_thread(
              make_handles(failed_executor), priority,
              scheduler.active_epoch());
        } catch (const TaggedBadAlloc&) {
          exact_exception = true;
        } catch (...) {
          exact_exception = false;
        }
      }
      if (force_gpu_route) {
        set_gpu_scheduler_force_gpu_route(false);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const SchedulerTransactionalStateSnapshot after = snapshot(&scheduler);
    EXPECT_TRUE(exact_exception);
    EXPECT_EQ(failed_executions.load(), 0);
    EXPECT_EQ(after.queued_tasks, before.queued_tasks);
    EXPECT_EQ(after.ready_tasks, before.ready_tasks);
    EXPECT_EQ(after.tasks_to_complete, before.tasks_to_complete);
    EXPECT_EQ(after.active_epoch, before.active_epoch);

    std::atomic<int> retry_executions{0};
    CompletingTaskExecutor retry_executor(scheduler, retry_executions);
    scheduler.submit_initial_task_handles({{&retry_executor, 0, 30}}, 1,
                                          priority);
    EXPECT_NO_THROW(scheduler.wait_for_completion());
    EXPECT_EQ(retry_executions.load(), 1);
    std::cout << "scheduler_ready_transaction_trace scheduler=" << label
              << " round=" << round << " exact_exception=" << exact_exception
              << " failed_callbacks=" << failed_executions.load()
              << " retry_callbacks=" << retry_executions.load() << '\n';
  }
  scheduler.shutdown();
}

/**
 * @brief Exercises the worker dependency-release batch used by dirty dispatch.
 *
 * @tparam Scheduler Concrete built-in scheduler.
 * @param scheduler Scheduler started and stopped by this helper.
 * @param setter Scheduler-specific failure hook setter.
 * @param snapshot Scheduler-specific transactional state reader.
 * @param label Stable trace label.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_worker_release_transaction_case(Scheduler& scheduler,
                                         FailureHookSetter setter,
                                         StateReader snapshot,
                                         const char* label) {
  scheduler.start();
  std::atomic<int> released_executions{0};
  bool exact_exception = false;
  {
    CountingTaskExecutor released_executor(released_executions);
    FailureInjectionState failure;
    failure.fail_on_attempt = 2;
    const SchedulerFailureInjectionHook hook{&failure, inject_tagged_bad_alloc};
    std::vector<SchedulerTaskRuntime::Task> initial;
    initial.emplace_back([&] {
      ScopedFailureHook guard(setter, &hook);
      scheduler.submit_ready_task_handles_from_worker(
          make_handles(released_executor), SchedulerTaskPriority::Normal);
      scheduler.dec_tasks_to_complete();
    });
    scheduler.submit_initial_tasks(std::move(initial), 1,
                                   SchedulerTaskPriority::Normal);
    try {
      scheduler.wait_for_completion();
    } catch (const TaggedBadAlloc&) {
      exact_exception = true;
    } catch (...) {
      exact_exception = false;
    }
  }
  const SchedulerTransactionalStateSnapshot after = snapshot(&scheduler);
  EXPECT_TRUE(exact_exception);
  EXPECT_EQ(released_executions.load(), 0);
  EXPECT_EQ(after.queued_tasks, 0u);
  EXPECT_EQ(after.ready_tasks, 0);
  EXPECT_EQ(after.in_flight_tasks, 0u);

  std::atomic<int> retry_executions{0};
  CompletingTaskExecutor retry_executor(scheduler, retry_executions);
  scheduler.submit_initial_task_handles({{&retry_executor, 0, 40}}, 1);
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_EQ(retry_executions.load(), 1);
  scheduler.shutdown();
  std::cout << "scheduler_worker_release_transaction_trace scheduler=" << label
            << " exact_exception=" << exact_exception
            << " released_callbacks=" << released_executions.load()
            << " retry_callbacks=" << retry_executions.load() << '\n';
}

/**
 * @brief Injects one start failure, audits rollback, then retries real work.
 *
 * @tparam Scheduler Concrete built-in scheduler.
 * @param scheduler Fresh stopped scheduler owned by the caller.
 * @param setter Scheduler-specific failure hook setter.
 * @param snapshot Scheduler-specific transactional state reader.
 * @param point Start lifecycle point that throws.
 * @param fail_on_attempt One-based resource/thread attempt to fail.
 * @param system_error True for system_error, false for tagged bad_alloc.
 * @param label Stable trace label.
 * @throws GTest assertions only.
 */
template <typename Scheduler>
void run_start_failure_case(Scheduler& scheduler, FailureHookSetter setter,
                            StateReader snapshot, SchedulerFailurePoint point,
                            std::size_t fail_on_attempt, bool system_error,
                            const char* label) {
  FailureInjectionState failure;
  failure.point = point;
  failure.fail_on_attempt = fail_on_attempt;
  failure.throw_system_error = system_error;
  const SchedulerFailureInjectionHook hook{&failure, inject_tagged_bad_alloc};
  bool exact_exception = false;
  {
    ScopedFailureHook guard(setter, &hook);
    try {
      scheduler.start();
    } catch (const TaggedBadAlloc&) {
      exact_exception = !system_error;
    } catch (const std::system_error& error) {
      exact_exception =
          system_error &&
          std::string(error.what()).find("scheduler-tagged-thread-error") !=
              std::string::npos;
    } catch (...) {
      exact_exception = false;
    }
  }

  const SchedulerTransactionalStateSnapshot rolled_back = snapshot(&scheduler);
  EXPECT_TRUE(exact_exception);
  EXPECT_FALSE(rolled_back.running);
  EXPECT_FALSE(rolled_back.worker_loop_active);
  EXPECT_EQ(rolled_back.worker_threads, 0u);
  EXPECT_EQ(rolled_back.local_queues, 0u);
  EXPECT_EQ(rolled_back.queued_tasks, 0u);
  EXPECT_EQ(rolled_back.ready_tasks, 0);
  EXPECT_EQ(rolled_back.tasks_to_complete, 0);
  EXPECT_EQ(rolled_back.in_flight_tasks, 0u);
  EXPECT_EQ(rolled_back.active_epoch, 0u);
  EXPECT_EQ(rolled_back.epoch_counter, 0u);
  EXPECT_FALSE(rolled_back.exception_claimed);
  EXPECT_FALSE(rolled_back.has_exception);
  EXPECT_FALSE(rolled_back.has_exception_ptr);
  EXPECT_EQ(rolled_back.exception_epoch, 0u);
  EXPECT_FALSE(rolled_back.exception_cleanup_complete);
  EXPECT_NO_THROW(scheduler.shutdown());
  EXPECT_NO_THROW(scheduler.shutdown());

  scheduler.start();
  std::atomic<int> completed{0};
  std::vector<SchedulerTaskRuntime::Task> tasks;
  tasks.emplace_back([&] {
    completed.fetch_add(1, std::memory_order_relaxed);
    scheduler.dec_tasks_to_complete();
  });
  scheduler.submit_initial_tasks(std::move(tasks), 1);
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_EQ(completed.load(), 1);
  scheduler.shutdown();
  std::cout << "scheduler_start_rollback_trace scheduler=" << label
            << " point=" << static_cast<int>(point)
            << " fail_attempt=" << fail_on_attempt << " exception_kind="
            << (system_error ? "system_error" : "bad_alloc")
            << " exact_exception=" << exact_exception
            << " running_after_failure=" << rolled_back.running
            << " workers_after_failure=" << rolled_back.worker_threads
            << " queues_after_failure=" << rolled_back.queued_tasks
            << " retry_completed=" << completed.load() << '\n';
}

/** @brief Pointer to a scheduler-specific publication snapshot reader. */
using PublicationSnapshotReader =
    decltype(&cpu_scheduler_exception_publication_snapshot);  // NOLINT

/**
 * @brief Runs one deterministic publication-window propagation scenario.
 *
 * @tparam Scheduler Concrete CPU or GPU-pipeline scheduler type.
 * @param scheduler Started/stopped by this helper and otherwise test-owned.
 * @param setter Scheduler-specific BUILD_TESTING hook installer.
 * @param snapshot Scheduler-specific mutex-safe state inspection function.
 * @param label Stable scheduler/path label emitted into runtime evidence.
 * @param throw_bad_alloc True for `TaggedBadAlloc`, false for ordinary error.
 * @return Nothing.
 * @throws GTest assertions only; scheduler exceptions are caught and audited.
 * @note The state snapshot is taken while the worker is blocked immediately
 * after flag visibility. The task then resumes and the real
 * `wait_for_completion()` path must propagate the exact original exception.
 */
template <typename Scheduler>
void run_publication_case(Scheduler& scheduler,
                          ScopedPublicationHook::Setter setter,
                          PublicationSnapshotReader snapshot, const char* label,
                          bool throw_bad_alloc) {
  PublicationBarrier barrier;
  const SchedulerExceptionPublicationHook hook{&barrier,
                                               block_after_flag_visible};
  ScopedPublicationHook hook_guard(setter, &hook);

  scheduler.start();
  std::vector<SchedulerTaskRuntime::Task> tasks;
  if (throw_bad_alloc) {
    tasks.emplace_back([] { throw TaggedBadAlloc(); });
  } else {
    tasks.emplace_back([] { throw TaggedSchedulerError(); });
  }
  scheduler.submit_initial_tasks(std::move(tasks), 1);

  const bool reached =
      wait_for_event(barrier.arrived, std::chrono::milliseconds(3000));
  SchedulerExceptionPublicationSnapshot observed;
  if (reached) {
    observed = snapshot(&scheduler);
  }
  barrier.release.store(true, std::memory_order_release);

  bool exact_exception = false;
  std::string message;
  if (reached) {
    try {
      scheduler.wait_for_completion();
    } catch (const TaggedBadAlloc& error) {
      exact_exception = throw_bad_alloc;
      message = error.what();
    } catch (const TaggedSchedulerError& error) {
      exact_exception = !throw_bad_alloc;
      message = error.what();
    } catch (const std::exception& error) {
      message = error.what();
    } catch (...) {
      message = "non-standard exception";
    }
  }
  scheduler.shutdown();

  EXPECT_TRUE(reached) << "publisher did not reach deterministic barrier";
  EXPECT_TRUE(observed.has_exception);
  EXPECT_TRUE(observed.has_exception_ptr)
      << "consumer-visible flag preceded first_exception_ publication";
  EXPECT_TRUE(observed.cleanup_complete)
      << "consumer-visible flag preceded queue cleanup";
  EXPECT_EQ(observed.ready_tasks, 0)
      << "consumer-visible flag preceded ready-predicate cleanup";
  EXPECT_EQ(observed.exception_epoch, scheduler.active_epoch());
  EXPECT_TRUE(exact_exception) << message;
  EXPECT_EQ(message, throw_bad_alloc ? "scheduler-tagged-bad-alloc"
                                     : "scheduler-tagged-error");
  std::cout << "scheduler_exception_publication_trace scheduler=" << label
            << " kind=" << (throw_bad_alloc ? "bad_alloc" : "ordinary")
            << " flag=" << observed.has_exception
            << " pointer=" << observed.has_exception_ptr
            << " exact_identity=" << exact_exception << " message=" << message
            << '\n';
}

/**
 * @brief Proves an exception batch settles all old callbacks before returning.
 *
 * @tparam Scheduler Concrete CPU or GPU-pipeline scheduler.
 * @param scheduler Two-worker scheduler owned by the caller.
 * @param setter Scheduler-specific publication hook installer.
 * @param snapshot Scheduler-specific synchronized state reader.
 * @param label Stable trace label.
 * @return Nothing.
 * @throws GTest assertions only; worker exceptions are audited locally.
 * @note One old task publishes and blocks after visibility while another old
 * task remains live and later throws. The waiter must remain blocked until both
 * callbacks settle; only then may a clean second batch start.
 */
template <typename Scheduler>
void run_batch_settlement_case(Scheduler& scheduler,
                               ScopedPublicationHook::Setter setter,
                               PublicationSnapshotReader snapshot,
                               const char* label) {
  PublicationBarrier publication_barrier;
  const SchedulerExceptionPublicationHook hook{&publication_barrier,
                                               block_after_flag_visible};
  ScopedPublicationHook hook_guard(setter, &hook);
  std::atomic<bool> late_started{false};
  std::atomic<bool> release_late{false};
  std::atomic<bool> waiter_returned{false};
  bool exact_first_exception = false;

  scheduler.start();
  std::vector<SchedulerTaskRuntime::Task> tasks;
  tasks.emplace_back([&] {
    late_started.store(true, std::memory_order_release);
    while (!release_late.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    throw TaggedSchedulerError();
  });
  tasks.emplace_back([&] {
    while (!late_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    throw TaggedBadAlloc();
  });
  scheduler.submit_initial_tasks(std::move(tasks), 2);

  const bool publisher_visible = wait_for_event(
      publication_barrier.arrived, std::chrono::milliseconds(3000));
  const SchedulerExceptionPublicationSnapshot observed =
      publisher_visible ? snapshot(&scheduler)
                        : SchedulerExceptionPublicationSnapshot{};

  std::thread waiter([&] {
    try {
      scheduler.wait_for_completion();
    } catch (const TaggedBadAlloc&) {
      exact_first_exception = true;
    } catch (...) {
      exact_first_exception = false;
    }
    waiter_returned.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool returned_while_publisher_blocked =
      waiter_returned.load(std::memory_order_acquire);
  publication_barrier.release.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool returned_while_late_task_running =
      waiter_returned.load(std::memory_order_acquire);
  release_late.store(true, std::memory_order_release);
  const bool returned_after_settlement =
      wait_for_event(waiter_returned, std::chrono::milliseconds(3000));
  if (!returned_after_settlement) {
    scheduler.shutdown();
  }
  waiter.join();
  const SchedulerExceptionPublicationSnapshot settled = snapshot(&scheduler);

  std::atomic<int> second_batch_completed{0};
  if (returned_after_settlement) {
    std::vector<SchedulerTaskRuntime::Task> second_batch;
    second_batch.emplace_back([&] {
      second_batch_completed.fetch_add(1, std::memory_order_relaxed);
      scheduler.dec_tasks_to_complete();
    });
    scheduler.submit_initial_tasks(std::move(second_batch), 1);
    EXPECT_NO_THROW(scheduler.wait_for_completion());
  }
  scheduler.shutdown();

  EXPECT_TRUE(publisher_visible);
  EXPECT_TRUE(observed.cleanup_complete);
  EXPECT_EQ(observed.ready_tasks, 0);
  EXPECT_GE(observed.in_flight_tasks, 1u);
  EXPECT_FALSE(returned_while_publisher_blocked);
  EXPECT_FALSE(returned_while_late_task_running);
  EXPECT_TRUE(returned_after_settlement);
  EXPECT_TRUE(exact_first_exception);
  EXPECT_EQ(settled.in_flight_tasks, 0u);
  EXPECT_EQ(settled.ready_tasks, 0);
  EXPECT_EQ(second_batch_completed.load(std::memory_order_relaxed), 1);
  std::cout << "scheduler_batch_fence_trace scheduler=" << label
            << " cleanup_before_flag=" << observed.cleanup_complete
            << " in_flight_at_flag=" << observed.in_flight_tasks
            << " ready_at_flag=" << observed.ready_tasks
            << " ready_after_settlement=" << settled.ready_tasks
            << " returned_while_publisher_blocked="
            << returned_while_publisher_blocked
            << " returned_while_late_running="
            << returned_while_late_task_running
            << " exact_first_exception=" << exact_first_exception
            << " second_batch_completed=" << second_batch_completed.load()
            << '\n';
}

/**
 * @brief Reuses one scheduler across many worker and concurrent-publisher
 * errors.
 *
 * @tparam Scheduler Concrete scheduler under stress.
 * @param scheduler Started/stopped by this helper.
 * @param label Stable trace label written to stdout evidence.
 * @return Nothing.
 * @throws GTest assertions only; transported exceptions are consumed locally.
 * @note Worker batches alternate exact bad-alloc/ordinary identities. Separate
 * direct concurrent publishers prove first-exception selection never exposes a
 * null pointer, and a final successful batch proves reset/reuse consistency.
 */
template <typename Scheduler>
void run_reuse_and_concurrent_stress(Scheduler& scheduler, const char* label) {
  constexpr int kWorkerBatches = 100;
  constexpr int kConcurrentBatches = 50;
  constexpr int kPublishers = 8;
  scheduler.start();

  int worker_propagated = 0;
  for (int iteration = 0; iteration < kWorkerBatches; ++iteration) {
    std::vector<SchedulerTaskRuntime::Task> tasks;
    if (iteration % 2 == 0) {
      tasks.emplace_back([] { throw TaggedBadAlloc(); });
    } else {
      tasks.emplace_back([] { throw TaggedSchedulerError(); });
    }
    scheduler.submit_initial_tasks(std::move(tasks), 1);
    try {
      scheduler.wait_for_completion();
    } catch (const TaggedBadAlloc&) {
      EXPECT_EQ(iteration % 2, 0);
      ++worker_propagated;
    } catch (const TaggedSchedulerError&) {
      EXPECT_EQ(iteration % 2, 1);
      ++worker_propagated;
    } catch (...) {
      ADD_FAILURE() << "worker batch lost original exception identity";
    }
  }

  int concurrent_propagated = 0;
  for (int iteration = 0; iteration < kConcurrentBatches; ++iteration) {
    scheduler.submit_initial_tasks({}, 0);
    std::vector<std::thread> publishers;
    publishers.reserve(kPublishers);
    for (int publisher = 0; publisher < kPublishers; ++publisher) {
      publishers.emplace_back([&scheduler, iteration, publisher] {
        scheduler.set_exception(std::make_exception_ptr(
            std::runtime_error("concurrent-" + std::to_string(iteration) + "-" +
                               std::to_string(publisher))));
      });
    }
    for (auto& publisher : publishers) {
      publisher.join();
    }
    try {
      scheduler.wait_for_completion();
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find("concurrent-"),
                std::string::npos);
      ++concurrent_propagated;
    } catch (...) {
      ADD_FAILURE() << "concurrent batch exposed null/replacement exception";
    }
  }

  std::atomic<int> completed{0};
  std::vector<SchedulerTaskRuntime::Task> final_tasks;
  final_tasks.emplace_back([&scheduler, &completed] {
    completed.fetch_add(1, std::memory_order_relaxed);
    scheduler.dec_tasks_to_complete();
  });
  scheduler.submit_initial_tasks(std::move(final_tasks), 1);
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);
  scheduler.shutdown();

  EXPECT_EQ(worker_propagated, kWorkerBatches);
  EXPECT_EQ(concurrent_propagated, kConcurrentBatches);
  std::cout << "scheduler_exception_stress scheduler=" << label
            << " worker_batches=" << worker_propagated
            << " concurrent_batches=" << concurrent_propagated
            << " publishers_per_batch=" << kPublishers
            << " final_reuse_completed=" << completed.load() << '\n';
}

/** @brief Verifies CPU publication preserves tagged bad_alloc identity. */
TEST(SchedulerExceptionPublication, CpuPreservesBadAllocAtVisibilityBarrier) {
  CpuWorkStealingScheduler scheduler(1);
  run_publication_case(scheduler, set_cpu_scheduler_exception_publication_hook,
                       cpu_scheduler_exception_publication_snapshot, "cpu",
                       true);
}

/** @brief Verifies CPU publication preserves an ordinary exception identity. */
TEST(SchedulerExceptionPublication,
     CpuPreservesOrdinaryExceptionAtVisibilityBarrier) {
  CpuWorkStealingScheduler scheduler(1);
  run_publication_case(scheduler, set_cpu_scheduler_exception_publication_hook,
                       cpu_scheduler_exception_publication_snapshot, "cpu",
                       false);
}

/** @brief Verifies the GPU pipeline CPU path preserves tagged bad_alloc. */
TEST(SchedulerExceptionPublication,
     GpuPipelinePreservesBadAllocAtVisibilityBarrier) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_publication_case(scheduler, set_gpu_scheduler_exception_publication_hook,
                       gpu_scheduler_exception_publication_snapshot,
                       "gpu_pipeline_cpu_path", true);
}

/** @brief Verifies the GPU pipeline preserves an ordinary exception. */
TEST(SchedulerExceptionPublication,
     GpuPipelinePreservesOrdinaryExceptionAtVisibilityBarrier) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_publication_case(scheduler, set_gpu_scheduler_exception_publication_hook,
                       gpu_scheduler_exception_publication_snapshot,
                       "gpu_pipeline_cpu_path", false);
}

/** @brief Stresses CPU batch reset and concurrent first-publisher selection. */
TEST(SchedulerExceptionPublication, CpuReuseAndConcurrentStressTrace) {
  CpuWorkStealingScheduler scheduler(4);
  run_reuse_and_concurrent_stress(scheduler, "cpu_work_stealing");
}

/** @brief Stresses GPU-pipeline reset and concurrent publisher selection. */
TEST(SchedulerExceptionPublication, GpuPipelineReuseAndConcurrentStressTrace) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_reuse_and_concurrent_stress(scheduler, "gpu_pipeline_cpu_path");
}

/** @brief Proves CPU wait fences all old callbacks before immediate reuse. */
TEST(SchedulerExceptionPublication, CpuWaitsForOldBatchSettlementBeforeReuse) {
  CpuWorkStealingScheduler scheduler(2);
  run_batch_settlement_case(
      scheduler, set_cpu_scheduler_exception_publication_hook,
      cpu_scheduler_exception_publication_snapshot, "cpu_work_stealing");
}

/** @brief Proves GPU-pipeline wait fences old callbacks before reuse. */
TEST(SchedulerExceptionPublication,
     GpuPipelineWaitsForOldBatchSettlementBeforeReuse) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_batch_settlement_case(
      scheduler, set_gpu_scheduler_exception_publication_hook,
      gpu_scheduler_exception_publication_snapshot, "gpu_pipeline_cpu_path");
}

/**
 * @brief Proves exception cleanup cannot split local enqueue/ready publication.
 * @throws Nothing when cleanup leaves no stale ready predicate for reuse.
 * @note The publisher claims while the submitter holds global/local locks at
 * the exact post-enqueue, pre-ready-count window.
 */
TEST(SchedulerExceptionPublication,
     CpuCleanupCannotOvertakeLocalReadyPublication) {
  CpuWorkStealingScheduler scheduler(1);
  LocalReadyBarrier barrier;
  const SchedulerCpuLocalReadyHook hook{&barrier,
                                        block_between_local_enqueue_and_ready};
  ScopedLocalReadyHook hook_guard(&hook);
  std::atomic<bool> publisher_started{false};
  std::atomic<bool> publisher_returned{false};
  std::atomic<int> executed{0};

  scheduler.start();
  std::thread submitter([&] {
    std::vector<SchedulerTaskRuntime::Task> tasks;
    tasks.emplace_back([&] {
      executed.fetch_add(1, std::memory_order_relaxed);
      scheduler.dec_tasks_to_complete();
    });
    scheduler.submit_initial_tasks(std::move(tasks), 1);
  });

  const bool reached_window =
      wait_for_event(barrier.arrived, std::chrono::milliseconds(3000));
  std::thread publisher([&] {
    publisher_started.store(true, std::memory_order_release);
    scheduler.set_exception(std::make_exception_ptr(TaggedSchedulerError()));
    publisher_returned.store(true, std::memory_order_release);
  });
  const bool publisher_entered =
      wait_for_event(publisher_started, std::chrono::milliseconds(3000));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool cleanup_overtook_publication =
      publisher_returned.load(std::memory_order_acquire);
  barrier.release.store(true, std::memory_order_release);
  submitter.join();
  publisher.join();

  bool exact_exception = false;
  try {
    scheduler.wait_for_completion();
  } catch (const TaggedSchedulerError&) {
    exact_exception = true;
  } catch (...) {
    exact_exception = false;
  }
  const SchedulerExceptionPublicationSnapshot settled =
      cpu_scheduler_exception_publication_snapshot(&scheduler);
  scheduler.shutdown();

  EXPECT_TRUE(reached_window);
  EXPECT_TRUE(publisher_entered);
  EXPECT_FALSE(cleanup_overtook_publication);
  EXPECT_TRUE(exact_exception);
  EXPECT_EQ(settled.in_flight_tasks, 0u);
  EXPECT_EQ(settled.ready_tasks, 0);
  std::cout << "scheduler_local_ready_fence_trace reached_window="
            << reached_window
            << " cleanup_overtook_publication=" << cleanup_overtook_publication
            << " exact_exception=" << exact_exception
            << " ready_after_settlement=" << settled.ready_tasks
            << " executed_before_cleanup=" << executed.load() << '\n';
}

/** @brief Proves HP-CPU submission shares the worker wait handshake. */
TEST(SchedulerExceptionPublication,
     GpuHpCpuSubmissionCannotMissPredicateWaitWindow) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;
  config.gpu_workers = 0;
  config.prefer_gpu_for_hp = false;
  GpuPipelineScheduler scheduler(config);
  CpuWaitBarrier barrier;
  const SchedulerCpuWaitHook hook{&barrier, block_before_cpu_wait};
  ScopedCpuWaitHook hook_guard(&hook);
  std::atomic<bool> submit_returned{false};
  std::atomic<int> completed{0};

  scheduler.start();
  const bool reached_wait_window =
      wait_for_event(barrier.arrived, std::chrono::milliseconds(3000));
  std::thread submitter([&] {
    std::vector<SchedulerTaskRuntime::Task> tasks;
    tasks.emplace_back([&] {
      completed.fetch_add(1, std::memory_order_relaxed);
      scheduler.dec_tasks_to_complete();
    });
    scheduler.submit_initial_tasks(std::move(tasks), 1);
    submit_returned.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const bool submitted_while_wait_mutex_held =
      submit_returned.load(std::memory_order_acquire);
  barrier.release.store(true, std::memory_order_release);
  const bool submission_finished =
      wait_for_event(submit_returned, std::chrono::milliseconds(3000));
  submitter.join();
  if (submission_finished) {
    EXPECT_NO_THROW(scheduler.wait_for_completion());
  }
  scheduler.shutdown();

  EXPECT_TRUE(reached_wait_window);
  EXPECT_FALSE(submitted_while_wait_mutex_held);
  EXPECT_TRUE(submission_finished);
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);
  std::cout << "scheduler_hp_wait_handshake_trace reached_window="
            << reached_wait_window
            << " submitted_while_mutex_held=" << submitted_while_wait_mutex_held
            << " submission_finished=" << submission_finished
            << " completed=" << completed.load() << '\n';
}

/**
 * @brief CPU initial submissions reject invalid counts without publication.
 */
TEST(SchedulerTransactionalBatch, CpuInitialCountValidationPreservesState) {
  CpuWorkStealingScheduler scheduler(1);
  run_initial_count_validation_case(scheduler,
                                    cpu_scheduler_transactional_snapshot,
                                    "cpu/initial_count_validation");
}

/**
 * @brief Pipeline initial submissions reject invalid counts without routing.
 */
TEST(SchedulerTransactionalBatch, GpuInitialCountValidationPreservesState) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_initial_count_validation_case(scheduler,
                                    gpu_scheduler_transactional_snapshot,
                                    "gpu/initial_count_validation");
}

/** @brief CPU completion accounting has a zero floor and checked upper bound.
 */
TEST(SchedulerCompletionAccounting, CpuCounterBoundariesAndEpochWrap) {
  CpuWorkStealingScheduler scheduler(1);
  run_completion_counter_boundary_case(
      scheduler, cpu_scheduler_transactional_snapshot,
      set_cpu_scheduler_epoch_for_testing, "cpu/completion_boundaries");
}

/**
 * @brief Pipeline completion accounting has a zero floor and checked bound.
 */
TEST(SchedulerCompletionAccounting, GpuCounterBoundariesAndEpochWrap) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_completion_counter_boundary_case(scheduler,
                                       gpu_scheduler_transactional_snapshot,
                                       set_gpu_scheduler_epoch_for_testing,
                                       "gpu_pipeline/completion_boundaries");
}

/** @brief CPU null publication preserves every batch-state field. */
TEST(SchedulerExceptionPublication, CpuNullExceptionIsNoop) {
  CpuWorkStealingScheduler scheduler(2);
  run_null_exception_noop_case(scheduler, cpu_scheduler_transactional_snapshot,
                               "cpu/null_exception");
}

/** @brief Pipeline null publication preserves every batch-state field. */
TEST(SchedulerExceptionPublication, GpuPipelineNullExceptionIsNoop) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_null_exception_noop_case(scheduler, gpu_scheduler_transactional_snapshot,
                               "gpu_pipeline/null_exception");
}

/** @brief A CPU callback from an older epoch cannot change the current count.
 */
TEST(SchedulerCompletionAccounting, CpuStaleCallbackCannotMutateNewBatch) {
  CpuWorkStealingScheduler scheduler(1);
  run_stale_completion_counter_case(scheduler,
                                    cpu_scheduler_transactional_snapshot,
                                    "cpu/stale_completion_counter");
}

/**
 * @brief A pipeline callback from an older epoch cannot change current count.
 */
TEST(SchedulerCompletionAccounting, GpuStaleCallbackCannotMutateNewBatch) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_stale_completion_counter_case(scheduler,
                                    gpu_scheduler_transactional_snapshot,
                                    "gpu_pipeline/stale_completion_counter");
}

/**
 * @brief Reproduces partial CPU initial-handle publication on the second push.
 * @note Final behavior requires the exact exception and a completely empty
 * queue/counter transaction before any borrowed handle can run.
 */
TEST(SchedulerTransactionalBatch, CpuHighInitialRollbackOnSecondPush) {
  CpuWorkStealingScheduler scheduler(1);
  std::atomic<int> executions{0};
  CountingTaskExecutor executor(executions);
  FailureInjectionState failure;
  failure.fail_on_attempt = 2;
  const SchedulerFailureInjectionHook hook{&failure, inject_tagged_bad_alloc};

  scheduler.start();
  bool exact_exception = false;
  {
    ScopedFailureHook guard(set_cpu_scheduler_failure_injection_hook, &hook);
    try {
      scheduler.submit_initial_task_handles(make_handles(executor), 3,
                                            SchedulerTaskPriority::High);
    } catch (const TaggedBadAlloc&) {
      exact_exception = true;
    } catch (...) {
      exact_exception = false;
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const SchedulerTransactionalStateSnapshot state =
      cpu_scheduler_transactional_snapshot(&scheduler);
  scheduler.shutdown();

  EXPECT_TRUE(exact_exception);
  EXPECT_EQ(failure.matching_attempts.load(), 2u);
  EXPECT_EQ(executions.load(), 0);
  EXPECT_EQ(state.queued_tasks, 0u);
  EXPECT_EQ(state.ready_tasks, 0);
  EXPECT_EQ(state.tasks_to_complete, 0);
  std::cout << "scheduler_batch_transaction_prefix scheduler=cpu lane=high"
            << " exception_exact=" << exact_exception
            << " executions=" << executions.load()
            << " queued=" << state.queued_tasks
            << " ready=" << state.ready_tasks
            << " completion=" << state.tasks_to_complete << '\n';
}

/**
 * @brief Reproduces partial GPU-pipeline RT publication on the second push.
 */
TEST(SchedulerTransactionalBatch, GpuRtInitialRollbackOnSecondPush) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 1;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  std::atomic<int> executions{0};
  CountingTaskExecutor executor(executions);
  FailureInjectionState failure;
  failure.fail_on_attempt = 2;
  const SchedulerFailureInjectionHook hook{&failure, inject_tagged_bad_alloc};

  scheduler.start();
  bool exact_exception = false;
  {
    ScopedFailureHook guard(set_gpu_scheduler_failure_injection_hook, &hook);
    try {
      scheduler.submit_initial_task_handles(make_handles(executor), 3,
                                            SchedulerTaskPriority::High);
    } catch (const TaggedBadAlloc&) {
      exact_exception = true;
    } catch (...) {
      exact_exception = false;
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const SchedulerTransactionalStateSnapshot state =
      gpu_scheduler_transactional_snapshot(&scheduler);
  scheduler.shutdown();

  EXPECT_TRUE(exact_exception);
  EXPECT_EQ(failure.matching_attempts.load(), 2u);
  EXPECT_EQ(executions.load(), 0);
  EXPECT_EQ(state.queued_tasks, 0u);
  EXPECT_EQ(state.ready_tasks, 0);
  EXPECT_EQ(state.tasks_to_complete, 0);
  std::cout << "scheduler_batch_transaction_prefix scheduler=gpu lane=rt"
            << " exception_exact=" << exact_exception
            << " executions=" << executions.load()
            << " queued=" << state.queued_tasks
            << " ready=" << state.ready_tasks
            << " completion=" << state.tasks_to_complete << '\n';
}

/** @brief Repeats CPU high-priority initial transaction rollback and reuse. */
TEST(SchedulerTransactionalBatch, CpuHighInitialMultiRoundBorrowedSafety) {
  CpuWorkStealingScheduler scheduler(2);
  run_initial_handle_transaction_case(
      scheduler, set_cpu_scheduler_failure_injection_hook,
      cpu_scheduler_transactional_snapshot, SchedulerTaskPriority::High,
      "cpu/high_initial");
}

/** @brief Repeats CPU local-queue initial transaction rollback and reuse. */
TEST(SchedulerTransactionalBatch, CpuNormalInitialMultiRoundBorrowedSafety) {
  CpuWorkStealingScheduler scheduler(3);
  run_initial_handle_transaction_case(
      scheduler, set_cpu_scheduler_failure_injection_hook,
      cpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "cpu/normal_initial");
}

/** @brief Audits CPU global ready batches for both priority queues. */
TEST(SchedulerTransactionalBatch, CpuReadyBatchBothPrioritiesRollback) {
  CpuWorkStealingScheduler high_scheduler(2);
  run_ready_handle_transaction_case(
      high_scheduler, set_cpu_scheduler_failure_injection_hook,
      cpu_scheduler_transactional_snapshot, SchedulerTaskPriority::High,
      "cpu/high_ready");
  CpuWorkStealingScheduler normal_scheduler(2);
  run_ready_handle_transaction_case(
      normal_scheduler, set_cpu_scheduler_failure_injection_hook,
      cpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "cpu/normal_ready");
}

/** @brief Covers the CPU worker-local dirty dependency release transaction. */
TEST(SchedulerTransactionalBatch, CpuWorkerReleaseRollbackAndReuse) {
  CpuWorkStealingScheduler scheduler(2);
  run_worker_release_transaction_case(
      scheduler, set_cpu_scheduler_failure_injection_hook,
      cpu_scheduler_transactional_snapshot, "cpu/worker_local_release");
}

/** @brief Repeats GPU-pipeline RT initial rollback and reuse. */
TEST(SchedulerTransactionalBatch, GpuRtInitialMultiRoundBorrowedSafety) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_initial_handle_transaction_case(
      scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::High,
      "gpu_pipeline/rt_initial");
}

/** @brief Repeats GPU-pipeline HP-CPU initial rollback and reuse. */
TEST(SchedulerTransactionalBatch, GpuHpCpuInitialMultiRoundBorrowedSafety) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  config.prefer_gpu_for_hp = false;
  GpuPipelineScheduler scheduler(config);
  run_initial_handle_transaction_case(
      scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "gpu_pipeline/hp_cpu_initial");
}

/** @brief Audits the GPU queue rollback without requiring Metal hardware. */
TEST(SchedulerTransactionalBatch, GpuQueueInitialRollbackAndCpuRetry) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_initial_handle_transaction_case(
      scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "gpu_pipeline/gpu_initial", true);
}

/** @brief Audits RT, HP-CPU, and GPU ready-batch transactions. */
TEST(SchedulerTransactionalBatch, GpuReadyBatchAllRoutesRollback) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  config.prefer_gpu_for_hp = false;
  GpuPipelineScheduler rt_scheduler(config);
  run_ready_handle_transaction_case(
      rt_scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::High,
      "gpu_pipeline/rt_ready");
  GpuPipelineScheduler hp_scheduler(config);
  run_ready_handle_transaction_case(
      hp_scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "gpu_pipeline/hp_cpu_ready");
  GpuPipelineScheduler gpu_scheduler(config);
  run_ready_handle_transaction_case(
      gpu_scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "gpu_pipeline/gpu_ready", true);
}

/** @brief Covers GPU-pipeline worker dirty dependency release rollback. */
TEST(SchedulerTransactionalBatch, GpuWorkerReleaseRollbackAndReuse) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  config.prefer_gpu_for_hp = false;
  GpuPipelineScheduler scheduler(config);
  run_worker_release_transaction_case(
      scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, "gpu_pipeline/worker_release");
}

/** @brief Audits callback-valued CPU initial batches on both queue routes. */
TEST(SchedulerTransactionalBatch, CpuCallbackInitialAllRoutesRollback) {
  CpuWorkStealingScheduler high_scheduler(2);
  run_initial_callback_transaction_case(
      high_scheduler, set_cpu_scheduler_failure_injection_hook,
      cpu_scheduler_transactional_snapshot, SchedulerTaskPriority::High,
      "cpu/high_callback_initial");
  CpuWorkStealingScheduler normal_scheduler(3);
  run_initial_callback_transaction_case(
      normal_scheduler, set_cpu_scheduler_failure_injection_hook,
      cpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "cpu/normal_callback_initial");
}

/** @brief Audits callback-valued pipeline batches on RT, HP-CPU, and GPU. */
TEST(SchedulerTransactionalBatch, GpuCallbackInitialAllRoutesRollback) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  config.prefer_gpu_for_hp = false;
  GpuPipelineScheduler rt_scheduler(config);
  run_initial_callback_transaction_case(
      rt_scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::High,
      "gpu_pipeline/rt_callback_initial");
  GpuPipelineScheduler hp_scheduler(config);
  run_initial_callback_transaction_case(
      hp_scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "gpu_pipeline/hp_cpu_callback_initial");
  GpuPipelineScheduler gpu_scheduler(config);
  run_initial_callback_transaction_case(
      gpu_scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, SchedulerTaskPriority::Normal,
      "gpu_pipeline/gpu_callback_initial", true);
}

/** @brief CPU failed enqueue preserves a consumed batch's exception fields. */
TEST(SchedulerTransactionalBatch, CpuFailurePreservesExactExceptionState) {
  CpuWorkStealingScheduler scheduler(2);
  run_exception_state_rollback_case(
      scheduler, set_cpu_scheduler_failure_injection_hook,
      cpu_scheduler_transactional_snapshot, "cpu/exception_state");
}

/** @brief Pipeline failed enqueue preserves a consumed exception state. */
TEST(SchedulerTransactionalBatch,
     GpuPipelineFailurePreservesExactExceptionState) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_exception_state_rollback_case(
      scheduler, set_gpu_scheduler_failure_injection_hook,
      gpu_scheduler_transactional_snapshot, "gpu_pipeline/exception_state");
}

/** @brief CPU initial reset cannot overtake exception flag publication. */
TEST(SchedulerTransactionalBatch, CpuExceptionPublicationHoldsBatchGate) {
  CpuWorkStealingScheduler scheduler(2);
  run_cross_epoch_publication_gate_case(
      scheduler, set_cpu_scheduler_exception_publication_hook,
      cpu_scheduler_transactional_snapshot, "cpu/cross_epoch_gate");
}

/** @brief Pipeline initial reset cannot overtake exception flag publication. */
TEST(SchedulerTransactionalBatch,
     GpuPipelineExceptionPublicationHoldsBatchGate) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_cross_epoch_publication_gate_case(
      scheduler, set_gpu_scheduler_exception_publication_hook,
      gpu_scheduler_transactional_snapshot, "gpu_pipeline/cross_epoch_gate");
}

/** @brief CPU public running waits for complete worker-vector installation. */
TEST(SchedulerStartRollback, CpuRunningPublishesAfterWorkerVectorInstall) {
  CpuWorkStealingScheduler scheduler(4);
  run_start_publication_case(
      scheduler, set_cpu_scheduler_start_publication_hook,
      cpu_scheduler_transactional_snapshot, 4, "cpu/start_publication");
}

/** @brief Pipeline public running waits for both complete worker vectors. */
TEST(SchedulerStartRollback,
     GpuPipelineRunningPublishesAfterWorkerVectorInstall) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_start_publication_case(scheduler,
                             set_gpu_scheduler_start_publication_hook,
                             gpu_scheduler_transactional_snapshot, 4,
                             "gpu_pipeline/start_publication");
}

/** @brief CPU start leaves no state when local-queue resize fails. */
TEST(SchedulerStartRollback, CpuQueueResizeBadAllocAndRetry) {
  CpuWorkStealingScheduler scheduler(4);
  run_start_failure_case(scheduler, set_cpu_scheduler_failure_injection_hook,
                         cpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::StartResourceAllocation, 1,
                         false, "cpu/queue_resize");
}

/** @brief CPU start leaves no state when mutex-vector reserve fails. */
TEST(SchedulerStartRollback, CpuMutexReserveBadAllocAndRetry) {
  CpuWorkStealingScheduler scheduler(4);
  run_start_failure_case(scheduler, set_cpu_scheduler_failure_injection_hook,
                         cpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::StartResourceAllocation, 2,
                         false, "cpu/mutex_reserve");
}

/** @brief CPU start drops staged mutexes when a middle allocation fails. */
TEST(SchedulerStartRollback, CpuMiddleMutexBadAllocAndRetry) {
  CpuWorkStealingScheduler scheduler(4);
  run_start_failure_case(scheduler, set_cpu_scheduler_failure_injection_hook,
                         cpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::StartResourceAllocation, 4,
                         false, "cpu/middle_mutex");
}

/** @brief CPU start leaves no state when worker-vector reserve fails. */
TEST(SchedulerStartRollback, CpuWorkerReserveBadAllocAndRetry) {
  CpuWorkStealingScheduler scheduler(4);
  run_start_failure_case(scheduler, set_cpu_scheduler_failure_injection_hook,
                         cpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::StartResourceAllocation, 7,
                         false, "cpu/worker_reserve");
}

/** @brief CPU start rolls back a first-thread bad_alloc and can retry. */
TEST(SchedulerStartRollback, CpuFirstThreadBadAllocAndRetry) {
  CpuWorkStealingScheduler scheduler(4);
  run_start_failure_case(scheduler, set_cpu_scheduler_failure_injection_hook,
                         cpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::CpuThreadCreate, 1, false,
                         "cpu/first_thread");
}

/** @brief CPU start joins partial workers after a middle system_error. */
TEST(SchedulerStartRollback, CpuMiddleThreadSystemErrorAndRetry) {
  CpuWorkStealingScheduler scheduler(4);
  run_start_failure_case(scheduler, set_cpu_scheduler_failure_injection_hook,
                         cpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::CpuThreadCreate, 3, true,
                         "cpu/middle_thread");
}

/** @brief Pipeline start leaves no state on CPU-worker reserve failure. */
TEST(SchedulerStartRollback, GpuPipelineCpuReserveBadAllocAndRetry) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_start_failure_case(scheduler, set_gpu_scheduler_failure_injection_hook,
                         gpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::StartResourceAllocation, 1,
                         false, "gpu_pipeline/cpu_reserve");
}

/** @brief Pipeline start leaves no state on GPU-worker reserve failure. */
TEST(SchedulerStartRollback, GpuPipelineGpuReserveBadAllocAndRetry) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_start_failure_case(scheduler, set_gpu_scheduler_failure_injection_hook,
                         gpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::StartResourceAllocation, 2,
                         false, "gpu_pipeline/gpu_reserve");
}

/** @brief Pipeline start rolls back its first CPU-thread bad_alloc. */
TEST(SchedulerStartRollback, GpuPipelineFirstCpuThreadBadAllocAndRetry) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_start_failure_case(scheduler, set_gpu_scheduler_failure_injection_hook,
                         gpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::CpuThreadCreate, 1, false,
                         "gpu_pipeline/first_cpu_thread");
}

/** @brief Pipeline start joins partial CPU workers after system_error. */
TEST(SchedulerStartRollback, GpuPipelineMiddleCpuThreadSystemErrorAndRetry) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 4;
  config.gpu_workers = 0;
  GpuPipelineScheduler scheduler(config);
  run_start_failure_case(scheduler, set_gpu_scheduler_failure_injection_hook,
                         gpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::CpuThreadCreate, 3, true,
                         "gpu_pipeline/middle_cpu_thread");
}

/** @brief Pipeline start joins CPU and first GPU worker after GPU failure. */
TEST(SchedulerStartRollback, GpuPipelineMiddleGpuThreadBadAllocAndRetry) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 3;
  GpuPipelineScheduler scheduler(config);
  set_gpu_scheduler_force_gpu_route(true);
  run_start_failure_case(scheduler, set_gpu_scheduler_failure_injection_hook,
                         gpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::GpuThreadCreate, 2, false,
                         "gpu_pipeline/middle_gpu_thread");
  set_gpu_scheduler_force_gpu_route(false);
}

/** @brief Pipeline first GPU-thread system_error preserves identity and retry.
 */
TEST(SchedulerStartRollback, GpuPipelineFirstGpuThreadSystemErrorAndRetry) {
  GpuPipelineScheduler::Config config;
  config.cpu_workers = 2;
  config.gpu_workers = 2;
  GpuPipelineScheduler scheduler(config);
  set_gpu_scheduler_force_gpu_route(true);
  run_start_failure_case(scheduler, set_gpu_scheduler_failure_injection_hook,
                         gpu_scheduler_transactional_snapshot,
                         SchedulerFailurePoint::GpuThreadCreate, 1, true,
                         "gpu_pipeline/first_gpu_thread");
  set_gpu_scheduler_force_gpu_route(false);
}

}  // namespace
}  // namespace ps::testing
