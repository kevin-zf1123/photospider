#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "schedulers/sdk_example_scheduler.hpp"

namespace {

using ps::Device;
using ps::kSchedulerWorkerRequestMax;
using ps::SchedulerHostContext;
using ps::SchedulerTaskPriority;
using ps::SchedulerTraceAction;
using ps::TaskExecutor;
using ps::TaskHandle;
using ps::scheduler_example::ExamplePolicy;
using ps::scheduler_example::SdkExampleScheduler;

/** @brief Maximum duration allowed for deterministic thread handshakes. */
constexpr auto kTestTimeout = std::chrono::seconds(2);

/**
 * @brief Waits for an atomic integer to reach one expected value.
 * @param value Counter published by a scheduler worker.
 * @param expected Value required before the deadline.
 * @param timeout Maximum wait duration.
 * @return True when the expected value becomes visible.
 * @throws Nothing.
 */
bool wait_for_atomic_value(const std::atomic<int>& value, int expected,
                           std::chrono::milliseconds timeout) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (value.load(std::memory_order_acquire) != expected) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

/**
 * @brief Waits for scheduler callback bookkeeping to reach one exact count.
 * @param scheduler Scheduler exposing the test-only borrowed-callback count.
 * @param expected Exact in-flight count required before the deadline.
 * @param timeout Maximum wait duration.
 * @return True when the expected count becomes observable.
 * @throws std::system_error if scheduler-state locking fails.
 * @note Executor-visible completion may precede the worker's `finish_work()`
 *       bookkeeping. Polling this explicit state avoids assuming those two
 *       publications are atomic while a deliberately blocked callback keeps
 *       the target count stable.
 */
bool wait_for_borrowed_in_flight(const SdkExampleScheduler& scheduler,
                                 std::size_t expected,
                                 std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (scheduler.borrowed_in_flight_for_testing() != expected) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

/**
 * @brief Manual one-shot gate used to order scheduler concurrency tests.
 *
 * @throws std::system_error if synchronization primitives cannot initialize or
 * an `open()`/`wait_for()` operation cannot synchronize.
 * @note `open()` and `wait_for()` may run on different threads. The gate never
 * resets, each phase uses a separate instance, and the owning test keeps the
 * gate alive until every waiter has returned.
 */
class ManualGate final {
 public:
  /**
   * @brief Opens the gate and wakes every waiter.
   * @return Nothing.
   * @throws std::system_error if mutex locking fails.
   */
  void open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    condition_.notify_all();
  }

  /**
   * @brief Waits until the gate opens or the timeout expires.
   * @param timeout Maximum wait duration.
   * @return True when the gate opened.
   * @throws std::system_error if condition-variable waiting fails.
   */
  bool wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this]() { return open_; });
  }

  /**
   * @brief Prevents copying synchronization state.
   * @param other Gate whose synchronization state cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  ManualGate(const ManualGate&) = delete;

  /**
   * @brief Prevents copy assignment of synchronization state.
   * @param other Gate whose synchronization state cannot replace this gate.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ManualGate& operator=(const ManualGate&) = delete;

  /**
   * @brief Creates one closed gate.
   * @throws std::system_error if mutex or condition-variable initialization
   * fails.
   */
  ManualGate() = default;

 private:
  /** @brief Protects the open predicate. */
  std::mutex mutex_;
  /** @brief Wakes threads waiting for the open predicate. */
  std::condition_variable condition_;
  /** @brief True after the first call to `open`. */
  bool open_ = false;
};

/**
 * @brief Records public host-context attribution without throwing.
 *
 * @throws Nothing through its host-context callbacks and destruction.
 * @note Atomic fields allow scheduler workers and the test thread to inspect
 * counts without another synchronization dependency. The scheduler borrows
 * this object from `attach()` through `detach()`, so each test keeps it alive
 * until that interval has ended.
 */
class RecordingHostContext final : public SchedulerHostContext {
 public:
  /** @copydoc SchedulerHostContext::is_device_available */
  bool is_device_available(Device device) const noexcept override {
    return device == Device::CPU;
  }

