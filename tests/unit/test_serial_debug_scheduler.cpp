#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "scheduler/serial_debug_scheduler.hpp"

namespace {

/** @brief Deterministic maximum wait for serial scheduler handshakes. */
constexpr auto kTimeout = std::chrono::seconds(2);

/**
 * @brief One-shot synchronization gate for deterministic callback ordering.
 * @throws std::system_error if synchronization primitives cannot initialize or
 * an `open()`/`wait_for()` operation cannot synchronize.
 * @note `open()` and `wait_for()` may run on different threads. The owning test
 * keeps the gate alive until all waiters return, and no gate is reset.
 */
class ManualGate final {
 public:
  /**
   * @brief Creates one closed gate.
   * @throws std::system_error if synchronization initialization fails.
   */
  ManualGate() = default;

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
   * @brief Opens the gate and wakes all waiters.
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
   * @brief Waits for the gate to open.
   * @param timeout Maximum duration to wait.
   * @return True when the gate opened before timeout.
   * @throws std::system_error if condition-variable waiting fails.
   */
  bool wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [this]() { return open_; });
  }

 private:
  /** @brief Protects the open predicate. */
  std::mutex mutex_;
  /** @brief Wakes threads after opening. */
  std::condition_variable condition_;
  /** @brief True after the first open call. */
  bool open_ = false;
};

/**
 * @brief Records serial callback attribution in atomic host-context fields.
 * @throws Nothing through its host-context callbacks and destruction.
 * @note Scheduler callbacks and the test thread may access the fields
 * concurrently. The scheduler borrows this object from `attach()` through
 * `detach()`, so the test keeps it alive until that interval ends.
 */
class RecordingHostContext final : public ps::SchedulerHostContext {
 public:
  /** @copydoc ps::SchedulerHostContext::is_device_available */
  bool is_device_available(ps::Device device) const noexcept override {
    return device == ps::Device::CPU;
  }

  /** @copydoc ps::SchedulerHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    set_count.fetch_add(1, std::memory_order_relaxed);
    active_count.fetch_add(1, std::memory_order_relaxed);
    last_worker.store(worker_id, std::memory_order_relaxed);
    last_epoch.store(epoch, std::memory_order_relaxed);
  }

  /** @copydoc ps::SchedulerHostContext::clear_task_context */
  void clear_task_context() noexcept override {
    clear_count.fetch_add(1, std::memory_order_relaxed);
    active_count.fetch_sub(1, std::memory_order_relaxed);
  }

  /** @copydoc ps::SchedulerHostContext::log_event */
  void log_event(ps::SchedulerTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch) noexcept override {
    (void)node_id;
    trace_count.fetch_add(1, std::memory_order_relaxed);
    last_action.store(static_cast<std::uint32_t>(action),
                      std::memory_order_relaxed);
    last_worker.store(worker_id, std::memory_order_relaxed);
    last_epoch.store(epoch, std::memory_order_relaxed);
  }

  /**
   * @brief Releases the concrete recorder after scheduler detachment.
   * @throws Nothing.
   * @note No scheduler may retain the borrowed host pointer at destruction.
   */
  ~RecordingHostContext() override = default;

  /** @brief Number of context publications. */
  std::atomic<int> set_count{0};
  /** @brief Number of context clears. */
  std::atomic<int> clear_count{0};
  /** @brief Current context balance. */
  std::atomic<int> active_count{0};
  /** @brief Number of trace publications. */
  std::atomic<int> trace_count{0};
  /** @brief Most recent worker id. */
  std::atomic<int> last_worker{-1};
  /** @brief Most recent epoch. */
  std::atomic<std::uint64_t> last_epoch{0U};
  /** @brief Most recent trace action representation. */
  std::atomic<std::uint32_t> last_action{0U};
};

/**
 * @brief Exception identity used by stale serial callbacks.
 * @throws std::bad_alloc if runtime-error message storage cannot allocate.
 * @note A deliberately stale callback publishes this host-owned type after a
 * new serial batch starts so the test can verify epoch filtering.
 */
class StaleSerialError final : public std::runtime_error {
 public:
  /**
   * @brief Creates the fixed stale-callback exception.
   * @throws std::bad_alloc if `std::runtime_error` message storage cannot
   * allocate.
   */
  StaleSerialError() : std::runtime_error("stale serial callback") {}
};

