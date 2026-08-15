#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "execution/device/compute_io_executor.hpp"

namespace ps::execution {
namespace {

/**
 * @brief Typed exception used to verify exact callback/factory propagation.
 *
 * @throws std::runtime_error construction exceptions unchanged.
 */
class ComputeIoSentinel final : public std::runtime_error {
 public:
  /**
   * @brief Creates one distinguishable executor-test failure.
   * @param message Stable diagnostic text.
   * @throws std::bad_alloc if runtime-error storage cannot allocate.
   */
  explicit ComputeIoSentinel(const char* message)
      : std::runtime_error(message) {}
};

/**
 * @brief Owns deterministic entry and release gates for one blocking callback.
 *
 * The callback captures shared state rather than this helper, so an early test
 * return releases the gate without invalidating provider-owned synchronization.
 *
 * @throws std::bad_alloc or std::system_error from shared/future state setup.
 */
class BlockingIoTask final {
 public:
  /**
   * @brief Creates unresolved entry and release signals.
   * @throws std::bad_alloc or std::system_error from future state setup.
   */
  BlockingIoTask() : state_(std::make_shared<State>()) {}

  /**
   * @brief Releases a blocked callback on early test exit.
   * @throws Nothing; an unexpected promise failure terminates.
   */
  ~BlockingIoTask() noexcept { release(); }

  /**
   * @brief Builds one callback that announces entry and waits for release.
   * @return Copyable executor task retaining all synchronization state.
   * @throws std::bad_alloc when callback storage cannot allocate.
   * @note Exactly one returned callback may execute for this helper.
   */
  ComputeIoExecutor::Task task() const {
    const std::shared_ptr<State> state = state_;
    return [state]() {
      state->entered.set_value();
      state->release_future.wait();
    };
  }

  /**
   * @brief Waits a bounded interval for provider callback entry.
   * @return True when the independent I/O worker entered the callback.
   * @throws std::future_error for a broken test promise.
   */
  bool wait_until_entered() const {
    return state_->entered_future.wait_for(std::chrono::seconds(2)) ==
           std::future_status::ready;
  }

  /**
   * @brief Opens the release gate exactly once.
   * @return Nothing.
   * @throws Nothing; an unexpected promise failure terminates.
   */
  void release() noexcept {
    if (state_->released.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      state_->release.set_value();
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /**
   * @brief Shared synchronization retained independently by the callback.
   *
   * @throws std::system_error or std::future_error during construction.
   */
  struct State final {
    /**
     * @brief Creates shared futures before callback publication.
     * @throws std::future_error if future state construction fails.
     */
    State()
        : entered_future(entered.get_future().share()),
          release_future(release.get_future().share()) {}

    /** @brief Entry signal fulfilled by the independent I/O worker. */
    std::promise<void> entered;

    /** @brief Copyable entry observation used by the test thread. */
    std::shared_future<void> entered_future;

    /** @brief Release signal fulfilled by the test thread. */
    std::promise<void> release;

    /** @brief Copyable release wait retained by the callback. */
    std::shared_future<void> release_future;

    /** @brief Exact-once release guard shared by helper destruction. */
    std::atomic_bool released{false};
  };

  /** @brief Heap state safe across helper/test early return. */
  std::shared_ptr<State> state_;
};

/**
 * @brief Terminates this one GoogleTest process if a regression deadlocks.
 *
 * The watchdog is used only around calls whose historical failure mode was an
 * unrecoverable executor join or worker wait. Each discovered GoogleTest runs
 * in its own CTest process, so a nonzero `_Exit` reports a bounded failure
 * without leaving a detached thread or an executor with dangling references.
 *
 * @throws std::system_error if watchdog-thread construction fails.
 * @note `disarm()` or destruction joins the watchdog. Timeout expiry exits the
 * process immediately and intentionally bypasses stack unwinding.
 */
class ComputeIoTestProcessWatchdog final {
 public:
  /**
   * @brief Starts one bounded process watchdog.
   * @param timeout Maximum duration before forced nonzero process exit.
   * @param exit_code Nonzero code identifying the guarded regression path.
   * @throws std::system_error if thread construction fails.
   */
  ComputeIoTestProcessWatchdog(std::chrono::milliseconds timeout, int exit_code)
      : timeout_(timeout),
        exit_code_(exit_code),
        worker_([this]() noexcept { run(); }) {}

  /**
   * @brief Disarms and joins the watchdog during normal or early test exit.
   * @throws Nothing; unexpected synchronization failure terminates.
   */
  ~ComputeIoTestProcessWatchdog() noexcept { disarm(); }

  /** @brief Watchdog thread ownership cannot be copied. */
  ComputeIoTestProcessWatchdog(const ComputeIoTestProcessWatchdog&) = delete;

  /** @brief Watchdog thread ownership cannot be assigned. */
  ComputeIoTestProcessWatchdog& operator=(const ComputeIoTestProcessWatchdog&) =
      delete;

  /**
   * @brief Publishes successful progress and joins the watchdog exactly once.
   * @return Nothing.
   * @throws Nothing; unexpected synchronization failure terminates.
   */
  void disarm() noexcept {
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        disarmed_ = true;
      }
      condition_.notify_all();
      if (worker_.joinable()) {
        worker_.join();
      }
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /**
   * @brief Waits for disarm or exits the test process after the deadline.
   * @return Nothing when disarmed; timeout never returns.
   * @throws Nothing; synchronization failure uses the same nonzero exit.
   */
  void run() noexcept {
    try {
      std::unique_lock<std::mutex> lock(mutex_);
      const bool was_disarmed =
          condition_.wait_for(lock, timeout_, [this]() { return disarmed_; });
      if (was_disarmed) {
        return;
      }
    } catch (...) {
      std::_Exit(exit_code_);
    }
    std::_Exit(exit_code_);
  }

  /** @brief Serializes the one disarm transition with timeout observation. */
  std::mutex mutex_;

  /** @brief Wakes the watchdog immediately after normal guarded progress. */
  std::condition_variable condition_;

  /** @brief Maximum time allowed for the guarded regression path. */
  const std::chrono::milliseconds timeout_;

  /** @brief Distinct nonzero process code used on timeout or wait failure. */
  const int exit_code_ = 1;

  /** @brief Whether normal progress has cancelled forced process exit. */
  bool disarmed_ = false;

  /** @brief Sole joinable watchdog thread, declared after captured state. */
  std::thread worker_;
};

/**
 * @brief Waits until graceful shutdown has stopped new admission.
 * @param executor Executor whose accepting flag is observed.
 * @return True when admission stopped within the bounded interval.
 * @throws std::system_error when snapshot locking fails.
 * @note Polling is test-only and never participates in production progress.
 */
bool wait_until_admission_stops(const ComputeIoExecutor& executor) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!executor.snapshot().accepting) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return !executor.snapshot().accepting;
}

/**
 * @brief Rejects zero dimensions before an I/O worker can accept work.
 */
TEST(ComputeIoExecutor, RejectsZeroLimits) {
  EXPECT_THROW((void)ComputeIoExecutor(ComputeIoExecutorLimits{0U, 1U}),
               std::invalid_argument);
  EXPECT_THROW((void)ComputeIoExecutor(ComputeIoExecutorLimits{1U, 0U}),
               std::invalid_argument);
}

/**
 * @brief Proves task-capacity rejection does not invoke the lazy factory.
 */
TEST(ComputeIoExecutor, TaskLimitRejectsBeforeLazyFactory) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{1U, 64U});
  BlockingIoTask blocker;
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(1);
  const ComputeIoSubmission first = executor.try_submit(
      16U, lifetime, [&blocker]() { return blocker.task(); });
  EXPECT_TRUE(first.accepted());
  EXPECT_TRUE(blocker.wait_until_entered());