  /** @copydoc SchedulerHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    set_count.fetch_add(1, std::memory_order_relaxed);
    active_count.fetch_add(1, std::memory_order_relaxed);
    last_worker.store(worker_id, std::memory_order_relaxed);
    last_epoch.store(epoch, std::memory_order_relaxed);
  }

  /** @copydoc SchedulerHostContext::clear_task_context */
  void clear_task_context() noexcept override {
    clear_count.fetch_add(1, std::memory_order_relaxed);
    active_count.fetch_sub(1, std::memory_order_relaxed);
  }

  /** @copydoc SchedulerHostContext::log_event */
  void log_event(SchedulerTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch) noexcept override {
    (void)node_id;
    trace_count.fetch_add(1, std::memory_order_relaxed);
    last_action.store(static_cast<std::uint32_t>(action),
                      std::memory_order_relaxed);
    last_worker.store(worker_id, std::memory_order_relaxed);
    last_epoch.store(epoch, std::memory_order_relaxed);
  }

  /**
   * @brief Releases the concrete test host after scheduler detachment.
   * @throws Nothing.
   * @note No scheduler may retain the borrowed host pointer at destruction.
   */
  ~RecordingHostContext() override = default;

  /** @brief Number of published task contexts. */
  std::atomic<int> set_count{0};
  /** @brief Number of cleared task contexts. */
  std::atomic<int> clear_count{0};
  /** @brief Current published-context balance. */
  std::atomic<int> active_count{0};
  /** @brief Number of trace callbacks observed. */
  std::atomic<int> trace_count{0};
  /** @brief Most recently observed worker id. */
  std::atomic<int> last_worker{-1};
  /** @brief Most recently observed epoch. */
  std::atomic<std::uint64_t> last_epoch{0U};
  /** @brief Most recently observed trace action representation. */
  std::atomic<std::uint32_t> last_action{0U};
};

/**
 * @brief Executes a two-handle batch while holding the first handle at a gate.
 *
 * @throws std::system_error if gate or scheduler synchronization fails.
 * @throws std::runtime_error if the release gate does not open before timeout.
 * @throws std::bad_alloc if timeout diagnostic storage cannot allocate.
 * @note Scheduler workers may enter this executor concurrently. It borrows the
 * scheduler and both gates, which the test keeps alive through completion; its
 * atomic counter records every entry before completion accounting decrements.
 */
class BlockingBatchExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Borrows scheduler and gates for the executor lifetime.
   * @param scheduler Scheduler whose active batch owns the handles.
   * @param entered Gate opened when task zero starts.
   * @param release Gate that releases task zero.
   * @throws Nothing.
   */
  BlockingBatchExecutor(SdkExampleScheduler& scheduler, ManualGate& entered,
                        ManualGate& release) noexcept
      : scheduler_(scheduler), entered_(entered), release_(release) {}

  /** @copydoc TaskExecutor::run_task */
  void run_task(int task_id) override {
    execution_count.fetch_add(1, std::memory_order_relaxed);
    if (task_id == 0) {
      entered_.open();
      if (!release_.wait_for(kTestTimeout)) {
        throw std::runtime_error("blocking executor release timed out");
      }
    }
    scheduler_.dec_tasks_to_complete();
  }

  /** @brief Number of handles whose callback body was entered. */
  std::atomic<int> execution_count{0};

 private:
  /** @brief Scheduler receiving completion calls. */
  SdkExampleScheduler& scheduler_;
  /** @brief Signals first-handle entry. */
  ManualGate& entered_;
  /** @brief Releases the first handle. */
  ManualGate& release_;
};

/**
 * @brief Records any incorrectly published candidate handle.
 * @throws Nothing.
 * @note A scheduler worker may invoke the borrowed executor, while the test
 * reads its atomic counter only after the publication boundary settles.
 */
class PassiveExecutor final : public TaskExecutor {
 public:
  /** @copydoc TaskExecutor::run_task */
  void run_task(int task_id) override {
    (void)task_id;
    execution_count.fetch_add(1, std::memory_order_relaxed);
  }

  /** @brief Number of unexpectedly entered handles. */
  std::atomic<int> execution_count{0};
};

/**
 * @brief Exception identity used to detect cross-batch contamination.
 * @throws std::bad_alloc if runtime-error message storage cannot allocate.
 * @note An old scheduler worker publishes this identity after a new batch has
 * started so the test can verify epoch filtering.
 */
