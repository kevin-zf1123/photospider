#include "execution/device/compute_io_executor.hpp"

#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ps::execution {

namespace {

/** @brief Per-thread nesting depth that forbids compute-I/O blocking waits. */
thread_local std::uint32_t g_compute_io_wait_prohibition_depth = 0U;

/**
 * @brief Executor state currently invoking a callback on this I/O worker.
 *
 * @note The pointer is installed for the complete worker-loop lifetime and
 * grants no ownership. It permits self-shutdown rejection without racing
 * another control thread's `std::thread::join()`.
 */
thread_local const ComputeIoExecutorState* g_current_io_executor = nullptr;

/** @brief One node in the current thread's nested lazy-factory stack. */
class ComputeIoFactoryInvocationScope;

/** @brief Borrowed pointer to one stack-owned lazy-factory scope. */
using ComputeIoFactoryScopePointer = const ComputeIoFactoryInvocationScope*;

/**
 * @brief Innermost active compute-I/O lazy-factory scope on this thread.
 *
 * @note The linked scopes are stack-owned, allocate no memory, and retain no
 * executor. Walking the full chain distinguishes legal different-executor
 * control from direct or indirect shutdown re-entry.
 */
thread_local ComputeIoFactoryScopePointer g_io_factory_scope = nullptr;

/**
 * @brief Tracks one exception-safe nested lazy-factory invocation.
 *
 * Each scope pushes a borrowed executor-state identity onto a thread-local
 * linked stack. `shutdown()` can therefore reject any executor still present
 * in an outer or inner factory frame, including `A -> B -> A` control cycles,
 * without rejecting an unrelated executor.
 *
 * @throws Nothing for construction and destruction.
 * @note Scope nodes and state identities are borrowed only for synchronous
 * factory invocation. Destruction requires strict LIFO order.
 */
class ComputeIoFactoryInvocationScope final {
 public:
  /**
   * @brief Pushes one executor identity onto the current factory stack.
   * @param state Non-null borrowed state whose factory is about to run.
   * @throws Nothing; a null state terminates as an internal invariant breach.
   */
  explicit ComputeIoFactoryInvocationScope(
      const ComputeIoExecutorState* state) noexcept
      : state_(state), previous_(g_io_factory_scope) {
    if (state_ == nullptr) {
      std::terminate();
    }
    g_io_factory_scope = this;
  }

  /**
   * @brief Pops this exact scope and restores the outer factory identity.
   * @throws Nothing; non-LIFO destruction terminates as an invariant breach.
   */
  ~ComputeIoFactoryInvocationScope() noexcept {
    if (g_io_factory_scope != this) {
      std::terminate();
    }
    g_io_factory_scope = previous_;
  }

  /** @brief Factory-scope ownership cannot be copied. */
  ComputeIoFactoryInvocationScope(const ComputeIoFactoryInvocationScope&) =
      delete;

  /** @brief Factory-scope ownership cannot be assigned. */
  ComputeIoFactoryInvocationScope& operator=(
      const ComputeIoFactoryInvocationScope&) = delete;

  /**
   * @brief Searches every active factory frame on the current thread.
   * @param state Borrowed executor identity to find.
   * @return True when an inner or outer factory is constructing for `state`.
   * @throws Nothing.
   */
  static bool current_thread_contains(
      const ComputeIoExecutorState* state) noexcept {
    for (const ComputeIoFactoryInvocationScope* scope = g_io_factory_scope;
         scope != nullptr; scope = scope->previous_) {
      if (scope->state_ == state) {
        return true;
      }
    }
    return false;
  }

 private:
  /** @brief Borrowed identity of the executor under construction. */
  const ComputeIoExecutorState* state_ = nullptr;

  /** @brief Borrowed outer scope, or null at the stack root. */
  const ComputeIoFactoryInvocationScope* previous_ = nullptr;
};

/**
 * @brief Reports whether the current thread may not block on compute I/O.
 * @return True inside at least one prohibition scope.
 * @throws Nothing.
 */
bool compute_io_wait_prohibited() noexcept {
  return g_compute_io_wait_prohibition_depth != 0U;
}

}  // namespace

/**
 * @brief Mutable completion state shared independently of executor lifetime.
 *
 * @throws Standard synchronization exceptions from explicit operations.
 * @note Task payload, lifetime token, and budget authority live in the queue
 * entry instead, so terminal completion handles retain none of them.
 */