  std::atomic_int factory_entries{0};
  const ComputeIoSubmission rejected = executor.try_submit(
      1U, lifetime, [&factory_entries]() -> ComputeIoExecutor::Task {
        factory_entries.fetch_add(1, std::memory_order_relaxed);
        return []() {};
      });
  EXPECT_EQ(rejected.admission_status(), ComputeIoAdmissionStatus::TaskLimit);
  EXPECT_FALSE(rejected.completion().active());
  EXPECT_EQ(rejected.admission_event().status,
            ComputeIoAdmissionStatus::TaskLimit);
  EXPECT_EQ(rejected.admission_event().offered_planned_bytes, 1U);
  EXPECT_EQ(rejected.admission_event().charged_tasks, 0U);
  EXPECT_EQ(rejected.admission_event().charged_planned_bytes, 0U);
  EXPECT_EQ(rejected.admission_event().snapshot_after.active_tasks, 1U);
  EXPECT_EQ(rejected.admission_event().snapshot_after.active_planned_bytes,
            16U);
  EXPECT_EQ(factory_entries.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 16U);

  blocker.release();
  if (first.accepted()) {
    EXPECT_EQ(first.completion().wait().status(),
              ComputeIoCompletionStatus::Succeeded);
  }
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Proves atomic events retain one task's exact charge amid other work.
 *
 * The target task is admitted and entered first. A second task remains charged
 * while the target settles, so its global post-release snapshot is nonzero.
 * The target settlement must nevertheless bind exactly one task and eleven
 * bytes to its own admission sequence.
 *
 * @throws Test infrastructure exceptions from executor and synchronization.
 */
TEST(ComputeIoExecutor,
     AtomicAdmissionAndSettlementEventsBindOwnChargeUnderConcurrency) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{2U, 32U});
  BlockingIoTask target_gate;
  BlockingIoTask unrelated_gate;
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(15);

  const ComputeIoSubmission target = executor.try_submit(
      11U, lifetime, [&target_gate]() { return target_gate.task(); });
  ASSERT_TRUE(target.accepted());
  ASSERT_TRUE(target_gate.wait_until_entered());
  const ComputeIoAdmissionEvent target_admission = target.admission_event();
  EXPECT_EQ(target_admission.status, ComputeIoAdmissionStatus::Accepted);
  EXPECT_EQ(target_admission.offered_planned_bytes, 11U);
  EXPECT_EQ(target_admission.charged_tasks, 1U);
  EXPECT_EQ(target_admission.charged_planned_bytes, 11U);
  EXPECT_EQ(target_admission.snapshot_after.active_tasks, 1U);
  EXPECT_EQ(target_admission.snapshot_after.active_planned_bytes, 11U);

  const ComputeIoSubmission unrelated = executor.try_submit(
      7U, lifetime, [&unrelated_gate]() { return unrelated_gate.task(); });
  ASSERT_TRUE(unrelated.accepted());
  EXPECT_GT(unrelated.admission_event().sequence, target_admission.sequence);

  target_gate.release();
  const ComputeIoTaskResult target_result = target.completion().wait();
  ASSERT_EQ(target_result.status(), ComputeIoCompletionStatus::Succeeded);
  const ComputeIoSettlementEvent& target_settlement =
      target_result.settlement_event();
  EXPECT_GT(target_settlement.sequence, unrelated.admission_event().sequence);
  EXPECT_EQ(target_settlement.admission_sequence, target_admission.sequence);
  EXPECT_EQ(target_settlement.status, ComputeIoCompletionStatus::Succeeded);
  EXPECT_EQ(target_settlement.released_tasks, 1U);
  EXPECT_EQ(target_settlement.released_planned_bytes, 11U);
  EXPECT_EQ(target_settlement.snapshot_after.active_tasks, 1U);
  EXPECT_EQ(target_settlement.snapshot_after.active_planned_bytes, 7U);

  ASSERT_TRUE(unrelated_gate.wait_until_entered());
  unrelated_gate.release();
  EXPECT_EQ(unrelated.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Proves byte-capacity rejection leaves both accounts unchanged.
 */
TEST(ComputeIoExecutor, PlannedByteLimitRejectsBeforeLazyFactory) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{2U, 16U});
  BlockingIoTask blocker;
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(2);
  const ComputeIoSubmission first = executor.try_submit(
      12U, lifetime, [&blocker]() { return blocker.task(); });
  EXPECT_TRUE(first.accepted());
  EXPECT_TRUE(blocker.wait_until_entered());

  std::atomic_int factory_entries{0};
  const ComputeIoSubmission rejected = executor.try_submit(
      5U, lifetime, [&factory_entries]() -> ComputeIoExecutor::Task {
        factory_entries.fetch_add(1, std::memory_order_relaxed);
        return []() {};
      });
  EXPECT_EQ(rejected.admission_status(),
            ComputeIoAdmissionStatus::PlannedByteLimit);
  EXPECT_EQ(factory_entries.load(std::memory_order_relaxed), 0);
  const ComputeIoExecutorSnapshot charged = executor.snapshot();
  EXPECT_EQ(charged.active_tasks, 1U);
  EXPECT_EQ(charged.active_planned_bytes, 12U);

  blocker.release();
  if (first.accepted()) {
    EXPECT_EQ(first.completion().wait().status(),
              ComputeIoCompletionStatus::Succeeded);
  }
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Verifies factory rollback, callback exception identity, and reuse.
 */
