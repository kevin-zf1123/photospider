#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace ps::execution {

/** @brief Opaque shared state retained by one compute-I/O completion. */
struct ComputeIoTaskState;

/** @brief Opaque process executor state shared with its independent worker. */
struct ComputeIoExecutorState;

/**
 * @brief Immutable private limits for one process compute-I/O executor.
 *
 * @throws Nothing for value construction.
 * @note These dimensions are independent from Host `ResourceLedger`, device
 * accounts, CPU/GPU routes, and scheduler ready-store capacity.
 */
struct ComputeIoExecutorLimits final {
  /** @brief Maximum admitted, constructing, queued, and running tasks. */
  std::uint64_t task_limit = 0U;

  /** @brief Maximum summed estimated bytes for the same active task set. */
  std::uint64_t planned_bytes_limit = 0U;
};

/**
 * @brief Typed result of one compute-I/O admission attempt.
 *
 * @throws Nothing.
 * @note Rejection never invokes the payload factory or changes executor
 * accounting.
 */
enum class ComputeIoAdmissionStatus : std::uint8_t {
  /** @brief Both budgets committed and a completion fact was created. */
  Accepted,
  /** @brief Estimate or lifetime-token input was invalid. */
  InvalidRequest,
  /** @brief The active task-count limit denied admission. */
  TaskLimit,
  /** @brief Planned bytes overflowed or exceeded remaining capacity. */
  PlannedByteLimit,
  /** @brief Executor shutdown already stopped new admission. */
  ShuttingDown,
};

/**
 * @brief Returns a stable private diagnostic name for one admission result.
 * @param status Admission status to name.
 * @return Process-lifetime lowercase identifier.
 * @throws Nothing.
 */
const char* compute_io_admission_status_name(
    ComputeIoAdmissionStatus status) noexcept;

/**
 * @brief Typed terminal state for one accepted compute-I/O task.
 *
 * @throws Nothing.
 * @note Cancellation of running provider work suppresses its late result but
 * cannot forcibly interrupt the provider call.
 */
enum class ComputeIoCompletionStatus : std::uint8_t {
  /** @brief Callback returned normally before cancellation won. */
  Succeeded,
  /** @brief Callback threw and no cancellation won before settlement. */
  Failed,
  /** @brief Queued work was skipped or running work returned after cancel. */
  Cancelled,
};

/**
 * @brief Immutable typed completion fact for one accepted compute-I/O task.
 *
 * The result carries the exact terminal category, original callback exception
 * identity when failed, and time spent inside the I/O worker callback. It owns
 * no queue, task budget, byte budget, lifetime token, or Graph state.
 *
 * @throws Nothing for copying and observation.
 */
class ComputeIoTaskResult final {
 public:
  /**
   * @brief Returns the exact terminal category.
   * @return Succeeded, Failed, or Cancelled.
   * @throws Nothing.
   */
  ComputeIoCompletionStatus status() const noexcept { return status_; }

  /**
   * @brief Returns time spent inside the worker callback.
   * @return Nonnegative steady-clock duration; zero for skipped queued work.
   * @throws Nothing.
   */
  std::chrono::nanoseconds work_duration() const noexcept {
    return work_duration_;
  }

  /**
   * @brief Returns the original callback exception identity.
   * @return Non-null only for `Failed`.
   * @throws Nothing.
   */
  std::exception_ptr failure() const noexcept { return failure_; }

  /**
   * @brief Rethrows the original callback exception when failed.
   * @return Nothing.
   * @throws The exact callback exception for `Failed`; nothing otherwise.
   */
  void rethrow_if_failed() const;

 private:
  /**
   * @brief Creates one executor-owned terminal fact.
   * @param status Exact terminal category.
   * @param failure Original callback exception for failure.
   * @param work_duration Time spent inside the callback.
   * @throws Nothing.
   */
  ComputeIoTaskResult(ComputeIoCompletionStatus status,
                      std::exception_ptr failure,
                      std::chrono::nanoseconds work_duration) noexcept
      : status_(status),
        failure_(std::move(failure)),
        work_duration_(work_duration) {}