struct ComputeIoTaskState final {
  /**
   * @brief Creates provisional nonterminal state for one owning executor.
   * @param executor Shared executor identity observed weakly by `wait()`.
   * @param accepted_planned_bytes Exact positive task byte charge.
   * @throws Nothing.
   * @note Admission sequence remains zero until successful factory construction
   * reaches either queue publication or the shutdown-won atomic
   * Accepted/Cancelled decision.
   * @note Weak ownership prevents a retained terminal completion from keeping
   * the process executor alive or causing address-reuse false positives.
   */
  explicit ComputeIoTaskState(
      const std::shared_ptr<ComputeIoExecutorState>& executor,
      std::uint64_t accepted_planned_bytes) noexcept
      : planned_bytes(accepted_planned_bytes), owning_executor(executor) {
    if (planned_bytes == 0U) {
      std::terminate();
    }
  }

  /** @brief Internal nonterminal/terminal progress category. */
  enum class Phase : std::uint8_t {
    /** @brief Queue owns the task but worker callback has not entered. */
    Queued,
    /** @brief Independent worker callback is currently entered. */
    Running,
    /** @brief Typed result is immutable and waiters may return. */
    Terminal,
  };

  /** @brief Serializes phase, cancellation, and result. */
  mutable std::mutex mutex;

  /** @brief Wakes waiters after immutable terminal publication. */
  std::condition_variable completion_cv;

  /** @brief Current callback lifecycle phase. */
  Phase phase = Phase::Queued;

  /** @brief Whether cancellation won before terminal publication. */
  bool cancellation_requested = false;

  /** @brief Immutable terminal result present exactly in Terminal. */
  std::optional<ComputeIoTaskResult> result;

  /** @brief Executor event sequence that atomically admitted this task. */
  std::uint64_t admission_sequence = 0U;

  /** @brief Exact immutable planned-byte charge admitted for this task. */
  const std::uint64_t planned_bytes = 0U;

  /**
   * @brief Weak identity of the executor whose worker owns this completion.
   *
   * @note The pointer is locked only for identity comparison and never grants
   * queue, shutdown, or budget authority.
   */
  std::weak_ptr<ComputeIoExecutorState> owning_executor;

  /**
   * @brief Binds the one signed Accepted event after provisional construction.
   * @param accepted_admission_sequence Nonzero executor event identity.
   * @return Nothing.
   * @throws Nothing; duplicate/zero binding terminates as an invariant breach.
   * @note The caller holds the executor mutex before queue visibility or typed
   * shutdown cancellation; no worker can observe an unbound queued state.
   */
  void bind_admission(std::uint64_t accepted_admission_sequence) noexcept {
    if (admission_sequence != 0U || accepted_admission_sequence == 0U) {
      std::terminate();
    }
    admission_sequence = accepted_admission_sequence;
  }

  /**
   * @brief Publishes the one immutable terminal fact while already locked.
   * @param status Exact terminal category.
   * @param failure Original callback exception when failed.
   * @param duration Time spent inside the provider callback.
   * @param settlement_event Exact executor-authored charge-release proof.
   * @return Nothing.
   * @throws Nothing; duplicate publication terminates as an invariant breach.
   * @note The caller must hold `mutex` and release executor accounting before
   * invoking this helper.
   */
  void publish_terminal(ComputeIoCompletionStatus status,
                        std::exception_ptr failure,
                        std::chrono::nanoseconds duration,
                        ComputeIoSettlementEvent settlement_event) noexcept {
    if (phase == Phase::Terminal || result.has_value() ||
        settlement_event.admission_sequence != admission_sequence ||
        settlement_event.status != status ||
        settlement_event.released_tasks != 1U ||
        settlement_event.released_planned_bytes != planned_bytes) {
      std::terminate();
    }
    result = ComputeIoTaskResult(status, std::move(failure), duration,
                                 std::move(settlement_event));
    phase = Phase::Terminal;
  }
};

/**
 * @brief One accepted task's payload and exact budget/lifetime ownership.
 *
 * @throws std::bad_alloc when copied callback/token/path payload allocates.
 * @note The executor destroys `task` and `lifetime_token` before releasing the
 * matching budget charge and publishing terminal completion.
 */
struct ComputeIoQueueEntry final {
  /** @brief Shared completion fact. */
  std::shared_ptr<ComputeIoTaskState> completion;

  /** @brief Run/transaction owner retained through callback retirement. */
  std::shared_ptr<const void> lifetime_token;