TEST(ComputeIoExecutor, FailuresReleaseCapacityAndWorkerContinues) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{1U, 8U});
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(3);

  EXPECT_THROW(
      (void)executor.try_submit(8U, lifetime,
                                []() -> ComputeIoExecutor::Task {
                                  throw ComputeIoSentinel("factory failure");
                                }),
      ComputeIoSentinel);
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);

  const ComputeIoSubmission failed =
      executor.try_submit(8U, lifetime, []() -> ComputeIoExecutor::Task {
        return []() { throw ComputeIoSentinel("callback failure"); };
      });
  EXPECT_TRUE(failed.accepted());
  if (failed.accepted()) {
    const ComputeIoTaskResult result = failed.completion().wait();
    EXPECT_EQ(result.status(), ComputeIoCompletionStatus::Failed);
    EXPECT_THROW(result.rethrow_if_failed(), ComputeIoSentinel);
  }
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);

  std::atomic_bool recovered{false};
  const ComputeIoSubmission recovery = executor.try_submit(
      8U, lifetime, [&recovered]() -> ComputeIoExecutor::Task {
        return [&recovered]() {
          recovered.store(true, std::memory_order_release);
        };
      });
  EXPECT_TRUE(recovery.accepted());
  if (recovery.accepted()) {
    EXPECT_EQ(recovery.completion().wait().status(),
              ComputeIoCompletionStatus::Succeeded);
  }
  EXPECT_TRUE(recovered.load(std::memory_order_acquire));
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Proves factory failures leave no orphan Accepted event or phase
 * charge.
 * @throws Executor, allocation, synchronization, and test failures unchanged.
 */
TEST(ComputeIoExecutor,
     FactoryThrowAndEmptyTaskRollbackBeforeAcceptedIdentityPublication) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{1U, 8U});
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(31);

  EXPECT_THROW(static_cast<void>(executor.try_submit(
                   8U, lifetime,
                   []() -> ComputeIoExecutor::Task {
                     throw ComputeIoSentinel("provisional factory failure");
                   })),
               ComputeIoSentinel);
  ComputeIoExecutorSnapshot snapshot = executor.snapshot();
  EXPECT_EQ(snapshot.active_tasks, 0U);
  EXPECT_EQ(snapshot.active_planned_bytes, 0U);
  EXPECT_EQ(snapshot.constructing_tasks, 0U);
  EXPECT_EQ(snapshot.queued_tasks, 0U);
  EXPECT_EQ(snapshot.running_tasks, 0U);

  EXPECT_THROW(
      static_cast<void>(executor.try_submit(
          8U, lifetime, []() -> ComputeIoExecutor::Task { return {}; })),
      std::invalid_argument);
  snapshot = executor.snapshot();
  EXPECT_EQ(snapshot.active_tasks, 0U);
  EXPECT_EQ(snapshot.active_planned_bytes, 0U);
  EXPECT_EQ(snapshot.constructing_tasks, 0U);
  EXPECT_EQ(snapshot.queued_tasks, 0U);
  EXPECT_EQ(snapshot.running_tasks, 0U);

  const ComputeIoSubmission accepted = executor.try_submit(
      8U, lifetime, []() -> ComputeIoExecutor::Task { return []() {}; });
  ASSERT_TRUE(accepted.accepted());
  EXPECT_EQ(accepted.admission_event().sequence, 1U);
  EXPECT_EQ(accepted.admission_event().charged_tasks, 1U);
  EXPECT_EQ(accepted.admission_event().charged_planned_bytes, 8U);
  EXPECT_EQ(accepted.admission_event().snapshot_after.active_tasks, 1U);
  EXPECT_EQ(accepted.admission_event().snapshot_after.constructing_tasks, 0U);
  EXPECT_EQ(accepted.admission_event().snapshot_after.queued_tasks, 1U);
  EXPECT_EQ(accepted.admission_event().snapshot_after.running_tasks, 0U);
  const ComputeIoTaskResult settled = accepted.completion().wait();
  EXPECT_EQ(settled.status(), ComputeIoCompletionStatus::Succeeded);
  EXPECT_EQ(settled.settlement_event().sequence, 2U);
  EXPECT_EQ(settled.settlement_event().admission_sequence,
            accepted.admission_event().sequence);
  EXPECT_EQ(settled.settlement_event().snapshot_after.active_tasks, 0U);
  EXPECT_EQ(settled.settlement_event().snapshot_after.constructing_tasks, 0U);
  EXPECT_EQ(settled.settlement_event().snapshot_after.queued_tasks, 0U);
  EXPECT_EQ(settled.settlement_event().snapshot_after.running_tasks, 0U);
}

