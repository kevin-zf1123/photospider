#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

#include "execution/compute_io_executor.hpp"

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
 * @brief Verifies shutdown cancels an admitted factory before queue publish.
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
  EXPECT_TRUE(accepted.accepted());
  if (accepted.accepted()) {
    EXPECT_EQ(accepted.completion().wait().status(),
              ComputeIoCompletionStatus::Cancelled);
  }
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