  /** @brief Callback invoked only by the independent I/O worker. */
  ComputeIoExecutor::Task task;

  /** @brief Exact estimated-byte charge committed at admission. */
  std::uint64_t planned_bytes = 0U;
};

/**
 * @brief Complete process executor queue, worker, and dual-budget authority.
 *
 * @throws std::bad_alloc from FIFO growth and std::system_error from explicit
 * synchronization or worker construction.
 * @note `mutex` protects every phase counter, budget value, queue transition,
 * and admission/shutdown flag. `shutdown_mutex` serializes joining callers.
 */
struct ComputeIoExecutorState final {
  /**
   * @brief Creates unstarted executor state with immutable positive limits.
   * @param executor_limits Validated task/byte limits.
   * @throws Nothing.
   */
  explicit ComputeIoExecutorState(ComputeIoExecutorLimits executor_limits)
      : limits(executor_limits) {}

  /** @brief Immutable task/byte limits. */
  const ComputeIoExecutorLimits limits;

  /** @brief Serializes queue, counts, budgets, and state flags. */
  mutable std::mutex mutex;

  /** @brief Wakes the worker for work, construction settlement, or shutdown. */
  std::condition_variable worker_cv;

  /** @brief Serializes repeated shutdown callers around thread join. */
  std::mutex shutdown_mutex;

  /** @brief FIFO of accepted published task ownership. */
  std::deque<ComputeIoQueueEntry> queue;

  /** @brief Sole independent process I/O worker. */
  std::thread worker;

  /** @brief Provisional/accepted tasks across all three active phases. */
  std::uint64_t active_tasks = 0U;

  /** @brief Summed planned bytes for all provisional/accepted tasks. */
  std::uint64_t active_planned_bytes = 0U;

  /** @brief Reservations still occupying construction/final-decision phase. */
  std::uint64_t constructing_tasks = 0U;

  /** @brief Published FIFO entries not yet entered. */
  std::uint64_t queued_tasks = 0U;

  /** @brief Currently entered callbacks; at most one with the fixed worker. */
  std::uint64_t running_tasks = 0U;

  /** @brief True until graceful shutdown linearizes admission stop. */
  bool accepting = true;

  /** @brief True after shutdown asks the worker to drain and exit. */
  bool stopping = false;

  /** @brief True after the independent worker has been joined. */
  bool shutdown_complete = false;

  /** @brief Next nonzero executor-authored admission/settlement sequence. */
  std::uint64_t next_accounting_event_sequence = 1U;
};

