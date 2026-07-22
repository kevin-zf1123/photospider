#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

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
  class ContinuationState;
  struct QueueNode;

 public:
  /**
   * @brief Maximum number of waiting tasks owned by a production lane.
   * @note The at-most-one task currently executing is excluded from this
   *       capacity, so default total admitted work is at most 65 tasks.
   */
  static constexpr std::size_t kDefaultQueueCapacity = 64;

  /**
   * @brief Selects how the configured capacity charges active one-shot work.
   * @throws Nothing for value construction and comparison.
   * @note Graph-state lanes retain the historical waiting-only contract. The
   *       private compute-request lane selects TotalAdmission so queued,
   *       running, and parked continuation units share one exact bound.
   */
  enum class CapacityMode {
    /** @brief Charge only one-shot work waiting in the FIFO. */
    WaitingOnly,
    /** @brief Charge every queued/running one-shot or continuation unit. */
    TotalAdmission,
  };

  /**
   * @brief Worker-return decision for one reserved continuation turn.
   * @throws Nothing for value construction and comparison.
   * @note The worker applies the decision only after the callback has fully
   *       returned and no callback-owned lock remains held.
   */
  enum class ContinuationAction {
    /** @brief Keep the charged ticket parked until an owner wakes it. */
    Park,
    /** @brief Append the same charged ticket at the FIFO tail. */
    Queue,
    /** @brief Release the ticket and its admission unit exactly once. */
    Retire,
  };

  /**
   * @brief Copyable handle for one executor-owned reserved continuation.
   *
   * A handle never owns another admission unit. The executor retains the
   * callback and one preallocated intrusive FIFO node from reservation through
   * exact retirement, allowing wake and worker-tail handoff without allocation
   * or ordinary submit re-entry.
   *
   * @throws std::system_error when a valid executor mutex cannot be locked.
   * @note Handles must not outlive their executor. External publication uses
   * `wake()`. The accepted-work owner uses `resume_after_settlement()` so close
   * can finish an already admitted parked continuation after external wake has
   * been rejected.
   */
  class ContinuationTicket {
   public:
    /**
     * @brief Creates an empty non-owning ticket handle.
     * @throws Nothing.
     * @note Every operation on an empty handle returns false.
     */
    ContinuationTicket() noexcept = default;

    /**
     * @brief Reports whether this handle names a reservation.
     * @return True before empty construction or movement is observed.
     * @throws Nothing.
     */
    bool valid() const noexcept { return state_ != nullptr; }

    /**
     * @brief Nonblockingly wakes an accepted continuation while admission is
     * open.
     * @return True when the reservation remains live and the wake was accepted;
     * false after admission stops or retirement wins.
     * @throws std::system_error if executor synchronization fails.
     * @note A parked ticket receives its already allocated FIFO node; a running
     * ticket records one idempotent wake-pending bit. No capacity wait,
     * allocation, callback, or ordinary submit occurs here.
     */
    bool wake() const;

    /**
     * @brief Wakes already admitted ownership after runner settlement.
     * @return True while the reservation remains live.
     * @throws std::system_error if executor synchronization fails.
     * @note Unlike external wake, this operation remains valid while draining.
     * In that state it marks one close-owned turn without allocating a FIFO
     * node, so accepted work can retire before the worker joins.
     */
    bool resume_after_settlement() const;

    /**
     * @brief Requests exact retirement of this reservation.
     * @return True only when this call first records or completes retirement.
     * @throws std::system_error if executor synchronization fails.
     * @note Parked retirement releases capacity immediately. Queued/running
     * retirement is completed by the worker without invoking another callback.
     */
    bool retire() const;

    /**
     * @brief Compares whether two handles name the same charged reservation.
     * @param other Candidate handle.
     * @return True only for the same executor-owned ticket state.
     * @throws Nothing.
     */
    bool same_reservation(const ContinuationTicket& other) const noexcept {
      return owner_ == other.owner_ && state_ == other.state_;
    }

   private:
    friend class GraphStateExecutor;

    /**
     * @brief Binds a handle to executor-retained ticket state.
     * @param owner Executor that owns admission and the intrusive node.
     * @param state Shared ticket state retained by internal handles.
     * @throws Nothing.
     */
    ContinuationTicket(GraphStateExecutor* owner,
                       std::shared_ptr<ContinuationState> state) noexcept
        : owner_(owner), state_(std::move(state)) {}

    /** @brief Borrowed executor that must outlive this handle. */
    GraphStateExecutor* owner_ = nullptr;
    /** @brief Shared identity/state for one executor-owned reservation. */
    std::shared_ptr<ContinuationState> state_;
  };

  /**
   * @brief Callback invoked once for each reserved continuation turn.
   * @param ticket Handle naming the running reservation.
   * @return Park, tail-queue, or retire decision applied after return.
   * @throws Callbacks should contain their own logical-request failures. An
   *       escaping exception retires the ticket to protect lane liveness.
   * @note The executor mutex is not held during invocation. One turn must
   *       materialize at most one logical request and must not submit to its
   *       own lane.
   */
  using ContinuationCallback =
      std::function<ContinuationAction(const ContinuationTicket& ticket)>;

  /**
   * @brief Binds an executor to one model with the same owning lifetime.
   * @param model Model serialized by every submitted callable.
   * @param queue_capacity Maximum waiting tasks in WaitingOnly mode, or total
   *        admitted one-shot/ticket units in TotalAdmission mode. Production
   *        callers use `kDefaultQueueCapacity`; injection exists for tests.
   * @param capacity_mode Historical graph-state or exact total-admission
   *        accounting.
   * @throws std::invalid_argument if queue_capacity is zero.
   * @throws std::system_error if the sole worker cannot be created.
   * @note Worker ownership becomes active before construction returns.
   */
  explicit GraphStateExecutor(
      GraphModel& model, std::size_t queue_capacity = kDefaultQueueCapacity,
      CapacityMode capacity_mode = CapacityMode::WaitingOnly);

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
   * @note Submission blocks at the configured waiting-only or total-admission
   *       bound. Destroying the returned future does not cancel or wait for
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

  /**
   * @brief Reserves one persistent continuation admission without queuing it.
   * @param callback Non-empty callback retained through ticket retirement.
   * @return Parked ticket charged against the total-admission ledger.
   * @throws std::invalid_argument when callback is empty.
   * @throws std::logic_error on worker re-entry or a non-TotalAdmission lane.
   * @throws std::runtime_error after admission stops.
   * @throws std::bad_alloc if ticket/callback/map storage cannot allocate.
   * @throws std::overflow_error if ticket identity is exhausted.
   * @throws std::system_error if executor synchronization fails.
   * @note Reservation is the only continuation operation that may wait for
   * capacity. Its intrusive FIFO node is allocated before publication, so all
   * later wake and tail-handoff operations consume the same token and node.
   */
  ContinuationTicket reserve_continuation(ContinuationCallback callback);

  /**
   * @brief Returns the current total charged admission count.
   * @return Queued/running one-shots plus queued/running/parked tickets in
   * TotalAdmission mode; queued one-shots in WaitingOnly mode.
   * @throws std::system_error if executor synchronization fails.
   * @note This private observation supports deterministic capacity assertions;
   * it grants no mutation or lifecycle authority.
   */
  std::size_t admitted_units() const;

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
   * @brief Lifecycle of one charged continuation under `mutex_`.
   * @throws Nothing for value construction and comparison.
   */
  enum class ContinuationPhase {
    /** @brief Charged but absent from the FIFO. */
    Parked,
    /** @brief Its persistent node occurs exactly once in the FIFO. */
    Queued,
    /** @brief The sole worker is invoking its callback. */
    Running,
    /** @brief Admission was released and no future wake may succeed. */
    Retired,
  };

  /**
   * @brief Intrusive FIFO node allocated before any no-throw handoff.
   * @throws Nothing for default construction.
   * @note One-shot nodes are owned by `one_shot_nodes_`; continuation nodes are
   * embedded in their ticket state and reused for every turn.
   */
  struct QueueNode {
    /** @brief Kind of admitted unit represented by this node. */
    enum class Kind { OneShot, Continuation };

    /** @brief Node kind fixed for its complete lifetime. */
    Kind kind = Kind::OneShot;
    /** @brief Move-only one-shot callback; empty for continuation nodes. */
    std::packaged_task<void()> task;
    /** @brief Borrowed continuation state for a persistent ticket node. */
    ContinuationState* continuation = nullptr;
    /** @brief Intrusive next pointer owned by the executor FIFO. */
    QueueNode* next = nullptr;
  };

  /**
   * @brief Executor-retained state for one continuation reservation.
   * @throws std::bad_alloc when callback ownership allocates.
   * @note The embedded node gives wake and tail handoff a no-allocation path.
   */
  class ContinuationState {
   public:
    /**
     * @brief Creates one parked ticket with a persistent FIFO node.
     * @param identity Monotonic executor-local ticket identity.
     * @param callback_in Callback retained until retirement.
     * @throws std::bad_alloc when callback ownership allocates.
     */
    ContinuationState(std::uint64_t identity, ContinuationCallback callback_in)
        : id(identity), callback(std::move(callback_in)) {
      node.kind = QueueNode::Kind::Continuation;
      node.continuation = this;
    }

    /** @brief Monotonic executor-local reservation identity. */
    std::uint64_t id = 0;
    /** @brief Callback invoked outside the executor mutex. */
    ContinuationCallback callback;
    /** @brief Persistent intrusive node reused by every queued turn. */
    QueueNode node;
    /** @brief Current queued/running/parked/retired phase. */
    ContinuationPhase phase = ContinuationPhase::Parked;
    /** @brief One publication arrived while the callback was running. */
    bool wake_pending = false;
    /** @brief Retirement will win when queued/running work returns. */
    bool retire_pending = false;
    /** @brief Draining worker owes this parked ticket one accepted turn. */
    bool close_turn_pending = false;
  };

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
   * @brief Appends a pre-owned node to the intrusive FIFO without allocation.
   * @param node Node currently absent from the FIFO.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds `mutex_` and has already established admission.
   */
  void append_node_locked(QueueNode* node) noexcept;

  /**
   * @brief Removes the FIFO head without releasing its admission owner.
   * @return Removed node, or null when the FIFO is empty.
   * @throws Nothing.
   * @note Caller holds `mutex_`; active/retirement handling follows outside.
   */
  QueueNode* pop_node_locked() noexcept;

  /**
   * @brief Finds the oldest parked ticket owed a close-owned drain turn.
   * @return Borrowed matching state, or null when none is pending.
   * @throws Nothing.
   * @note Caller holds `mutex_`; identity order is deterministic and no
   * temporary collection is allocated.
   */
  ContinuationState* oldest_close_turn_locked() noexcept;

  /**
   * @brief Applies worker return and possibly hands a ticket to the FIFO tail.
   * @param ticket Running ticket retained by `continuations_`.
   * @param action Callback decision, or Retire after callback failure.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds `mutex_`; tail handoff uses the persistent node.
   */
  void finish_continuation_turn_locked(ContinuationState& ticket,
                                       ContinuationAction action) noexcept;

  /**
   * @brief Releases one nonqueued/nonrunning ticket admission.
   * @param ticket Parked or worker-returned ticket.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds `mutex_`; every admission counter decrements once.
   */
  void retire_continuation_locked(ContinuationState& ticket) noexcept;

  /**
   * @brief Implements public ticket wake with close rejection.
   * @param ticket Shared reservation state retained by the caller handle.
   * @return True when a live wake is accepted or already represented.
   * @throws std::system_error if executor synchronization fails.
   */
  bool wake_continuation(const std::shared_ptr<ContinuationState>& ticket);
  /**
   * @brief Implements accepted-owner resume during admission or drain.
   * @param ticket Shared reservation state retained by the accepted owner.
   * @return True while the reservation remains live.
   * @throws std::system_error if executor synchronization fails.
   */
  bool resume_continuation(const std::shared_ptr<ContinuationState>& ticket);
  /**
   * @brief Implements idempotent parked/deferred ticket retirement.
   * @param ticket Shared reservation state retained by its owner.
   * @return True only when this call records or completes retirement first.
   * @throws std::system_error if executor synchronization fails.
   */
  bool retire_continuation(const std::shared_ptr<ContinuationState>& ticket);

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

  /** @brief Borrowed model whose owner also owns this executor. */
  GraphModel& model_;
  /** @brief Protects FIFO ownership and the admission-stop flag. */
  mutable std::mutex mutex_;
  /** @brief Wakes the worker when work arrives or destruction begins. */
  std::condition_variable work_available_;
  /** @brief Wakes blocked producers when one queued task becomes active. */
  std::condition_variable space_available_;
  /** @brief Coordinates worker-stop and generation completion among closers. */
  std::condition_variable state_changed_;
  /** @brief Owns every admitted one-shot node until it becomes active. */
  std::unordered_map<QueueNode*, std::unique_ptr<QueueNode>> one_shot_nodes_;
  /** @brief Owns every charged continuation until exact retirement. */
  std::unordered_map<std::uint64_t, std::shared_ptr<ContinuationState>>
      continuations_;
  /** @brief Borrowed head of the allocation-free intrusive FIFO. */
  QueueNode* queue_head_ = nullptr;
  /** @brief Borrowed tail of the allocation-free intrusive FIFO. */
  QueueNode* queue_tail_ = nullptr;
  /** @brief Number of nodes currently present in the FIFO. */
  std::size_t queued_task_count_ = 0;
  /** @brief Sole thread permitted to invoke submitted graph-state callables. */
  std::thread worker_;
  /** @brief Fixed waiting-only or total-admission bound selected by mode. */
  const std::size_t queue_capacity_;
  /** @brief Waiting-only graph lane or exact total-admission request lane. */
  const CapacityMode capacity_mode_;
  /** @brief Total charged units in TotalAdmission mode. */
  std::size_t admitted_unit_count_ = 0;
  /** @brief Next executor-local continuation identity; zero is invalid. */
  std::uint64_t next_continuation_id_ = 1;
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