  /** @brief Exact terminal category. */
  ComputeIoCompletionStatus status_ = ComputeIoCompletionStatus::Cancelled;

  /** @brief Original callback exception, or null outside Failed. */
  std::exception_ptr failure_;

  /** @brief Time spent in the independent worker callback. */
  std::chrono::nanoseconds work_duration_{0};

  friend class ComputeIoExecutor;
  friend struct ComputeIoExecutorState;
  friend struct ComputeIoTaskState;
};

/**
 * @brief Copyable non-owning observation of one accepted task's terminal fact.
 *
 * Copies share only completion state. Task payload, lifetime token, and both
 * executor charges retire at task settlement rather than completion-handle
 * destruction.
 *
 * @throws Nothing for default/copy/move/destruction.
 * @note `wait()` is forbidden from an `ExecutionService` CPU worker. Such
 * workers must progress through nonblocking coordination instead.
 */
class ComputeIoCompletion final {
 public:
  /** @brief Creates an inactive completion for a rejected submission. */
  ComputeIoCompletion() noexcept = default;

  /**
   * @brief Reports whether this handle observes an accepted task.
   * @return True when shared completion state exists.
   * @throws Nothing.
   */
  bool active() const noexcept { return state_ != nullptr; }

  /**
   * @brief Polls terminal readiness without waiting.
   * @return True only when an accepted task has settled.
   * @throws std::system_error if completion locking fails.
   * @note This method is valid from CPU workers because it never blocks for
   * provider completion.
   */
  bool ready() const;

  /**
   * @brief Copies a terminal result without waiting.
   * @return Result when settled, otherwise `std::nullopt`.
   * @throws std::logic_error for an inactive completion.
   * @throws std::system_error if completion locking fails.
   */
  std::optional<ComputeIoTaskResult> try_result() const;

  /**
   * @brief Waits for and copies the exact terminal result.
   * @return Succeeded, Failed, or Cancelled completion fact.
   * @throws std::logic_error for an inactive handle or when called by an
   * `ExecutionService` CPU worker.
   * @throws std::system_error if completion waiting fails.
   * @note The wait grants no Graph mutation, persistence, or retry authority.
   */
  ComputeIoTaskResult wait() const;

  /**
   * @brief Requests cancellation without interrupting running provider code.
   * @return True only for the first request published before terminal state.
   * @throws std::system_error if completion locking fails.
   * @note Queued callbacks are skipped. Running callbacks retain payload,
   * lifetime token, and budgets until their late return, then settle Cancelled.
   */
  bool cancel() const;

 private:
  /**
   * @brief Creates one observation over accepted task state.
   * @param state Shared completion state.
   * @throws Nothing.
   */
  explicit ComputeIoCompletion(
      std::shared_ptr<ComputeIoTaskState> state) noexcept
      : state_(std::move(state)) {}

  /** @brief Shared terminal state, empty for rejected submissions. */
  std::shared_ptr<ComputeIoTaskState> state_;

  friend class ComputeIoExecutor;
};

/**
 * @brief Value returned by one lazy compute-I/O admission attempt.
 *
 * @throws Nothing for observation and movement.
 * @note Accepted submissions always carry an active completion. Rejected
 * submissions always carry an inactive completion.
 */
class ComputeIoSubmission final {
 public:
  /** @brief Creates an invalid-request result with no completion. */
  ComputeIoSubmission() noexcept = default;

  /**
   * @brief Returns the exact admission category.
   * @return Accepted or one typed rejection reason.
   * @throws Nothing.
   */
  ComputeIoAdmissionStatus admission_status() const noexcept { return status_; }

  /**
   * @brief Reports whether both budgets committed.
   * @return True exactly for Accepted.
   * @throws Nothing.
   */
  bool accepted() const noexcept {
    return status_ == ComputeIoAdmissionStatus::Accepted;
  }