namespace {

/**
 * @brief Copies executor counters while the complete state mutex is held.
 * @param state Locked executor state.
 * @return Atomic-cut authority-free snapshot.
 * @throws Nothing.
 */
ComputeIoExecutorSnapshot snapshot_locked(
    const ComputeIoExecutorState& state) noexcept {
  return ComputeIoExecutorSnapshot{
      state.limits.task_limit,  state.limits.planned_bytes_limit,
      state.active_tasks,       state.active_planned_bytes,
      state.constructing_tasks, state.queued_tasks,
      state.running_tasks,      state.accepting,
      state.shutdown_complete,
  };
}

/**
 * @brief Allocates one nonzero accounting-event sequence under executor lock.
 * @param state Locked executor state.
 * @return Unique monotonically increasing sequence.
 * @throws Nothing; exhaustion terminates as an unrecoverable evidence breach.
 */
std::uint64_t next_accounting_event_sequence_locked(
    ComputeIoExecutorState& state) noexcept {
  if (state.next_accounting_event_sequence == 0U ||
      state.next_accounting_event_sequence ==
          std::numeric_limits<std::uint64_t>::max()) {
    std::terminate();
  }
  return state.next_accounting_event_sequence++;
}

/**
 * @brief Captures one exact admission decision at its locked linearization.
 * @param state Locked executor state after rejection or reservation adoption.
 * @param status Exact typed admission decision.
 * @param offered_planned_bytes Caller-provided estimate, possibly zero-invalid.
 * @param charged Whether Accepted adopted both provisional reservations.
 * @return Executor-authored event and same-lock post-decision snapshot.
 * @throws Nothing.
 */
ComputeIoAdmissionEvent make_admission_event_locked(
    ComputeIoExecutorState& state, ComputeIoAdmissionStatus status,
    std::uint64_t offered_planned_bytes, bool charged) noexcept {
  return ComputeIoAdmissionEvent{
      next_accounting_event_sequence_locked(state),
      status,
      offered_planned_bytes,
      charged ? 1U : 0U,
      charged ? offered_planned_bytes : 0U,
      snapshot_locked(state),
  };
}

/**
 * @brief Captures one accepted task's exact release under executor lock.
 * @param state Locked executor state after task/byte release.
 * @param completion Accepted task identity and immutable charge.
 * @param status Exact terminal category about to be published.
 * @return Executor-authored settlement event and post-release snapshot.
 * @throws Nothing.
 */
ComputeIoSettlementEvent make_settlement_event_locked(
    ComputeIoExecutorState& state, const ComputeIoTaskState& completion,
    ComputeIoCompletionStatus status) noexcept {
  return ComputeIoSettlementEvent{
      next_accounting_event_sequence_locked(state),
      completion.admission_sequence,
      status,
      1U,
      completion.planned_bytes,
      snapshot_locked(state),
  };
}

/**
 * @brief Releases one exact task/byte admission charge under executor lock.
 * @param state Locked executor state.
 * @param planned_bytes Exact charge stored by the task or rollback guard.
 * @return Nothing.
 * @throws Nothing; accounting underflow terminates.
 */
void release_admission_locked(ComputeIoExecutorState& state,
                              std::uint64_t planned_bytes) noexcept {
  if (state.active_tasks == 0U || state.active_planned_bytes < planned_bytes) {
    std::terminate();
  }
  --state.active_tasks;
  state.active_planned_bytes -= planned_bytes;
}

/**
 * @brief Rolls back a provisional construction unless final admission consumes
 * it.
 *
 * @throws Nothing from destruction; synchronization failure terminates.
 * @note The guard owns exactly one constructing count plus task/byte
 * reservation.
 */
class ComputeIoConstructionRollback final {
 public:
  /**
   * @brief Adopts one provisionally charged construction.
   * @param state Shared executor authority.
   * @param planned_bytes Exact charged estimate.
   * @throws Nothing.
   */
  ComputeIoConstructionRollback(std::shared_ptr<ComputeIoExecutorState> state,
                                std::uint64_t planned_bytes) noexcept
      : state_(std::move(state)), planned_bytes_(planned_bytes) {}

  /**
   * @brief Releases unconsumed construction authority.
   * @throws Nothing; invariant or synchronization failure terminates.
   */
  ~ComputeIoConstructionRollback() noexcept {
    if (!active_) {
      return;
    }
    try {
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->constructing_tasks == 0U) {
          std::terminate();
        }
        --state_->constructing_tasks;
        release_admission_locked(*state_, planned_bytes_);
      }
      state_->worker_cv.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  /** @brief Unique rollback ownership cannot be copied. */
  ComputeIoConstructionRollback(const ComputeIoConstructionRollback&) = delete;

  /** @brief Unique rollback ownership cannot be assigned. */
  ComputeIoConstructionRollback& operator=(
      const ComputeIoConstructionRollback&) = delete;

  /**
   * @brief Relinquishes rollback after queue admission or atomic cancellation.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept { active_ = false; }

 private:
  /** @brief Executor whose construction count and budgets were charged. */
  std::shared_ptr<ComputeIoExecutorState> state_;

  /** @brief Exact estimated-byte charge. */
  std::uint64_t planned_bytes_ = 0U;