/**
 * @brief Verifies queued cancellation suppresses callback entry and settles.
 */
TEST(ComputeIoExecutor, QueuedCancellationSkipsWorkAndReleasesExactlyOnce) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{2U, 32U});
  BlockingIoTask blocker;
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(4);
  const ComputeIoSubmission running = executor.try_submit(
      16U, lifetime, [&blocker]() { return blocker.task(); });
  EXPECT_TRUE(running.accepted());
  EXPECT_TRUE(blocker.wait_until_entered());

  std::atomic_int queued_entries{0};
  const ComputeIoSubmission queued = executor.try_submit(
      16U, lifetime, [&queued_entries]() -> ComputeIoExecutor::Task {
        return [&queued_entries]() {
          queued_entries.fetch_add(1, std::memory_order_relaxed);
        };
      });
  EXPECT_TRUE(queued.accepted());
  if (queued.accepted()) {
    EXPECT_TRUE(queued.completion().cancel());
    EXPECT_FALSE(queued.completion().cancel());
  }

  blocker.release();
  if (running.accepted()) {
    EXPECT_EQ(running.completion().wait().status(),
              ComputeIoCompletionStatus::Succeeded);
  }
  if (queued.accepted()) {
    EXPECT_EQ(queued.completion().wait().status(),
              ComputeIoCompletionStatus::Cancelled);
  }
  EXPECT_EQ(queued_entries.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Verifies running cancellation retains lifetime and budgets until exit.
 */
TEST(ComputeIoExecutor, RunningCancellationSettlesOnlyAfterLateReturn) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{1U, 32U});
  BlockingIoTask blocker;
  std::shared_ptr<const void> lifetime = std::make_shared<int>(5);
  const std::weak_ptr<const void> weak_lifetime(lifetime);
  const ComputeIoSubmission running = executor.try_submit(
      32U, lifetime, [&blocker]() { return blocker.task(); });
  EXPECT_TRUE(running.accepted());
  EXPECT_TRUE(blocker.wait_until_entered());
  lifetime.reset();

  if (running.accepted()) {
    EXPECT_TRUE(running.completion().cancel());
  }
  EXPECT_FALSE(weak_lifetime.expired());
  const ComputeIoExecutorSnapshot blocked = executor.snapshot();
  EXPECT_EQ(blocked.running_tasks, 1U);
  EXPECT_EQ(blocked.active_tasks, 1U);
  EXPECT_EQ(blocked.active_planned_bytes, 32U);

  blocker.release();
  if (running.accepted()) {
    EXPECT_EQ(running.completion().wait().status(),
              ComputeIoCompletionStatus::Cancelled);
  }
  EXPECT_TRUE(weak_lifetime.expired());
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Verifies CPU wait prohibition fails before a blocked task completes.
 */
TEST(ComputeIoExecutor, ProhibitedWaitFailsBeforeProviderRelease) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{1U, 8U});
  BlockingIoTask blocker;
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(6);
  const ComputeIoSubmission running = executor.try_submit(
      8U, lifetime, [&blocker]() { return blocker.task(); });
  EXPECT_TRUE(running.accepted());
  EXPECT_TRUE(blocker.wait_until_entered());

  if (running.accepted()) {
    const ComputeIoWaitProhibitionScope prohibited;
    EXPECT_THROW((void)running.completion().wait(), std::logic_error);
    EXPECT_FALSE(running.completion().ready());
  }

  blocker.release();
  if (running.accepted()) {
    EXPECT_EQ(running.completion().wait().status(),
              ComputeIoCompletionStatus::Succeeded);
  }
}

/**
 * @brief Proves same-executor worker submission rejects before lazy work.
 *
 * One callback attempts to submit to its owning single-worker executor and
 * then invokes the returned completion wait exactly as the historical
 * deadlock did. Rejection must be inactive and immediate, leave both budgets
 * unchanged beyond the outer task, and preserve later worker reuse.
 *
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Test infrastructure exceptions from executor or future setup.
 * @note The process watchdog bounds the old queue-behind-self deadlock.
 */