  /**
   * @brief Returns the accepted task's completion observation.
   * @return Active completion for Accepted, inactive otherwise.
   * @throws Nothing.
   */
  const ComputeIoCompletion& completion() const noexcept { return completion_; }

 private:
  /**
   * @brief Creates one typed submission result.
   * @param status Exact admission category.
   * @param completion Active only for Accepted.
   * @throws Nothing.
   */
  ComputeIoSubmission(ComputeIoAdmissionStatus status,
                      ComputeIoCompletion completion) noexcept
      : status_(status), completion_(std::move(completion)) {}

  /** @brief Exact admission category. */
  ComputeIoAdmissionStatus status_ = ComputeIoAdmissionStatus::InvalidRequest;

  /** @brief Accepted task completion, otherwise inactive. */
  ComputeIoCompletion completion_;

  friend class ComputeIoExecutor;
};

/**
 * @brief Immutable observation of bounded executor state.
 *
 * @throws Nothing for value construction.
 * @note This grants no admission, cancellation, queue, or worker authority.
 */
struct ComputeIoExecutorSnapshot final {
  /** @brief Configured immutable task limit. */
  std::uint64_t task_limit = 0U;
  /** @brief Configured immutable planned-byte limit. */
  std::uint64_t planned_bytes_limit = 0U;
  /** @brief Currently charged tasks across every active phase. */
  std::uint64_t active_tasks = 0U;
  /** @brief Currently charged estimated bytes. */
  std::uint64_t active_planned_bytes = 0U;
  /** @brief Admitted tasks whose lazy factory/queue publication is ongoing. */
  std::uint64_t constructing_tasks = 0U;
  /** @brief Published FIFO entries not yet selected by the worker. */
  std::uint64_t queued_tasks = 0U;
  /** @brief Callbacks currently entered on the sole I/O worker. */
  std::uint64_t running_tasks = 0U;
  /** @brief Whether new submissions may still be admitted. */
  bool accepting = false;
  /** @brief Whether graceful shutdown joined the worker. */
  bool shutdown_complete = false;
};

/**
 * @brief Marks one CPU worker thread as forbidden from blocking on compute I/O.
 *
 * Nested scopes are counted per thread. Construction and destruction perform
 * no allocation or synchronization.
 *
 * @throws Nothing.
 * @note `ExecutionService::worker_loop` installs this only for the CPU lane.
 */
class ComputeIoWaitProhibitionScope final {
 public:
  /**
   * @brief Optionally enters the current thread's prohibition scope.
   * @param active Whether this scope should change the thread-local depth.
   * @throws Nothing; depth overflow terminates as an internal invariant.
   */
  explicit ComputeIoWaitProhibitionScope(bool active = true) noexcept;

  /**
   * @brief Leaves a previously entered prohibition scope.
   * @throws Nothing; depth underflow terminates as an internal invariant.
   */
  ~ComputeIoWaitProhibitionScope() noexcept;

  /** @brief Prohibition scope ownership cannot be copied. */
  ComputeIoWaitProhibitionScope(const ComputeIoWaitProhibitionScope&) = delete;

  /** @brief Prohibition scope ownership cannot be assigned. */
  ComputeIoWaitProhibitionScope& operator=(
      const ComputeIoWaitProhibitionScope&) = delete;

 private:
  /** @brief Whether this instance incremented thread-local depth. */
  bool active_ = false;
};

/**
 * @brief Owns one independent bounded process compute-I/O worker.
 *
 * Submission atomically charges task count and planned bytes before invoking a
 * non-owning lazy factory. The resulting callback and explicit lifetime token
 * are then published to a private FIFO. One worker contains callback failures,
 * produces typed completion, and releases both charges exactly once.
 *
 * @throws std::invalid_argument for zero limits.
 * @throws std::bad_alloc or std::system_error from state/thread construction.
 * @note This mechanism owns no Graph/cache/output policy, persistence identity,
 * path, retry, visibility, receipt, durability, CPU/GPU route, or scheduler.
 */