  /** @brief Whether destruction must roll the charge back. */
  bool active_ = true;
};

/**
 * @brief Publishes one terminal result while releasing exact executor charges.
 * @param state Locked process executor authority.
 * @param completion Locked task completion state.
 * @param planned_bytes Exact accepted estimate.
 * @param status Terminal category selected after callback/cancellation.
 * @param failure Original callback exception when failed.
 * @param duration Time spent inside callback.
 * @return Nothing.
 * @throws Nothing; accounting or phase inconsistency terminates.
 * @note Caller has already destroyed task payload and lifetime-token ownership.
 */
void settle_entry_locked(ComputeIoExecutorState& state,
                         ComputeIoTaskState& completion,
                         std::uint64_t planned_bytes,
                         ComputeIoCompletionStatus status,
                         std::exception_ptr failure,
                         std::chrono::nanoseconds duration) noexcept {
  if (completion.phase == ComputeIoTaskState::Phase::Terminal ||
      state.running_tasks == 0U) {
    std::terminate();
  }
  --state.running_tasks;
  release_admission_locked(state, planned_bytes);
  const ComputeIoSettlementEvent settlement_event =
      make_settlement_event_locked(state, completion, status);
  completion.publish_terminal(status, std::move(failure), duration,
                              settlement_event);
}

/**
 * @brief Drains the FIFO and executes accepted callbacks independently.
 * @param state Shared process authority retained through thread exit.
 * @return Nothing.
 * @throws Nothing; unexpected internal failures terminate.
 */
void compute_io_worker_loop(
    const std::shared_ptr<ComputeIoExecutorState>& state) noexcept {
  if (g_current_io_executor != nullptr) {
    std::terminate();
  }
  g_current_io_executor = state.get();
  try {
    for (;;) {
      ComputeIoQueueEntry entry;
      bool skip_cancelled = false;
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->worker_cv.wait(lock, [&state]() {
          return !state->queue.empty() ||
                 (state->stopping && state->constructing_tasks == 0U);
        });
        if (state->queue.empty()) {
          if (state->stopping && state->constructing_tasks == 0U) {
            return;
          }
          continue;
        }

        entry = std::move(state->queue.front());
        state->queue.pop_front();
        if (state->queued_tasks == 0U) {
          std::terminate();
        }
        --state->queued_tasks;
        ++state->running_tasks;
        {
          std::lock_guard<std::mutex> completion_lock(entry.completion->mutex);
          skip_cancelled = entry.completion->cancellation_requested;
          if (!skip_cancelled) {
            entry.completion->phase = ComputeIoTaskState::Phase::Running;
          }
        }
      }

      std::exception_ptr failure;
      std::chrono::nanoseconds duration{0};
      if (!skip_cancelled) {
        const auto started = std::chrono::steady_clock::now();
        try {
          entry.task();
        } catch (...) {
          failure = std::current_exception();
        }
        duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
      }

      entry.task = {};
      entry.lifetime_token.reset();

      {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        std::lock_guard<std::mutex> completion_lock(entry.completion->mutex);
        const bool cancelled =
            skip_cancelled || entry.completion->cancellation_requested;
        const ComputeIoCompletionStatus status =
            cancelled ? ComputeIoCompletionStatus::Cancelled
                      : (failure ? ComputeIoCompletionStatus::Failed
                                 : ComputeIoCompletionStatus::Succeeded);
        if (cancelled) {
          failure = nullptr;
        }
        settle_entry_locked(*state, *entry.completion, entry.planned_bytes,
                            status, std::move(failure), duration);
      }
      entry.completion->completion_cv.notify_all();
      state->worker_cv.notify_all();
    }
  } catch (...) {
    std::terminate();
  }
}

}  // namespace

/** @copydoc compute_io_admission_status_name */
const char* compute_io_admission_status_name(
    ComputeIoAdmissionStatus status) noexcept {
  switch (status) {
    case ComputeIoAdmissionStatus::Accepted:
      return "accepted";
    case ComputeIoAdmissionStatus::InvalidRequest:
      return "invalid_request";
    case ComputeIoAdmissionStatus::TaskLimit:
      return "task_limit";
    case ComputeIoAdmissionStatus::PlannedByteLimit:
      return "planned_byte_limit";
    case ComputeIoAdmissionStatus::ShuttingDown:
      return "shutting_down";
  }
  return "invalid_request";
}

/** @copydoc ComputeIoTaskResult::rethrow_if_failed */
void ComputeIoTaskResult::rethrow_if_failed() const {
  if (status_ == ComputeIoCompletionStatus::Failed) {
    if (!failure_) {
      throw std::logic_error(
          "Compute-I/O failed result has no exception identity.");
    }
    std::rethrow_exception(failure_);
  }
}

/** @copydoc ComputeIoCompletion::ready */
bool ComputeIoCompletion::ready() const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->phase == ComputeIoTaskState::Phase::Terminal;
}