TEST(ComputeIoExecutor, SameWorkerNestedSubmissionAndWaitRejectsBeforeFactory) {
  ComputeIoTestProcessWatchdog watchdog(std::chrono::seconds(5), 81);
  ComputeIoExecutor executor(ComputeIoExecutorLimits{2U, 16U});
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(9);
  std::atomic_int nested_factory_entries{0};
  std::atomic<ComputeIoAdmissionStatus> nested_status{
      ComputeIoAdmissionStatus::Accepted};
  std::atomic_bool nested_wait_rejected{false};
  std::atomic_bool unexpected_wait_failure{false};

  const ComputeIoSubmission outer = executor.try_submit(
      8U, lifetime,
      [&executor, &lifetime, &nested_factory_entries, &nested_status,
       &nested_wait_rejected,
       &unexpected_wait_failure]() -> ComputeIoExecutor::Task {
        return [&executor, &lifetime, &nested_factory_entries, &nested_status,
                &nested_wait_rejected, &unexpected_wait_failure]() {
          const ComputeIoSubmission nested = executor.try_submit(
              1U, lifetime,
              [&nested_factory_entries]() -> ComputeIoExecutor::Task {
                nested_factory_entries.fetch_add(1, std::memory_order_relaxed);
                return []() {};
              });
          nested_status.store(nested.admission_status(),
                              std::memory_order_release);
          try {
            (void)nested.completion().wait();
          } catch (const std::logic_error&) {
            nested_wait_rejected.store(true, std::memory_order_release);
          } catch (...) {
            unexpected_wait_failure.store(true, std::memory_order_release);
          }
        };
      });
  ASSERT_TRUE(outer.accepted());
  EXPECT_EQ(outer.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);

  std::atomic_bool recovery_ran{false};
  const ComputeIoSubmission recovery = executor.try_submit(
      1U, lifetime, [&recovery_ran]() -> ComputeIoExecutor::Task {
        return [&recovery_ran]() {
          recovery_ran.store(true, std::memory_order_release);
        };
      });
  ASSERT_TRUE(recovery.accepted());
  EXPECT_EQ(recovery.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  watchdog.disarm();

  EXPECT_EQ(nested_status.load(std::memory_order_acquire),
            ComputeIoAdmissionStatus::InvalidRequest);
  EXPECT_EQ(nested_factory_entries.load(std::memory_order_relaxed), 0);
  EXPECT_TRUE(nested_wait_rejected.load(std::memory_order_acquire));
  EXPECT_FALSE(unexpected_wait_failure.load(std::memory_order_acquire));
  EXPECT_TRUE(recovery_ran.load(std::memory_order_acquire));
  const ComputeIoExecutorSnapshot drained = executor.snapshot();
  EXPECT_EQ(drained.active_tasks, 0U);
  EXPECT_EQ(drained.active_planned_bytes, 0U);
  EXPECT_EQ(drained.constructing_tasks, 0U);
  EXPECT_EQ(drained.queued_tasks, 0U);
  EXPECT_EQ(drained.running_tasks, 0U);
}

/**
 * @brief Proves the owning worker rejects a preaccepted queued completion.
 *
 * The first callback receives the second task's completion only after that
 * task is queued behind it, then calls `wait()`. The owner-identity guard must
 * fail before condition-variable waiting so both callbacks can settle.
 *
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Test infrastructure exceptions from promises or executor setup.
 * @note The process watchdog bounds the historical worker-self-wait cycle.
 */
TEST(ComputeIoExecutor, OwningWorkerRejectsQueuedCompletionWait) {
  ComputeIoTestProcessWatchdog watchdog(std::chrono::seconds(5), 82);
  ComputeIoExecutor executor(ComputeIoExecutorLimits{2U, 16U});
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(10);
  std::promise<void> first_entered;
  std::shared_future<void> first_entered_future =
      first_entered.get_future().share();
  std::promise<ComputeIoCompletion> queued_completion;
  std::shared_future<ComputeIoCompletion> queued_completion_future =
      queued_completion.get_future().share();
  std::atomic_bool owning_wait_rejected{false};
  std::atomic_bool unexpected_wait_failure{false};

  const ComputeIoSubmission first = executor.try_submit(
      8U, lifetime,
      [&first_entered, queued_completion_future, &owning_wait_rejected,
       &unexpected_wait_failure]() -> ComputeIoExecutor::Task {
        return [&first_entered, queued_completion_future, &owning_wait_rejected,
                &unexpected_wait_failure]() {
          first_entered.set_value();
          const ComputeIoCompletion completion = queued_completion_future.get();
          try {
            (void)completion.wait();
          } catch (const std::logic_error&) {
            owning_wait_rejected.store(true, std::memory_order_release);
          } catch (...) {
            unexpected_wait_failure.store(true, std::memory_order_release);
          }
        };
      });
  ASSERT_TRUE(first.accepted());
  ASSERT_EQ(first_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  std::atomic_bool second_ran{false};
  const ComputeIoSubmission second = executor.try_submit(
      8U, lifetime, [&second_ran]() -> ComputeIoExecutor::Task {
        return [&second_ran]() {
          second_ran.store(true, std::memory_order_release);
        };
      });
  ASSERT_TRUE(second.accepted());
  queued_completion.set_value(second.completion());

  EXPECT_EQ(first.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  EXPECT_EQ(second.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  watchdog.disarm();

  EXPECT_TRUE(owning_wait_rejected.load(std::memory_order_acquire));
  EXPECT_FALSE(unexpected_wait_failure.load(std::memory_order_acquire));
  EXPECT_TRUE(second_ran.load(std::memory_order_acquire));
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Proves the owning worker may copy an already terminal completion.
 *
 * The test settles one seed task before submitting a second callback to the
 * same executor. That callback calls `wait()` on the immutable seed result;
 * because no worker progress or condition-variable wait is required, the
 * exact success fact must remain observable without a logic error.
 *
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Test infrastructure exceptions from executor setup or submission.
 * @note This is the nonblocking counterpart to the queued-completion guard.
 */
TEST(ComputeIoExecutor, OwningWorkerReadsAlreadyTerminalCompletion) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{1U, 8U});
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(11);
  const ComputeIoSubmission seed = executor.try_submit(
      8U, lifetime, []() -> ComputeIoExecutor::Task { return []() {}; });
  ASSERT_TRUE(seed.accepted());
  ASSERT_EQ(seed.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);

  std::atomic_bool observed_terminal_success{false};
  const ComputeIoCompletion seed_completion = seed.completion();
  const ComputeIoSubmission observer = executor.try_submit(
      8U, lifetime,
      [seed_completion,
       &observed_terminal_success]() -> ComputeIoExecutor::Task {
        return [seed_completion, &observed_terminal_success]() {
          if (seed_completion.wait().status() ==
              ComputeIoCompletionStatus::Succeeded) {
            observed_terminal_success.store(true, std::memory_order_release);
          }
        };
      });
  ASSERT_TRUE(observer.accepted());
  EXPECT_EQ(observer.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  EXPECT_TRUE(observed_terminal_success.load(std::memory_order_acquire));
  EXPECT_EQ(executor.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Proves factory shutdown re-entry throws and rolls admission back.
 *
 * The factory lets the exact shutdown rejection escape. Admission rollback
 * must release its construction count and both budgets while leaving normal
 * admission open; a recovery callback then settles before ordinary shutdown
 * drains and joins the worker.
 *
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Test infrastructure exceptions from executor setup.
 * @note The process watchdog bounds the historical construction/join cycle.
 */
TEST(ComputeIoExecutor, FactoryShutdownReentryRollsBackAndRecovers) {
  ComputeIoTestProcessWatchdog watchdog(std::chrono::seconds(5), 83);
  ComputeIoExecutor executor(ComputeIoExecutorLimits{1U, 8U});
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(12);

  EXPECT_THROW(
      (void)executor.try_submit(8U, lifetime,
                                [&executor]() -> ComputeIoExecutor::Task {
                                  executor.shutdown();
                                  return []() {};
                                }),
      std::logic_error);
  const ComputeIoExecutorSnapshot rolled_back = executor.snapshot();
  EXPECT_TRUE(rolled_back.accepting);
  EXPECT_FALSE(rolled_back.shutdown_complete);
  EXPECT_EQ(rolled_back.active_tasks, 0U);
  EXPECT_EQ(rolled_back.active_planned_bytes, 0U);
  EXPECT_EQ(rolled_back.constructing_tasks, 0U);

  std::atomic_bool recovery_ran{false};
  const ComputeIoSubmission recovery = executor.try_submit(
      8U, lifetime, [&recovery_ran]() -> ComputeIoExecutor::Task {
        return [&recovery_ran]() {
          recovery_ran.store(true, std::memory_order_release);
        };
      });
  ASSERT_TRUE(recovery.accepted());
  EXPECT_EQ(recovery.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  EXPECT_TRUE(recovery_ran.load(std::memory_order_acquire));
  EXPECT_NO_THROW(executor.shutdown());
  watchdog.disarm();

  const ComputeIoExecutorSnapshot drained = executor.snapshot();
  EXPECT_TRUE(drained.shutdown_complete);
  EXPECT_EQ(drained.active_tasks, 0U);
  EXPECT_EQ(drained.active_planned_bytes, 0U);
  EXPECT_EQ(drained.constructing_tasks, 0U);
  EXPECT_EQ(drained.queued_tasks, 0U);
  EXPECT_EQ(drained.running_tasks, 0U);
}

/**
 * @brief Proves an I/O worker may submit to and wait for another executor.
 *
 * Executor A's callback lazily submits one callback to independent executor B
 * and synchronously consumes B's completion. Identity-specific admission and
 * wait guards must preserve this legal nesting while each executor releases
 * its own task and byte charge exactly once.
 *
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Test infrastructure exceptions from executor setup or submission.
 * @note The process watchdog bounds any accidental global worker prohibition.
 */
TEST(ComputeIoExecutor, DifferentExecutorNestedSubmissionAndWaitSucceeds) {
  ComputeIoTestProcessWatchdog watchdog(std::chrono::seconds(5), 84);
  ComputeIoExecutor executor_a(ComputeIoExecutorLimits{1U, 8U});
  ComputeIoExecutor executor_b(ComputeIoExecutorLimits{1U, 8U});
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(13);
  std::atomic_bool inner_factory_ran{false};
  std::atomic_bool inner_callback_ran{false};
  std::atomic_bool outer_observed_success{false};

  const ComputeIoSubmission outer =
      executor_a.try_submit(8U, lifetime, [&]() -> ComputeIoExecutor::Task {
        return [&]() {
          const ComputeIoSubmission inner = executor_b.try_submit(
              8U, lifetime, [&]() -> ComputeIoExecutor::Task {
                inner_factory_ran.store(true, std::memory_order_release);
                return [&inner_callback_ran]() {
                  inner_callback_ran.store(true, std::memory_order_release);
                };
              });
          if (inner.accepted() && inner.completion().wait().status() ==
                                      ComputeIoCompletionStatus::Succeeded) {
            outer_observed_success.store(true, std::memory_order_release);
          }
        };
      });
  ASSERT_TRUE(outer.accepted());
  EXPECT_EQ(outer.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  watchdog.disarm();

  EXPECT_TRUE(inner_factory_ran.load(std::memory_order_acquire));
  EXPECT_TRUE(inner_callback_ran.load(std::memory_order_acquire));
  EXPECT_TRUE(outer_observed_success.load(std::memory_order_acquire));
  EXPECT_EQ(executor_a.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor_a.snapshot().active_planned_bytes, 0U);
  EXPECT_EQ(executor_b.snapshot().active_tasks, 0U);
  EXPECT_EQ(executor_b.snapshot().active_planned_bytes, 0U);
}

/**
 * @brief Proves nested factory tracking is identity-specific and stack-safe.
 *
 * An A factory enters a B factory. The inner frame must reject shutdown of
 * both active executors A and B, allow shutdown of unrelated executor C, and
 * restore A as the active frame after B returns. The accepted A callback then
 * waits legally for B's completion on its independent worker.
 *
 * @return Nothing; GoogleTest records every contract mismatch.
 * @throws Test infrastructure exceptions from executor setup or submission.
 * @note The process watchdog bounds a missing outer-frame or inner-frame
 * shutdown guard while also proving different-executor nesting remains legal.
 */
TEST(ComputeIoExecutor,
     NestedFactoriesRejectActiveShutdownAndAllowDifferentExecutorWait) {
  ComputeIoTestProcessWatchdog watchdog(std::chrono::seconds(5), 85);
  ComputeIoExecutor executor_a(ComputeIoExecutorLimits{1U, 8U});
  ComputeIoExecutor executor_b(ComputeIoExecutorLimits{1U, 8U});
  ComputeIoExecutor executor_c(ComputeIoExecutorLimits{1U, 8U});
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(14);
  std::atomic_bool rejected_outer_from_inner{false};
  std::atomic_bool rejected_inner{false};
  std::atomic_bool rejected_outer_after_restore{false};
  std::atomic_bool unrelated_shutdown_succeeded{false};
  std::atomic_bool inner_callback_ran{false};
  std::atomic_bool outer_observed_inner_success{false};
  std::atomic_bool unexpected_factory_failure{false};

  const ComputeIoSubmission outer =
      executor_a.try_submit(8U, lifetime, [&]() -> ComputeIoExecutor::Task {
        const ComputeIoSubmission inner = executor_b.try_submit(
            8U, lifetime, [&]() -> ComputeIoExecutor::Task {
              try {
                executor_c.shutdown();
                unrelated_shutdown_succeeded.store(true,
                                                   std::memory_order_release);
              } catch (...) {
                unexpected_factory_failure.store(true,
                                                 std::memory_order_release);
              }
              try {
                executor_a.shutdown();
              } catch (const std::logic_error&) {
                rejected_outer_from_inner.store(true,
                                                std::memory_order_release);
              } catch (...) {
                unexpected_factory_failure.store(true,
                                                 std::memory_order_release);
              }
              try {
                executor_b.shutdown();
              } catch (const std::logic_error&) {
                rejected_inner.store(true, std::memory_order_release);
              } catch (...) {
                unexpected_factory_failure.store(true,
                                                 std::memory_order_release);
              }
              return [&inner_callback_ran]() {
                inner_callback_ran.store(true, std::memory_order_release);
              };
            });
        try {
          executor_a.shutdown();
        } catch (const std::logic_error&) {
          rejected_outer_after_restore.store(true, std::memory_order_release);
        } catch (...) {
          unexpected_factory_failure.store(true, std::memory_order_release);
        }
        const ComputeIoCompletion inner_completion = inner.completion();
        return [inner_completion, &outer_observed_inner_success,
                &unexpected_factory_failure]() {
          try {
            if (inner_completion.active() &&
                inner_completion.wait().status() ==
                    ComputeIoCompletionStatus::Succeeded) {
              outer_observed_inner_success.store(true,
                                                 std::memory_order_release);
            }
          } catch (...) {
            unexpected_factory_failure.store(true, std::memory_order_release);
          }
        };
      });
  ASSERT_TRUE(outer.accepted());
  EXPECT_EQ(outer.completion().wait().status(),
            ComputeIoCompletionStatus::Succeeded);
  EXPECT_NO_THROW(executor_a.shutdown());
  EXPECT_NO_THROW(executor_b.shutdown());
  EXPECT_NO_THROW(executor_c.shutdown());
  watchdog.disarm();

  EXPECT_TRUE(rejected_outer_from_inner.load(std::memory_order_acquire));
  EXPECT_TRUE(rejected_inner.load(std::memory_order_acquire));
  EXPECT_TRUE(rejected_outer_after_restore.load(std::memory_order_acquire));
  EXPECT_TRUE(unrelated_shutdown_succeeded.load(std::memory_order_acquire));
  EXPECT_TRUE(inner_callback_ran.load(std::memory_order_acquire));
  EXPECT_TRUE(outer_observed_inner_success.load(std::memory_order_acquire));
  EXPECT_FALSE(unexpected_factory_failure.load(std::memory_order_acquire));
  EXPECT_TRUE(executor_a.snapshot().shutdown_complete);
  EXPECT_TRUE(executor_b.snapshot().shutdown_complete);
  EXPECT_TRUE(executor_c.snapshot().shutdown_complete);
}

/**
 * @brief Verifies shutdown atomically accepts and cancels constructed work.
 * @throws Executor, allocation, synchronization, and test failures unchanged.
 */
TEST(ComputeIoExecutor, ShutdownRacingConstructionCancelsBeforeCallbackEntry) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{1U, 8U});
  BlockingIoTask factory_gate;
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(7);
  std::atomic_int callback_entries{0};

  std::future<ComputeIoSubmission> submission = std::async(
      std::launch::async,
      [&executor, &factory_gate, &callback_entries, lifetime]() {
        return executor.try_submit(
            8U, lifetime,
            [&factory_gate, &callback_entries]() -> ComputeIoExecutor::Task {
              ComputeIoExecutor::Task gate = factory_gate.task();
              gate();
              return [&callback_entries]() {
                callback_entries.fetch_add(1, std::memory_order_relaxed);
              };
            });
      });
  EXPECT_TRUE(factory_gate.wait_until_entered());

  const ComputeIoExecutorSnapshot constructing = executor.snapshot();
  EXPECT_EQ(constructing.constructing_tasks, 1U);
  EXPECT_EQ(constructing.active_tasks, 1U);
  EXPECT_EQ(constructing.active_planned_bytes, 8U);

  std::future<void> shutdown =
      std::async(std::launch::async, [&executor]() { executor.shutdown(); });
  EXPECT_TRUE(wait_until_admission_stops(executor));
  EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  factory_gate.release();
  EXPECT_EQ(submission.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ComputeIoSubmission accepted = submission.get();
  ASSERT_TRUE(accepted.accepted());
  const ComputeIoAdmissionEvent& admission = accepted.admission_event();
  EXPECT_EQ(admission.status, ComputeIoAdmissionStatus::Accepted);
  EXPECT_EQ(admission.sequence, 1U);
  EXPECT_EQ(admission.offered_planned_bytes, 8U);
  EXPECT_EQ(admission.charged_tasks, 1U);
  EXPECT_EQ(admission.charged_planned_bytes, 8U);
  EXPECT_EQ(admission.snapshot_after.task_limit, 1U);
  EXPECT_EQ(admission.snapshot_after.planned_bytes_limit, 8U);
  EXPECT_EQ(admission.snapshot_after.active_tasks, 1U);
  EXPECT_EQ(admission.snapshot_after.active_planned_bytes, 8U);
  EXPECT_EQ(admission.snapshot_after.constructing_tasks, 1U);
  EXPECT_EQ(admission.snapshot_after.queued_tasks, 0U);
  EXPECT_EQ(admission.snapshot_after.running_tasks, 0U);
  EXPECT_FALSE(admission.snapshot_after.accepting);
  EXPECT_FALSE(admission.snapshot_after.shutdown_complete);

  const ComputeIoTaskResult cancelled = accepted.completion().wait();
  EXPECT_EQ(cancelled.status(), ComputeIoCompletionStatus::Cancelled);
  EXPECT_EQ(cancelled.work_duration(), std::chrono::nanoseconds{0});
  EXPECT_FALSE(cancelled.failure());
  const ComputeIoSettlementEvent& settlement = cancelled.settlement_event();
  EXPECT_EQ(settlement.status, ComputeIoCompletionStatus::Cancelled);
  EXPECT_EQ(settlement.sequence, 2U);
  EXPECT_EQ(settlement.admission_sequence, admission.sequence);
  EXPECT_EQ(settlement.released_tasks, 1U);
  EXPECT_EQ(settlement.released_planned_bytes, 8U);
  EXPECT_EQ(settlement.snapshot_after.task_limit, 1U);
  EXPECT_EQ(settlement.snapshot_after.planned_bytes_limit, 8U);
  EXPECT_EQ(settlement.snapshot_after.active_tasks, 0U);
  EXPECT_EQ(settlement.snapshot_after.active_planned_bytes, 0U);
  EXPECT_EQ(settlement.snapshot_after.constructing_tasks, 0U);
  EXPECT_EQ(settlement.snapshot_after.queued_tasks, 0U);
  EXPECT_EQ(settlement.snapshot_after.running_tasks, 0U);
  EXPECT_FALSE(settlement.snapshot_after.accepting);
  EXPECT_FALSE(settlement.snapshot_after.shutdown_complete);
  EXPECT_EQ(callback_entries.load(std::memory_order_relaxed), 0);

  EXPECT_EQ(shutdown.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  if (shutdown.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    EXPECT_NO_THROW(shutdown.get());
  }
  const ComputeIoExecutorSnapshot drained = executor.snapshot();
  EXPECT_TRUE(drained.shutdown_complete);
  EXPECT_EQ(drained.constructing_tasks, 0U);
  EXPECT_EQ(drained.queued_tasks, 0U);
  EXPECT_EQ(drained.running_tasks, 0U);
  EXPECT_EQ(drained.active_tasks, 0U);
  EXPECT_EQ(drained.active_planned_bytes, 0U);
}

/**
 * @brief Verifies shutdown rejects late work, drains, joins, and is idempotent.
 */
TEST(ComputeIoExecutor, ShutdownWaitsForAcceptedWorkAndDrainsBudgets) {
  ComputeIoExecutor executor(ComputeIoExecutorLimits{2U, 16U});
  BlockingIoTask blocker;
  const std::shared_ptr<const void> lifetime = std::make_shared<int>(8);
  const ComputeIoSubmission running = executor.try_submit(
      8U, lifetime, [&blocker]() { return blocker.task(); });
  EXPECT_TRUE(running.accepted());
  EXPECT_TRUE(blocker.wait_until_entered());

  std::future<void> shutdown =
      std::async(std::launch::async, [&executor]() { executor.shutdown(); });
  EXPECT_TRUE(wait_until_admission_stops(executor));
  EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  std::atomic_int factory_entries{0};
  const ComputeIoSubmission rejected = executor.try_submit(
      1U, lifetime, [&factory_entries]() -> ComputeIoExecutor::Task {
        factory_entries.fetch_add(1, std::memory_order_relaxed);
        return []() {};
      });
  EXPECT_EQ(rejected.admission_status(),
            ComputeIoAdmissionStatus::ShuttingDown);
  EXPECT_EQ(factory_entries.load(std::memory_order_relaxed), 0);

  blocker.release();
  EXPECT_EQ(shutdown.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  if (shutdown.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    EXPECT_NO_THROW(shutdown.get());
  }
  if (running.accepted()) {
    EXPECT_EQ(running.completion().wait().status(),
              ComputeIoCompletionStatus::Succeeded);
  }
  const ComputeIoExecutorSnapshot drained = executor.snapshot();
  EXPECT_TRUE(drained.shutdown_complete);
  EXPECT_EQ(drained.constructing_tasks, 0U);
  EXPECT_EQ(drained.queued_tasks, 0U);
  EXPECT_EQ(drained.running_tasks, 0U);
  EXPECT_EQ(drained.active_tasks, 0U);
  EXPECT_EQ(drained.active_planned_bytes, 0U);
  EXPECT_NO_THROW(executor.shutdown());
}

}  // namespace
}  // namespace ps::execution
