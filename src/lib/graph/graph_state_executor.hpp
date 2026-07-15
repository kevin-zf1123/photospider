#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
#include "graph/graph_state_executor_test_access.hpp"  // NOLINT(build/include_subdir)
#endif

namespace ps {

/**
 * @brief Serializes all visible GraphModel access for one graph runtime.
 *
 * One executor-owned worker removes submissions from a FIFO queue and invokes
 * each callable against the borrowed model. Result values and exceptions are
 * delivered through submission futures while queue ownership remains with the
 * executor until work completes.
 *
 * @throws std::system_error if worker creation or synchronization fails.
 * @note The model owner must outlive this executor. Destruction stops
 * admission, drains admitted work, and joins the sole worker before releasing
 * queue ownership.
 */
class GraphStateExecutor {
 public:
  /**
   * @brief Maximum number of waiting tasks owned by a production lane.
   * @note The at-most-one task currently executing is excluded from this
   *       capacity, so default total admitted work is at most 65 tasks.
   */
  static constexpr std::size_t kDefaultQueueCapacity = 64;

  /**
   * @brief Binds an executor to one model with the same owning lifetime.
   * @param model Model serialized by every submitted callable.
   * @param queue_capacity Maximum waiting tasks, excluding active work.
   *        Production callers use `kDefaultQueueCapacity`; injection exists for
   *        deterministic internal tests.
   * @throws std::invalid_argument if queue_capacity is zero.
   * @throws std::system_error if the sole worker cannot be created.
   * @note Worker ownership becomes active before construction returns.
   */
  explicit GraphStateExecutor(
      GraphModel& model, std::size_t queue_capacity = kDefaultQueueCapacity);

  /**
   * @brief Drains admitted work and joins the sole executor worker.
   * @throws Nothing.
   * @note New submissions are not permitted to race destruction. The owner must
   *       begin destruction only after external admission has stopped.
   */
  ~GraphStateExecutor() noexcept;

  /**
   * @brief Stops new admission without waiting for admitted work or the worker.
   * @return Nothing.
   * @throws std::logic_error if called from this lane's own worker.
   * @throws std::overflow_error if the monotonic close generation is exhausted.
   * @throws std::system_error if lifecycle synchronization fails.
   * @note This is the first phase of graph close. It atomically changes an
   *       accepting lane to draining, wakes producers blocked on the bounded
   *       FIFO, and leaves already admitted work owned by the sole worker.
   *       Concurrent and repeated calls are idempotent. The owning lifecycle
   *       must later call `close_and_drain()` before model or scheduler
   *       teardown.
   */
  void stop_admission();

  /**
   * @brief Stops admission, drains admitted FIFO work, and joins the worker.
   * @return Nothing.
   * @throws std::logic_error if called from this lane's own worker.
   * @throws std::overflow_error if starting a new close generation would wrap.
   * @throws std::system_error if lifecycle synchronization or worker join
   * fails.
   * @note Concurrent calls capture the same close generation and each returns
   *       only after that generation's worker join is durably recorded.
   *       Failed-close restart may reopen a later generation before every
   *       waiter resumes; the completed-generation record still releases those
   *       original waiters. Repeated calls on a closed generation are
   *       idempotent. Producers blocked on a full queue are awakened and
   *       rejected.
   */
  void close_and_drain();

  /**
   * @brief Recreates the sole worker after scheduler shutdown aborts close.
   * @return Nothing.
   * @throws std::logic_error if called from this lane worker or before the
   *         prior worker has reached the fully joined `Closed` state.
   * @throws std::system_error if replacement worker creation fails.
   * @note Calling this while already accepting is idempotent. The method is a
   *       narrow Kernel close-rollback operation, not a general public restart
   *       facility; it never restores discarded tasks because close drained all
   *       prior admissions before scheduler shutdown began. Completion of the
   *       joined generation remains recorded so its delayed close waiters do
   *       not wait on this replacement worker.
   */
  void restart_after_close_failure();

  /**
   * @brief Disables copying because mutex ownership cannot be duplicated.
   * @param other Executor that retains its model and mutex.
   * @throws Nothing because construction is unavailable.
   */
  GraphStateExecutor(const GraphStateExecutor&) = delete;

  /**
   * @brief Disables copy assignment because the bound model cannot change.
   * @param other Executor that retains its model and mutex.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  GraphStateExecutor& operator=(const GraphStateExecutor&) = delete;

  /**
   * @brief Admits one callable for FIFO execution on the sole worker.
   *
   * @tparam Fn Callable invocable as `fn(GraphModel&)`.
   * @param fn Callable captured by value into executor-owned task storage.
   * @return Future carrying the callable's exact result or exception.
   * @throws std::bad_alloc if task/future capture allocation exhausts memory.
   * @throws std::logic_error if called from this lane's own worker.
   * @throws std::runtime_error if executor admission has stopped.
   * @throws std::system_error if queue synchronization fails.
   * @note Submission blocks while `tasks_` contains `queue_capacity_` waiting
   *       tasks. Destroying the returned future does not cancel or wait for
   *       admitted work; the executor retains ownership through invocation.
   */
  template <typename Fn>
  auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn, GraphModel&>> {
    reject_worker_reentry("submit");
    using Ret = std::invoke_result_t<Fn, GraphModel&>;
    std::packaged_task<Ret()> result_task(
        [this, f = std::forward<Fn>(fn)]() mutable -> Ret {
          if constexpr (std::is_void_v<Ret>) {
            std::invoke(f, model_);
          } else {
            return std::invoke(f, model_);
          }
        });
    std::future<Ret> result = result_task.get_future();
    std::packaged_task<void()> queue_task(
        [task = std::move(result_task)]() mutable { task(); });
    enqueue(std::move(queue_task));
    return result;
  }