/** @copydoc ComputeIoCompletion::try_result */
std::optional<ComputeIoTaskResult> ComputeIoCompletion::try_result() const {
  if (!state_) {
    throw std::logic_error("Compute-I/O completion is inactive.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->phase != ComputeIoTaskState::Phase::Terminal) {
    return std::nullopt;
  }
  if (!state_->result.has_value()) {
    throw std::logic_error("Compute-I/O terminal result is missing.");
  }
  return state_->result;
}

/** @copydoc ComputeIoCompletion::wait */
ComputeIoTaskResult ComputeIoCompletion::wait() const {
  if (!state_) {
    throw std::logic_error("Compute-I/O completion is inactive.");
  }
  if (compute_io_wait_prohibited()) {
    throw std::logic_error("CPU compute worker cannot wait for compute I/O.");
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  if (state_->phase != ComputeIoTaskState::Phase::Terminal) {
    const std::shared_ptr<ComputeIoExecutorState> owning_executor =
        state_->owning_executor.lock();
    if (owning_executor && g_current_io_executor == owning_executor.get()) {
      throw std::logic_error(
          "Compute-I/O worker cannot wait for its own executor.");
    }
    state_->completion_cv.wait(lock, [this]() {
      return state_->phase == ComputeIoTaskState::Phase::Terminal;
    });
  }
  if (!state_->result.has_value()) {
    throw std::logic_error("Compute-I/O terminal result is missing.");
  }
  return *state_->result;
}

/** @copydoc ComputeIoCompletion::cancel */
bool ComputeIoCompletion::cancel() const {
  if (!state_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->phase == ComputeIoTaskState::Phase::Terminal ||
      state_->cancellation_requested) {
    return false;
  }
  state_->cancellation_requested = true;
  return true;
}

/** @copydoc ComputeIoWaitProhibitionScope::ComputeIoWaitProhibitionScope */
ComputeIoWaitProhibitionScope::ComputeIoWaitProhibitionScope(
    bool active) noexcept
    : active_(active) {
  if (!active_) {
    return;
  }
  if (g_compute_io_wait_prohibition_depth ==
      std::numeric_limits<std::uint32_t>::max()) {
    std::terminate();
  }
  ++g_compute_io_wait_prohibition_depth;
}

/** @copydoc ComputeIoWaitProhibitionScope::~ComputeIoWaitProhibitionScope */
ComputeIoWaitProhibitionScope::~ComputeIoWaitProhibitionScope() noexcept {
  if (!active_) {
    return;
  }
  if (g_compute_io_wait_prohibition_depth == 0U) {
    std::terminate();
  }
  --g_compute_io_wait_prohibition_depth;
}

/** @copydoc ComputeIoExecutor::ComputeIoExecutor */
ComputeIoExecutor::ComputeIoExecutor(ComputeIoExecutorLimits limits) {
  if (limits.task_limit == 0U || limits.planned_bytes_limit == 0U) {
    throw std::invalid_argument(
        "Compute-I/O executor limits must be positive.");
  }
  state_ = std::make_shared<ComputeIoExecutorState>(limits);
  state_->worker = std::thread(compute_io_worker_loop, state_);
}

/** @copydoc ComputeIoExecutor::~ComputeIoExecutor */
ComputeIoExecutor::~ComputeIoExecutor() noexcept {
  try {
    shutdown();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ComputeIoExecutor::try_submit_erased */
ComputeIoSubmission ComputeIoExecutor::try_submit_erased(
    std::uint64_t planned_bytes,
    const std::shared_ptr<const void>& lifetime_token,
    const void* factory_context, TaskFactoryInvoker invoke) {
  const std::shared_ptr<ComputeIoExecutorState> state = state_;
  if (planned_bytes == 0U || !lifetime_token || factory_context == nullptr ||
      invoke == nullptr) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return ComputeIoSubmission(
        ComputeIoAdmissionStatus::InvalidRequest, {},
        make_admission_event_locked(*state,
                                    ComputeIoAdmissionStatus::InvalidRequest,
                                    planned_bytes, false));
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting) {
      return ComputeIoSubmission(
          ComputeIoAdmissionStatus::ShuttingDown, {},
          make_admission_event_locked(*state,
                                      ComputeIoAdmissionStatus::ShuttingDown,
                                      planned_bytes, false));
    }
    if (g_current_io_executor == state.get()) {
      return ComputeIoSubmission(
          ComputeIoAdmissionStatus::InvalidRequest, {},
          make_admission_event_locked(*state,
                                      ComputeIoAdmissionStatus::InvalidRequest,
                                      planned_bytes, false));
    }
    if (state->active_tasks >= state->limits.task_limit) {
      return ComputeIoSubmission(
          ComputeIoAdmissionStatus::TaskLimit, {},
          make_admission_event_locked(*state,
                                      ComputeIoAdmissionStatus::TaskLimit,
                                      planned_bytes, false));
    }
    if (planned_bytes > state->limits.planned_bytes_limit ||
        state->active_planned_bytes >
            state->limits.planned_bytes_limit - planned_bytes) {
      return ComputeIoSubmission(
          ComputeIoAdmissionStatus::PlannedByteLimit, {},
          make_admission_event_locked(
              *state, ComputeIoAdmissionStatus::PlannedByteLimit, planned_bytes,
              false));
    }
    ++state->active_tasks;
    state->active_planned_bytes += planned_bytes;
    ++state->constructing_tasks;
  }

  ComputeIoConstructionRollback rollback(state, planned_bytes);
  std::shared_ptr<const void> retained_lifetime = lifetime_token;
  Task task;
  {
    const ComputeIoFactoryInvocationScope factory_scope(state.get());
    task = invoke(factory_context);
  }
  if (!task) {
    throw std::invalid_argument(
        "Compute-I/O task factory returned an empty callback.");
  }
  auto completion_state =
      std::make_shared<ComputeIoTaskState>(state, planned_bytes);
  ComputeIoCompletion completion(completion_state);
  ComputeIoQueueEntry entry{completion_state, std::move(retained_lifetime),
                            std::move(task), planned_bytes};

  bool cancelled_by_shutdown = false;
  ComputeIoAdmissionEvent admission_event;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->accepting) {
      cancelled_by_shutdown = true;
    } else {
      state->queue.push_back(std::move(entry));
      if (state->constructing_tasks == 0U) {
        std::terminate();
      }
      --state->constructing_tasks;
      ++state->queued_tasks;
      admission_event = make_admission_event_locked(
          *state, ComputeIoAdmissionStatus::Accepted, planned_bytes, true);
      completion_state->bind_admission(admission_event.sequence);
    }
  }
  if (cancelled_by_shutdown) {
    entry.task = {};
    entry.lifetime_token.reset();
    {
      std::lock_guard<std::mutex> state_lock(state->mutex);
      std::lock_guard<std::mutex> completion_lock(completion_state->mutex);
      if (state->constructing_tasks == 0U) {
        std::terminate();
      }
      admission_event = make_admission_event_locked(
          *state, ComputeIoAdmissionStatus::Accepted, planned_bytes, true);
      completion_state->bind_admission(admission_event.sequence);
      --state->constructing_tasks;
      release_admission_locked(*state, planned_bytes);
      completion_state->cancellation_requested = true;
      const ComputeIoSettlementEvent settlement_event =
          make_settlement_event_locked(*state, *completion_state,
                                       ComputeIoCompletionStatus::Cancelled);
      completion_state->publish_terminal(ComputeIoCompletionStatus::Cancelled,
                                         nullptr, std::chrono::nanoseconds{0},
                                         settlement_event);
    }
  }
  rollback.release();
  state->worker_cv.notify_all();
  if (cancelled_by_shutdown) {
    completion_state->completion_cv.notify_all();
  }
  return ComputeIoSubmission(ComputeIoAdmissionStatus::Accepted,
                             std::move(completion), std::move(admission_event));
}