/**
 * @brief Exception identity used by serial context transport coverage.
 * @throws std::bad_alloc if runtime-error message storage cannot allocate.
 * @note A scheduler callback throws this host-owned type and the waiting test
 * requires the same dynamic type after balanced host-context cleanup.
 */
class SerialContextError final : public std::runtime_error {
 public:
  /**
   * @brief Creates the fixed serial context exception.
   * @throws std::bad_alloc if `std::runtime_error` message storage cannot
   * allocate.
   */
  SerialContextError() : std::runtime_error("serial context failure") {}
};

/**
 * @brief Blocks one serial handle until released, then completes it.
 * @throws std::system_error if gate or scheduler synchronization fails.
 * @throws std::runtime_error if the release gate does not open before timeout.
 * @throws std::bad_alloc if timeout diagnostic storage cannot allocate.
 * @note The executor borrows the scheduler and both gates. The invoking thread
 * returns before the test releases their storage, while gate access remains
 * synchronized with the controlling test thread.
 */
class GatedSerialExecutor final : public ps::TaskExecutor {
 public:
  /**
   * @brief Borrows scheduler and ordering gates.
   * @param scheduler Scheduler receiving completion.
   * @param entered Gate opened on callback entry.
   * @param release Gate allowing callback completion.
   * @throws Nothing.
   */
  GatedSerialExecutor(ps::SerialDebugScheduler& scheduler, ManualGate& entered,
                      ManualGate& release) noexcept
      : scheduler_(scheduler), entered_(entered), release_(release) {}

  /** @copydoc ps::TaskExecutor::run_task */
  void run_task(int task_id) override {
    (void)task_id;
    entered_.open();
    if (!release_.wait_for(kTimeout)) {
      throw std::runtime_error("serial executor release timed out");
    }
    scheduler_.dec_tasks_to_complete();
  }

 private:
  /** @brief Scheduler receiving completion. */
  ps::SerialDebugScheduler& scheduler_;
  /** @brief Signals callback entry. */
  ManualGate& entered_;
  /** @brief Releases callback completion. */
  ManualGate& release_;
};

/**
 * @brief Verifies completion-counter overflow is rejected before mutation.
 * @throws std::system_error if scheduler counter synchronization fails.
 * @throws std::bad_alloc if overflow diagnostic storage cannot allocate before
 * the expected `std::overflow_error` is caught.
 * @note The executor borrows a scheduler that outlives its callback. It catches
 * only the expected overflow identity and publishes the observation through an
 * atomic flag for the test thread.
 */
class OverflowProbeExecutor final : public ps::TaskExecutor {
 public:
  /**
   * @brief Borrows the scheduler under test.
   * @param scheduler Scheduler receiving counter calls.
   * @throws Nothing.
   */
  explicit OverflowProbeExecutor(ps::SerialDebugScheduler& scheduler) noexcept
      : scheduler_(scheduler) {}

  /** @copydoc ps::TaskExecutor::run_task */
  void run_task(int task_id) override {
    (void)task_id;
    try {
      scheduler_.inc_tasks_to_complete(std::numeric_limits<int>::max());
    } catch (const std::overflow_error&) {
      overflow_observed.store(true, std::memory_order_relaxed);
    }
    scheduler_.dec_tasks_to_complete();
  }

  /** @brief True when the checked overflow path was observed. */
  std::atomic<bool> overflow_observed{false};

 private:
  /** @brief Scheduler receiving counter calls. */
  ps::SerialDebugScheduler& scheduler_;
};

/**
 * @brief Counts task-handle entries without mutating scheduler accounting.
 * @throws Nothing.
 * @note The executor borrows no external state. Its atomic count remains safe
 * when a submitting callback thread and the observing test thread differ.
 */
class CountingExecutor final : public ps::TaskExecutor {
 public:
  /** @copydoc ps::TaskExecutor::run_task */
  void run_task(int task_id) override {
    (void)task_id;
    executions.fetch_add(1, std::memory_order_relaxed);
  }

  /** @brief Number of entered borrowed handles. */
  std::atomic<int> executions{0};
};