class LateBatchError final : public std::runtime_error {
 public:
  /**
   * @brief Creates the fixed late-batch failure identity.
   * @throws std::bad_alloc if `std::runtime_error` message storage cannot
   * allocate.
   */
  LateBatchError() : std::runtime_error("late batch failure") {}
};

/**
 * @brief Exception identity used to verify first-exception transport.
 * @throws std::bad_alloc if runtime-error message storage cannot allocate.
 * @note A scheduler worker throws this host-owned type and the waiting test
 * requires the same dynamic type to cross the scheduler boundary.
 */
class ContextTaskError final : public std::runtime_error {
 public:
  /**
   * @brief Creates the fixed task-context failure identity.
   * @throws std::bad_alloc if `std::runtime_error` message storage cannot
   * allocate.
   */
  ContextTaskError() : std::runtime_error("context task failure") {}
};

/**
 * @brief Blocks an old-epoch task, then attempts stale counter/error mutation.
 * @throws std::system_error if gate or scheduler synchronization fails.
 * @throws std::runtime_error if the release gate does not open before timeout.
 * @throws std::bad_alloc if exception construction or publication cannot
 * allocate.
 * @note A scheduler worker invokes this executor after borrowing the scheduler
 * and both gates. The test keeps all three alive until the blocked old-epoch
 * callback has attempted its deliberately stale publications.
 */
class LateBatchExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Borrows scheduler and ordering gates.
   * @param scheduler Scheduler whose epoch will advance while blocked.
   * @param entered Gate opened on task entry.
   * @param release Gate allowing stale publication attempts.
   * @throws Nothing.
   */
  LateBatchExecutor(SdkExampleScheduler& scheduler, ManualGate& entered,
                    ManualGate& release) noexcept
      : scheduler_(scheduler), entered_(entered), release_(release) {}

  /** @copydoc TaskExecutor::run_task */
  void run_task(int task_id) override {
    (void)task_id;
    entered_.open();
    if (!release_.wait_for(kTestTimeout)) {
      throw std::runtime_error("late executor release timed out");
    }
    scheduler_.set_exception(std::make_exception_ptr(LateBatchError()));
    scheduler_.dec_tasks_to_complete();
  }

 private:
  /** @brief Scheduler receiving deliberately stale calls. */
  SdkExampleScheduler& scheduler_;
  /** @brief Signals old task entry. */
  ManualGate& entered_;
  /** @brief Releases old task after a new batch publishes. */
  ManualGate& release_;
};

/**
 * @brief Completes one current batch handle normally.
 * @throws std::system_error if scheduler completion synchronization fails.
 * @note The executor borrows a scheduler that outlives every worker callback;
 * its atomic counter permits observation from the test thread.
 */
class CompletingExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Borrows the scheduler receiving completion.
   * @param scheduler Active scheduler.
   * @throws Nothing.
   */
  explicit CompletingExecutor(SdkExampleScheduler& scheduler) noexcept
      : scheduler_(scheduler) {}

  /** @copydoc TaskExecutor::run_task */
  void run_task(int task_id) override {
    (void)task_id;
    execution_count.fetch_add(1, std::memory_order_relaxed);
    scheduler_.dec_tasks_to_complete();
  }

  /** @brief Number of current handles entered. */
  std::atomic<int> execution_count{0};

 private:
  /** @brief Scheduler receiving completion. */
  SdkExampleScheduler& scheduler_;
};

/**
 * @brief Executor that logs attribution and optionally throws a tagged error.
 * @throws std::system_error if scheduler completion synchronization fails.
 * @throws std::bad_alloc if the tagged runtime-error message cannot allocate.
 * @note The executor borrows a scheduler that outlives its worker callback.
 * Host-context setup and clearing surround `run_task()` in scheduler code, so
 * both the success and exception paths exercise balanced attribution.
 */
class ContextExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Configures one success or failure callback.
   * @param scheduler Scheduler used for trace and completion calls.
   * @param should_throw True to throw `ContextTaskError` after tracing.
   * @throws Nothing.
   */
  ContextExecutor(SdkExampleScheduler& scheduler, bool should_throw) noexcept
      : scheduler_(scheduler), should_throw_(should_throw) {}

  /** @copydoc TaskExecutor::run_task */
  void run_task(int task_id) override {
    scheduler_.log_event(SchedulerTraceAction::Execute, task_id);
    if (should_throw_) {
      throw ContextTaskError();
    }
    scheduler_.dec_tasks_to_complete();
  }

 private:
  /** @brief Scheduler receiving trace and completion calls. */
  SdkExampleScheduler& scheduler_;
  /** @brief Selects the tagged exception path. */
  bool should_throw_;
};

/**
 * @brief Throws before the second initial handle enters the staged deque.
 * @param attempt One-based staging attempt.
 * @return Nothing for the first attempt.
 * @throws std::bad_alloc on the second attempt.
 * @note `ScopedStagingFailure` installs this process-local hook only around one
 * synchronous initial submission. Any concurrent initial submission would
 * observe the same hook and is therefore excluded by the test scope.
 */
void fail_second_initial_staging_attempt(std::size_t attempt) {
  if (attempt == 2U) {
    throw std::bad_alloc{};
  }
}

/**
 * @brief Owns the global example-scheduler failure hook for one test scope.
 * @throws Nothing.
 * @note The hook is process-local rather than scheduler-local. Tests must not
 * nest guards or perform another initial submission while a guard is active;
 * destruction clears the hook before borrowed fixture state leaves scope.
 */
class ScopedStagingFailure final {
 public:
  /**
   * @brief Installs deterministic second-entry failure injection.
   * @throws Nothing.
   * @note This guard is the sole owner of the process-local hook until its
   * destructor runs.
   */
  ScopedStagingFailure() noexcept {
    SdkExampleScheduler::set_initial_staging_failure_hook_for_testing(
        &fail_second_initial_staging_attempt);
  }

  /**
   * @brief Clears deterministic failure injection.
   * @throws Nothing.
   * @note The injecting submission has returned before guard destruction.
   */
  ~ScopedStagingFailure() noexcept {
    SdkExampleScheduler::set_initial_staging_failure_hook_for_testing(nullptr);
  }