/** @copydoc ComputeIoExecutor::snapshot */
ComputeIoExecutorSnapshot ComputeIoExecutor::snapshot() const {
  const std::shared_ptr<ComputeIoExecutorState> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  return snapshot_locked(*state);
}

/** @copydoc ComputeIoExecutor::shutdown */
void ComputeIoExecutor::shutdown() {
  const std::shared_ptr<ComputeIoExecutorState> state = state_;
  if (g_current_io_executor == state.get()) {
    throw std::logic_error(
        "Compute-I/O worker cannot synchronously shut down itself.");
  }
  if (ComputeIoFactoryInvocationScope::current_thread_contains(state.get())) {
    throw std::logic_error(
        "Compute-I/O task factory cannot synchronously shut down its "
        "executor.");
  }

  std::lock_guard<std::mutex> shutdown_lock(state->shutdown_mutex);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->shutdown_complete) {
      return;
    }
    state->accepting = false;
    state->stopping = true;
  }
  state->worker_cv.notify_all();
  if (state->worker.joinable()) {
    state->worker.join();
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->queue.empty() || state->active_tasks != 0U ||
        state->active_planned_bytes != 0U || state->constructing_tasks != 0U ||
        state->queued_tasks != 0U || state->running_tasks != 0U) {
      throw std::logic_error(
          "Compute-I/O executor did not drain all accepted work.");
    }
    state->shutdown_complete = true;
  }
}

}  // namespace ps::execution