 private:
  /**
   * @brief Internal close/drain phases protected by `mutex_`.
   * @throws Nothing for value construction and comparison.
   */
  enum class State {
    /** @brief New submissions may enter the bounded FIFO. */
    Accepting,
    /** @brief Admission stopped while the worker drains accepted work. */
    Draining,
    /** @brief Worker returned; one closer still has to join it. */
    WorkerStopped,
    /** @brief Worker joined and all lifecycle resources are quiescent. */
    Closed,
  };

  /**
   * @brief Monotonic identity of one admission-stop/drain/join cycle.
   * @throws Nothing for value construction and comparison.
   * @note Generation zero denotes the initial accepting lane before any close.
   *       Values never wrap; exhaustion rejects a new close cycle.
   */
  using CloseGeneration = std::uint64_t;

  /**
   * @brief Stops admission and returns the exact close generation observed.
   * @param operation Stable caller name used for worker-reentry diagnostics.
   * @return Existing in-flight/closed generation, or a newly created
   *         generation when this call changes `Accepting` to `Draining`.
   * @throws std::logic_error if called from this lane's own worker.
   * @throws std::overflow_error if a new generation cannot be represented.
   * @throws std::system_error if lifecycle synchronization fails.
   * @note The returned token remains a completed-history identity after a
   *       failed-close restart opens a later accepting generation. This lets
   *       every close waiter finish its original cycle even when restart wins
   *       the lifecycle mutex before that waiter wakes.
   */
  CloseGeneration stop_admission_for_close(const char* operation);

  /**
   * @brief Appends one type-erased callable to executor-owned FIFO storage.
   * @param task Task whose result state is owned by the caller's future.
   * @return Nothing.
   * @throws std::bad_alloc if queue storage cannot grow.
   * @throws std::runtime_error if destruction has stopped admission.
   * @throws std::system_error if queue synchronization fails.
   * @note Publication and worker notification occur while this executor owns
   *       the complete move-only task.
   */
  void enqueue(std::packaged_task<void()> task);

  /**
   * @brief Removes and invokes admitted tasks until destruction drains the
   * FIFO.
   * @return Nothing.
   * @throws Nothing from submitted callables because packaged tasks capture
   *         callable failures in their result states.
   * @note This routine runs only on `worker_`; it never exposes model access to
   *       a second executor thread.
   */
  void worker_loop();

  /**
   * @brief Rejects one lifecycle-sensitive call from this executor's worker.
   * @param operation Stable operation name used in the diagnostic.
   * @return Nothing.
   * @throws std::logic_error when called on `worker_`.
   * @note Detection occurs before task allocation, queue locking, or capacity
   *       waiting so behavior does not depend on current queue occupancy.
   */
  void reject_worker_reentry(const char* operation) const;

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
  /**
   * @brief Publishes one locked lane snapshot through the BUILD_TESTING seam.
   * @param event Lifecycle checkpoint represented by the snapshot.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller must hold `mutex_`. The installed callback may inspect scalar
   *       values only and must not allocate, block, or re-enter this executor.
   */
  void publish_test_snapshot_locked(
      testing::GraphStateExecutorTestEvent event) const noexcept;
#endif

  /** @brief Borrowed model whose owner also owns this executor. */
  GraphModel& model_;
  /** @brief Protects FIFO ownership and the admission-stop flag. */
  std::mutex mutex_;
  /** @brief Wakes the worker when work arrives or destruction begins. */
  std::condition_variable work_available_;
  /** @brief Wakes blocked producers when one queued task becomes active. */
  std::condition_variable space_available_;
  /** @brief Coordinates worker-stop and generation completion among closers. */
  std::condition_variable state_changed_;
  /** @brief Executor-owned FIFO of admitted move-only callables. */
  std::deque<std::packaged_task<void()>> tasks_;
  /** @brief Sole thread permitted to invoke submitted graph-state callables. */
  std::thread worker_;
  /** @brief Fixed bound for waiting tasks; active work is excluded. */
  const std::size_t queue_capacity_;
  /** @brief Current admission/drain/join phase guarded by `mutex_`. */
  State state_ = State::Accepting;
  /** @brief Latest close cycle created by an admission-stop transition. */
  CloseGeneration close_generation_ = 0;
  /** @brief Latest generation whose worker join was published successfully. */
  CloseGeneration completed_close_generation_ = 0;
  /** @brief Number of currently executing tasks; invariant is zero or one. */
  std::size_t active_task_count_ = 0;
  /** @brief Number of live executor worker loops; invariant is zero or one. */
  std::size_t worker_thread_count_ = 0;
  /** @brief Whether one close caller currently owns the worker join. */
  bool join_in_progress_ = false;
  /** @brief Executor currently owning the calling lane-worker thread. */
  static thread_local const GraphStateExecutor* current_executor_;
};

}  // namespace ps