  /**
   * @brief Prevents duplicating global-hook ownership.
   * @param other Guard whose global-hook ownership cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  ScopedStagingFailure(const ScopedStagingFailure& other) = delete;

  /**
   * @brief Prevents copy assignment of global-hook ownership.
   * @param other Guard whose ownership cannot replace this active guard.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedStagingFailure& operator=(const ScopedStagingFailure& other) = delete;
};

TEST(SchedulerTaskHandleContract, HalfEmptyHandleIsANoOp) {
  PassiveExecutor executor;
  const TaskHandle half_empty{&executor, -1, 17};

  EXPECT_FALSE(static_cast<bool>(half_empty));
  half_empty.run();
  EXPECT_EQ(executor.execution_count.load(std::memory_order_relaxed), 0);
}

/**
 * @brief Validates and obeys the exact ABI v2 worker grant for every policy.
 * @return Nothing; GoogleTest records grant or owned-thread mismatches.
 * @throws std::bad_alloc or std::system_error if valid scheduler construction
 *         or worker lifecycle setup exhausts resources.
 * @note CPU and heterogeneous policies own exactly the example's grant;
 *       serial policy demonstrates that a plugin may own fewer workers. No
 *       statistics text or operating-system thread snapshot is parsed.
 */
TEST(SchedulerSdkExample, ValidatesAndObeysResolvedWorkerGrant) {
  for (const ExamplePolicy policy : std::array<ExamplePolicy, 3>{
           ExamplePolicy::Serial, ExamplePolicy::CpuWorkers,
           ExamplePolicy::Heterogeneous}) {
    EXPECT_THROW((void)SdkExampleScheduler("zero_grant", policy, 0U),
                 std::invalid_argument);
    EXPECT_THROW((void)SdkExampleScheduler("excessive_grant", policy,
                                           kSchedulerWorkerRequestMax + 1U),
                 std::invalid_argument);
  }

  {
    SdkExampleScheduler cpu("cpu_grant", ExamplePolicy::CpuWorkers,
                            kSchedulerWorkerRequestMax);
    EXPECT_EQ(cpu.worker_grant_for_testing(), kSchedulerWorkerRequestMax);
    cpu.start();
    EXPECT_EQ(cpu.owned_worker_count_for_testing(), kSchedulerWorkerRequestMax);
    cpu.shutdown();
    EXPECT_EQ(cpu.owned_worker_count_for_testing(), 0U);
  }
  {
    SdkExampleScheduler heterogeneous("heterogeneous_grant",
                                      ExamplePolicy::Heterogeneous,
                                      kSchedulerWorkerRequestMax);
    EXPECT_EQ(heterogeneous.worker_grant_for_testing(),
              kSchedulerWorkerRequestMax);
    heterogeneous.start();
    EXPECT_EQ(heterogeneous.owned_worker_count_for_testing(),
              kSchedulerWorkerRequestMax);
    heterogeneous.shutdown();
    EXPECT_EQ(heterogeneous.owned_worker_count_for_testing(), 0U);
  }
  {
    SdkExampleScheduler serial("serial_grant", ExamplePolicy::Serial,
                               kSchedulerWorkerRequestMax);
    EXPECT_EQ(serial.worker_grant_for_testing(), kSchedulerWorkerRequestMax);
    serial.start();
    EXPECT_EQ(serial.owned_worker_count_for_testing(), 0U);
    serial.shutdown();
  }
}

TEST(SchedulerSdkExample, InvalidInitialCountsPreserveEpochAndBorrowing) {
  RecordingHostContext host;
  SdkExampleScheduler scheduler("count_validation", ExamplePolicy::Serial, 1U);
  scheduler.attach(host);
  scheduler.start();
  CompletingExecutor executor(scheduler);

  EXPECT_THROW(scheduler.submit_initial_task_handles(
                   {}, -1, SchedulerTaskPriority::Normal),
               std::invalid_argument);
  EXPECT_THROW(scheduler.submit_initial_task_handles(
                   {{&executor, 0, 70}}, 0, SchedulerTaskPriority::Normal),
               std::invalid_argument);
  EXPECT_EQ(executor.execution_count.load(std::memory_order_relaxed), 0);

  scheduler.submit_initial_task_handles({{&executor, 0, 71}}, 1,
                                        SchedulerTaskPriority::Normal);
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_EQ(executor.execution_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(host.last_epoch.load(std::memory_order_relaxed), 1U);

  scheduler.shutdown();
  scheduler.detach();
}

TEST(SchedulerSdkExample, NullExceptionDoesNotDiscardPublishedHandles) {
  SdkExampleScheduler scheduler("null_exception", ExamplePolicy::CpuWorkers,
                                1U);
  scheduler.start();
  ManualGate entered;
  ManualGate release;
  BlockingBatchExecutor executor(scheduler, entered, release);
  scheduler.submit_initial_task_handles(
      {{&executor, 0, 72}, {&executor, 1, 73}}, 2,
      SchedulerTaskPriority::Normal);
  if (!entered.wait_for(kTestTimeout)) {
    release.open();
    scheduler.shutdown();
    FAIL() << "published handle did not enter before null publication";
    return;
  }

  EXPECT_EQ(scheduler.tasks_to_complete_for_testing(), 2);
  EXPECT_EQ(scheduler.borrowed_in_flight_for_testing(), 1U);
  scheduler.set_exception(nullptr);
  EXPECT_EQ(scheduler.tasks_to_complete_for_testing(), 2);
  EXPECT_EQ(scheduler.borrowed_in_flight_for_testing(), 1U);
  EXPECT_FALSE(scheduler.completion_ready_for_testing());
  release.open();

  auto wait_result = std::async(
      std::launch::async, [&scheduler]() { scheduler.wait_for_completion(); });
  if (wait_result.wait_for(kTestTimeout) != std::future_status::ready) {
    scheduler.shutdown();
    wait_result.wait();
    FAIL() << "null exception publication discarded active batch work";
    return;
  }
  EXPECT_NO_THROW(wait_result.get());
  EXPECT_EQ(executor.execution_count.load(std::memory_order_relaxed), 2);
  scheduler.shutdown();
}

TEST(SchedulerSdkExample, CompletionCounterOverflowPreservesState) {
  SdkExampleScheduler scheduler("count_overflow", ExamplePolicy::Serial, 1U);
  scheduler.start();
  PassiveExecutor executor;
  scheduler.submit_initial_task_handles({{&executor, 0, 80}}, 1,
                                        SchedulerTaskPriority::Normal);

  scheduler.inc_tasks_to_complete(std::numeric_limits<int>::max() - 1);
  EXPECT_EQ(scheduler.tasks_to_complete_for_testing(),
            std::numeric_limits<int>::max());
  EXPECT_THROW(scheduler.inc_tasks_to_complete(1), std::overflow_error);
  EXPECT_EQ(scheduler.tasks_to_complete_for_testing(),
            std::numeric_limits<int>::max());

  scheduler.shutdown();
}

TEST(SchedulerSdkExample, InitialFailurePreservesPublishedBatch) {
  RecordingHostContext host;
  SdkExampleScheduler scheduler("strong_guarantee", ExamplePolicy::CpuWorkers,
                                1U);
  scheduler.attach(host);
  scheduler.start();

  ManualGate old_entered;
  ManualGate old_release;
  BlockingBatchExecutor old_executor(scheduler, old_entered, old_release);
  scheduler.submit_initial_task_handles(
      {{&old_executor, 0, 10}, {&old_executor, 1, 11}}, 2,
      SchedulerTaskPriority::Normal);
  if (!old_entered.wait_for(kTestTimeout)) {
    old_release.open();
    scheduler.shutdown();
    scheduler.detach();
    FAIL() << "published initial handle did not enter";
    return;
  }

  PassiveExecutor candidate_executor;
  {
    ScopedStagingFailure failure;
    EXPECT_THROW(
        scheduler.submit_initial_task_handles(
            {{&candidate_executor, 0, 20}, {&candidate_executor, 1, 21}}, 77,
            SchedulerTaskPriority::High),
        std::bad_alloc);
  }

  ManualGate preserved_epoch_callback;
  scheduler.submit_ready_task_any_thread(
      [&preserved_epoch_callback]() { preserved_epoch_callback.open(); },
      SchedulerTaskPriority::Normal, 1U);
  old_release.open();

  auto wait_result = std::async(
      std::launch::async, [&scheduler]() { scheduler.wait_for_completion(); });
  if (wait_result.wait_for(kTestTimeout) != std::future_status::ready) {
    scheduler.shutdown();
    wait_result.wait();
    ADD_FAILURE() << "failed initial submission changed completion state";
    scheduler.detach();
    return;
  }
  EXPECT_NO_THROW(wait_result.get());
  EXPECT_TRUE(preserved_epoch_callback.wait_for(kTestTimeout));
  EXPECT_EQ(old_executor.execution_count.load(std::memory_order_relaxed), 2);
  EXPECT_EQ(candidate_executor.execution_count.load(std::memory_order_relaxed),
            0);

  scheduler.shutdown();
  scheduler.detach();
}

TEST(SchedulerSdkExample, LateStaleCallbackCannotContaminateNewBatch) {
  RecordingHostContext host;
  SdkExampleScheduler scheduler("stale_epoch", ExamplePolicy::CpuWorkers, 1U);
  scheduler.attach(host);
  scheduler.start();

  ManualGate old_entered;
  ManualGate old_release;
  LateBatchExecutor old_executor(scheduler, old_entered, old_release);
  scheduler.submit_initial_task_handles({{&old_executor, 0, 30}}, 1,
                                        SchedulerTaskPriority::Normal);
  if (!old_entered.wait_for(kTestTimeout)) {
    old_release.open();
    scheduler.shutdown();
    scheduler.detach();
    FAIL() << "old epoch handle did not enter";
    return;
  }

  CompletingExecutor current_executor(scheduler);
  scheduler.submit_initial_task_handles({{&current_executor, 0, 40}}, 1,
                                        SchedulerTaskPriority::Normal);
  std::atomic<bool> stale_callback_entered{false};
  scheduler.submit_ready_task_any_thread(
      [&stale_callback_entered]() {
        stale_callback_entered.store(true, std::memory_order_relaxed);
      },
      SchedulerTaskPriority::Normal, 1U);

  old_release.open();
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_EQ(current_executor.execution_count.load(std::memory_order_relaxed),
            1);
  EXPECT_FALSE(stale_callback_entered.load(std::memory_order_relaxed));

  scheduler.shutdown();
  scheduler.detach();
}

TEST(SchedulerSdkExample, NewBatchWaitRetainsOldBorrowedExecutorFence) {
  RecordingHostContext host;
  SdkExampleScheduler scheduler("cross_epoch_fence", ExamplePolicy::CpuWorkers,
                                2U);
  scheduler.attach(host);
  scheduler.start();

  ManualGate old_entered;
  ManualGate old_release;
  LateBatchExecutor old_executor(scheduler, old_entered, old_release);
  scheduler.submit_initial_task_handles({{&old_executor, 0, 41}}, 1,
                                        SchedulerTaskPriority::Normal);
  if (!old_entered.wait_for(kTestTimeout)) {
    old_release.open();
    scheduler.shutdown();
    scheduler.detach();
    FAIL() << "old borrowed handle did not enter";
    return;
  }

  CompletingExecutor current_executor(scheduler);
  scheduler.submit_initial_task_handles({{&current_executor, 0, 42}}, 1,
                                        SchedulerTaskPriority::High);
  if (!wait_for_atomic_value(current_executor.execution_count, 1,
                             kTestTimeout)) {
    old_release.open();
    scheduler.shutdown();
    scheduler.detach();
    FAIL() << "new batch did not finish while old handle remained blocked";
    return;
  }

  if (!wait_for_borrowed_in_flight(scheduler, 1U, kTestTimeout)) {
    old_release.open();
    scheduler.shutdown();
    scheduler.detach();
    FAIL() << "new batch callback bookkeeping did not settle";
    return;
  }
  EXPECT_EQ(scheduler.tasks_to_complete_for_testing(), 0);
  EXPECT_EQ(scheduler.borrowed_in_flight_for_testing(), 1U);
  EXPECT_FALSE(scheduler.completion_ready_for_testing());
  auto wait_result = std::async(
      std::launch::async, [&scheduler]() { scheduler.wait_for_completion(); });
  EXPECT_EQ(wait_result.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  old_release.open();
  if (wait_result.wait_for(kTestTimeout) != std::future_status::ready) {
    scheduler.shutdown();
    wait_result.wait();
    scheduler.detach();
    FAIL() << "cross-epoch borrowed handle did not release completion wait";
    return;
  }
  EXPECT_NO_THROW(wait_result.get());
  EXPECT_EQ(scheduler.borrowed_in_flight_for_testing(), 0U);

  scheduler.shutdown();
  scheduler.detach();
}

TEST(SchedulerSdkExample, HostContextBalancesSuccessAndException) {
  {
    RecordingHostContext host;
    SdkExampleScheduler scheduler("context_success", ExamplePolicy::Serial, 1U);
    scheduler.attach(host);
    scheduler.start();
    ContextExecutor executor(scheduler, false);

    scheduler.submit_initial_task_handles({{&executor, 0, 50}}, 1,
                                          SchedulerTaskPriority::Normal);
    EXPECT_NO_THROW(scheduler.wait_for_completion());
    EXPECT_EQ(host.set_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(host.clear_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(host.active_count.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(host.trace_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(host.last_worker.load(std::memory_order_relaxed), -1);
    EXPECT_EQ(host.last_epoch.load(std::memory_order_relaxed), 1U);

    scheduler.shutdown();
    scheduler.detach();
  }

  {
    RecordingHostContext host;
    SdkExampleScheduler scheduler("context_exception", ExamplePolicy::Serial,
                                  1U);
    scheduler.attach(host);
    scheduler.start();
    ContextExecutor executor(scheduler, true);

    scheduler.submit_initial_task_handles({{&executor, 0, 60}}, 1,
                                          SchedulerTaskPriority::Normal);
    EXPECT_THROW(scheduler.wait_for_completion(), ContextTaskError);
    EXPECT_EQ(host.set_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(host.clear_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(host.active_count.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(host.trace_count.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(host.last_epoch.load(std::memory_order_relaxed), 1U);

    scheduler.shutdown();
    scheduler.detach();
  }
}

}  // namespace