TEST(SerialDebugSchedulerContract,
     InitialCallbackCountValidationPreservesBatchState) {
  ps::SerialDebugScheduler scheduler;
  EXPECT_THROW(scheduler.submit_initial_tasks({}, 0), std::logic_error);
  scheduler.start();
  scheduler.submit_initial_task_handles({}, 0);  // Epoch 1.
  const ps::SerialDebugScheduler::TestingState before =
      scheduler.testing_state();
  std::atomic<int> executions{0};

  std::vector<ps::SerialDebugScheduler::Task> negative_count_tasks;
  negative_count_tasks.emplace_back(
      [&executions]() { executions.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_THROW(
      scheduler.submit_initial_tasks(std::move(negative_count_tasks), -1),
      std::invalid_argument);
  std::vector<ps::SerialDebugScheduler::Task> undercounted_tasks;
  undercounted_tasks.emplace_back(
      [&executions]() { executions.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_THROW(scheduler.submit_initial_tasks(std::move(undercounted_tasks), 0),
               std::invalid_argument);

  const ps::SerialDebugScheduler::TestingState after =
      scheduler.testing_state();
  EXPECT_EQ(after.active_epoch, before.active_epoch);
  EXPECT_EQ(after.tasks_to_complete, before.tasks_to_complete);
  EXPECT_EQ(after.borrowed_in_flight, before.borrowed_in_flight);
  EXPECT_EQ(after.uncancellable_in_flight, before.uncancellable_in_flight);
  EXPECT_EQ(after.has_exception, before.has_exception);
  EXPECT_EQ(executions.load(std::memory_order_relaxed), 0);
  scheduler.shutdown();
}

TEST(SerialDebugSchedulerContract,
     InitialHandleCountValidationPreservesBatchStateAndBorrowing) {
  ps::SerialDebugScheduler scheduler;
  CountingExecutor executor;
  EXPECT_THROW(scheduler.submit_initial_task_handles({{&executor, 0, 1}}, 1),
               std::logic_error);
  scheduler.start();
  scheduler.submit_initial_task_handles({}, 0);  // Epoch 1.
  const ps::SerialDebugScheduler::TestingState before =
      scheduler.testing_state();

  EXPECT_THROW(scheduler.submit_initial_task_handles({{&executor, 0, 1}}, -1),
               std::invalid_argument);
  EXPECT_THROW(scheduler.submit_initial_task_handles({{&executor, 0, 1}}, 0),
               std::invalid_argument);

  const ps::SerialDebugScheduler::TestingState after =
      scheduler.testing_state();
  EXPECT_EQ(after.active_epoch, before.active_epoch);
  EXPECT_EQ(after.tasks_to_complete, before.tasks_to_complete);
  EXPECT_EQ(after.borrowed_in_flight, before.borrowed_in_flight);
  EXPECT_EQ(after.uncancellable_in_flight, before.uncancellable_in_flight);
  EXPECT_EQ(after.has_exception, before.has_exception);
  EXPECT_EQ(executor.executions.load(std::memory_order_relaxed), 0);
  scheduler.shutdown();
}

TEST(SerialDebugSchedulerContract, NullExceptionPreservesCompleteBatchState) {
  ps::SerialDebugScheduler scheduler;
  scheduler.start();
  scheduler.submit_initial_task_handles({}, 0);
  const ps::SerialDebugScheduler::TestingState before =
      scheduler.testing_state();

  scheduler.set_exception(nullptr);

  const ps::SerialDebugScheduler::TestingState after =
      scheduler.testing_state();
  EXPECT_EQ(after.active_epoch, before.active_epoch);
  EXPECT_EQ(after.tasks_to_complete, before.tasks_to_complete);
  EXPECT_EQ(after.borrowed_in_flight, before.borrowed_in_flight);
  EXPECT_EQ(after.uncancellable_in_flight, before.uncancellable_in_flight);
  EXPECT_EQ(after.has_exception, before.has_exception);
  EXPECT_EQ(after.completion_ready, before.completion_ready);
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  scheduler.shutdown();
}

TEST(SerialDebugSchedulerContract,
     NewBatchWaitRetainsOldBorrowedCallbackFence) {
  ps::SerialDebugScheduler scheduler;
  scheduler.start();
  scheduler.submit_initial_task_handles({}, 0);  // Epoch 1.
  ManualGate old_entered;
  ManualGate old_release;
  std::thread old_callback([&]() {
    scheduler.submit_ready_task_any_thread(
        [&]() {
          old_entered.open();
          if (!old_release.wait_for(kTimeout)) {
            throw std::runtime_error("old serial callback release timed out");
          }
          scheduler.set_exception(std::make_exception_ptr(StaleSerialError()));
          scheduler.dec_tasks_to_complete();
        },
        ps::SchedulerTaskPriority::Normal, 1U);
  });
  if (!old_entered.wait_for(kTimeout)) {
    old_release.open();
    old_callback.join();
    scheduler.shutdown();
    FAIL() << "old serial callback did not enter";
    return;
  }

  std::atomic<int> current_executions{0};
  std::vector<ps::SerialDebugScheduler::Task> current_batch;
  current_batch.emplace_back([&]() {
    current_executions.fetch_add(1, std::memory_order_relaxed);
    scheduler.dec_tasks_to_complete();
  });
  scheduler.submit_initial_tasks(std::move(current_batch), 1);
  const ps::SerialDebugScheduler::TestingState blocked_state =
      scheduler.testing_state();
  EXPECT_EQ(current_executions.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(blocked_state.active_epoch, 2U);
  EXPECT_EQ(blocked_state.tasks_to_complete, 0);
  EXPECT_EQ(blocked_state.borrowed_in_flight, 1U);
  EXPECT_FALSE(blocked_state.has_exception);
  EXPECT_FALSE(blocked_state.completion_ready);

  auto wait_result = std::async(
      std::launch::async, [&scheduler]() { scheduler.wait_for_completion(); });
  EXPECT_EQ(wait_result.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);
  old_release.open();
  old_callback.join();
  if (wait_result.wait_for(kTimeout) != std::future_status::ready) {
    scheduler.shutdown();
    wait_result.wait();
    FAIL() << "old serial callback did not release completion wait";
    return;
  }
  EXPECT_NO_THROW(wait_result.get());
  const ps::SerialDebugScheduler::TestingState settled_state =
      scheduler.testing_state();
  EXPECT_EQ(settled_state.borrowed_in_flight, 0U);
  EXPECT_FALSE(settled_state.has_exception);
  EXPECT_TRUE(settled_state.completion_ready);
  scheduler.shutdown();
}

TEST(SerialDebugSchedulerContract,
     StaleConcurrentCallbackCannotMutateNewBatch) {
  RecordingHostContext host;
  ps::SerialDebugScheduler scheduler;
  scheduler.attach(host);
  scheduler.start();

  scheduler.submit_initial_task_handles({}, 0);  // Epoch 1.
  ManualGate stale_entered;
  ManualGate stale_release;
  std::thread stale_thread([&scheduler, &stale_entered, &stale_release]() {
    scheduler.submit_ready_task_any_thread(
        [&scheduler, &stale_entered, &stale_release]() {
          stale_entered.open();
          if (!stale_release.wait_for(kTimeout)) {
            throw std::runtime_error("stale serial release timed out");
          }
          scheduler.set_exception(std::make_exception_ptr(StaleSerialError()));
          scheduler.dec_tasks_to_complete();
        },
        ps::SchedulerTaskPriority::Normal, 1U);
  });
  if (!stale_entered.wait_for(kTimeout)) {
    stale_release.open();
    stale_thread.join();
    scheduler.shutdown();
    scheduler.detach();
    FAIL() << "stale serial callback did not enter";
    return;
  }

  ManualGate current_entered;
  ManualGate current_release;
  GatedSerialExecutor current_executor(scheduler, current_entered,
                                       current_release);
  std::thread current_thread([&scheduler, &current_executor]() {
    scheduler.submit_initial_task_handles({{&current_executor, 0, 10}}, 2);
  });
  if (!current_entered.wait_for(kTimeout)) {
    stale_release.open();
    current_release.open();
    stale_thread.join();
    current_thread.join();
    scheduler.shutdown();
    scheduler.detach();
    FAIL() << "current serial callback did not enter";
    return;
  }

  std::atomic<bool> rejected_callback_entered{false};
  scheduler.submit_ready_task_any_thread(
      [&rejected_callback_entered]() {
        rejected_callback_entered.store(true, std::memory_order_relaxed);
      },
      ps::SchedulerTaskPriority::Normal, 1U);
  stale_release.open();
  stale_thread.join();
  current_release.open();
  current_thread.join();

  const ps::SerialDebugScheduler::TestingState state =
      scheduler.testing_state();
  EXPECT_EQ(state.active_epoch, 2U);
  EXPECT_EQ(state.tasks_to_complete, 1);
  EXPECT_EQ(state.borrowed_in_flight, 0U);
  EXPECT_EQ(state.uncancellable_in_flight, 0U);
  EXPECT_FALSE(state.has_exception);
  scheduler.dec_tasks_to_complete();
  EXPECT_NO_THROW(scheduler.wait_for_completion());
  EXPECT_FALSE(rejected_callback_entered.load(std::memory_order_relaxed));
  EXPECT_EQ(host.set_count.load(std::memory_order_relaxed),
            host.clear_count.load(std::memory_order_relaxed));
  EXPECT_EQ(host.active_count.load(std::memory_order_relaxed), 0);

  scheduler.shutdown();
  scheduler.detach();
}

TEST(SerialDebugSchedulerContract,
     AnyThreadTraceKeepsEpochAndBalancesExceptionContext) {
  RecordingHostContext host;
  ps::SerialDebugScheduler scheduler;
  scheduler.attach(host);
  scheduler.start();
  scheduler.submit_initial_task_handles({}, 0);  // Epoch 1.

  scheduler.submit_ready_task_any_thread(
      [&scheduler]() {
        scheduler.log_event(ps::SchedulerTraceAction::Execute, 91);
        throw SerialContextError();
      },
      ps::SchedulerTaskPriority::High, 1U);

  EXPECT_THROW(scheduler.wait_for_completion(), SerialContextError);
  EXPECT_EQ(host.set_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(host.clear_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(host.active_count.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(host.trace_count.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(host.last_worker.load(std::memory_order_relaxed), -1);
  EXPECT_EQ(host.last_epoch.load(std::memory_order_relaxed), 1U);

  scheduler.shutdown();
  scheduler.detach();
}

TEST(SerialDebugSchedulerContract, ConcurrentAnyThreadCallbacksAreSafe) {
  RecordingHostContext host;
  ps::SerialDebugScheduler scheduler;
  scheduler.attach(host);
  scheduler.start();
  scheduler.submit_initial_task_handles({}, 0);  // Epoch 1.

  constexpr int kThreadCount = 8;
  std::atomic<int> callback_count{0};
  std::vector<std::thread> callers;
  callers.reserve(kThreadCount);
  for (int index = 0; index < kThreadCount; ++index) {
    callers.emplace_back([&scheduler, &callback_count]() {
      scheduler.submit_ready_task_any_thread(
          [&callback_count]() {
            callback_count.fetch_add(1, std::memory_order_relaxed);
          },
          ps::SchedulerTaskPriority::Normal, 1U);
    });
  }
  for (std::thread& caller : callers) {
    caller.join();
  }

  EXPECT_EQ(callback_count.load(std::memory_order_relaxed), kThreadCount);
  EXPECT_EQ(host.set_count.load(std::memory_order_relaxed), kThreadCount);
  EXPECT_EQ(host.clear_count.load(std::memory_order_relaxed), kThreadCount);
  EXPECT_EQ(host.active_count.load(std::memory_order_relaxed), 0);

  scheduler.shutdown();
  scheduler.detach();
}

TEST(SerialDebugSchedulerContract, CounterOverflowPreservesActiveCount) {
  RecordingHostContext host;
  ps::SerialDebugScheduler scheduler;
  scheduler.attach(host);
  scheduler.start();
  OverflowProbeExecutor executor(scheduler);

  scheduler.submit_initial_task_handles({{&executor, 0, 20}}, 1);

  EXPECT_TRUE(executor.overflow_observed.load(std::memory_order_relaxed));
  EXPECT_NO_THROW(scheduler.wait_for_completion());

  scheduler.shutdown();
  scheduler.detach();
}

}  // namespace