class ComputeIoExecutor final {
 public:
  /** @brief Copyable callback executed on the independent I/O worker. */
  using Task = std::function<void()>;

  /**
   * @brief Creates and starts one independently bounded I/O worker.
   * @param limits Positive immutable task and planned-byte limits.
   * @throws std::invalid_argument when either limit is zero.
   * @throws std::bad_alloc or std::system_error from state/thread creation.
   */
  explicit ComputeIoExecutor(ComputeIoExecutorLimits limits);

  /**
   * @brief Stops admission, drains accepted work, and joins the worker.
   * @throws Nothing; an unexpected shutdown invariant failure terminates.
   */
  ~ComputeIoExecutor() noexcept;

  /** @brief Worker, queue, and budget ownership cannot be copied. */
  ComputeIoExecutor(const ComputeIoExecutor&) = delete;

  /** @brief Worker, queue, and budget ownership cannot be assigned. */
  ComputeIoExecutor& operator=(const ComputeIoExecutor&) = delete;

  /**
   * @brief Atomically admits one task before invoking its lazy factory.
   *
   * @tparam TaskFactory Const-invocable factory returning one nonempty `Task`.
   * @param planned_bytes Positive estimated retained bytes for complete work.
   * @param lifetime_token Non-null Run/transaction owner retained through
   * callback settlement.
   * @param factory Non-owning factory invoked only after both budgets commit.
   * @return Typed rejection, or Accepted with an active completion.
   * @throws std::invalid_argument when an admitted factory returns empty.
   * @throws Exceptions from the factory or queue/task-state allocation after
   * exact admission rollback.
   * @note Rejection never invokes or copies the payload produced by `factory`.
   * The factory object itself remains caller-owned for this synchronous call.
   */
  template <typename TaskFactory>
  ComputeIoSubmission try_submit(
      std::uint64_t planned_bytes,
      const std::shared_ptr<const void>& lifetime_token,
      TaskFactory&& factory) {
    using Factory = std::remove_reference_t<TaskFactory>;
    static_assert(std::is_invocable_r_v<Task, const Factory&>,
                  "ComputeIoExecutor factory must return Task from const call");
    return try_submit_erased(
        planned_bytes, lifetime_token,
        static_cast<const void*>(std::addressof(factory)),
        [](const void* context) -> Task {
          return std::invoke(*static_cast<const Factory*>(context));
        });
  }

  /**
   * @brief Copies current limits, phase counts, and budget usage.
   * @return Immutable authority-free snapshot.
   * @throws std::system_error if executor locking fails.
   */
  ComputeIoExecutorSnapshot snapshot() const;

  /**
   * @brief Stops admission, drains accepted work, and joins exactly once.
   * @return Nothing after every phase and both budget totals reach zero.
   * @throws std::logic_error when called by the I/O worker itself.
   * @throws std::system_error from control synchronization.
   * @note Repeated calls after complete shutdown are idempotent.
   */
  void shutdown();

 private:
  /** @brief Erased non-owning invocation of one caller-owned lazy factory. */
  using TaskFactoryInvoker = Task (*)(const void* context);

  /**
   * @brief Implements atomic admission around an erased lazy factory.
   * @param planned_bytes Positive caller estimate.
   * @param lifetime_token Explicit Run/transaction owner.
   * @param factory_context Borrowed factory address valid for this call.
   * @param invoke Factory invocation function.
   * @return Typed submission result.
   * @throws Factory, state-allocation, queue-allocation, or invalid-task errors
   * after exact rollback.
   */
  ComputeIoSubmission try_submit_erased(
      std::uint64_t planned_bytes,
      const std::shared_ptr<const void>& lifetime_token,
      const void* factory_context, TaskFactoryInvoker invoke);

  /** @brief Shared state retained through independent worker exit. */
  std::shared_ptr<ComputeIoExecutorState> state_;
};

}  // namespace ps::execution
