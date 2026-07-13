// Photospider built-in heterogeneous pipeline scheduler.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "photospider/core/compute_intent.hpp"
#include "photospider/scheduler/scheduler.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "scheduler/scheduler_exception_test_hooks.hpp"
#endif

namespace ps {

/**
 * @brief Dispatches RT and HP ready work across CPU and optional GPU lanes.
 *
 * The scheduler owns CPU/GPU worker threads, RT/HP-CPU/GPU queues, committed
 * ready/completion counters, batch epochs, and exact exception publication.
 * It borrows `SchedulerHostContext` and executors referenced by `TaskHandle`;
 * it never owns graph topology, cache state, or compute task graphs. Every bulk
 * queue mutation is committed atomically under the selected lane mutex.
 *
 * @return Concrete heterogeneous `IScheduler` with inherited task runtime.
 * @throws std::bad_alloc from explicit staging/submission methods.
 * @throws std::system_error from explicit worker lifecycle methods.
 * @note RT and HP priority routing is scheduler policy, while borrowed handle
 * lifetime ends at the corresponding completion/exception settlement fence.
 */
class GpuPipelineScheduler : public IScheduler {
 public:
  /**
   * @brief Immutable-at-start worker and HP routing configuration.
   * @return Value configuration copied into the scheduler.
   * @throws Nothing.
   * @note Values control resources only; they do not define task graphs, cache
   * authority, or operation selection ownership.
   */
  struct Config {
    /** @brief GPU submission worker count when a Metal device is available. */
    unsigned int gpu_workers;
    /** @brief RT/HP-fallback CPU workers; zero selects hardware concurrency. */
    unsigned int cpu_workers;
    /** @brief Whether normal-priority HP work prefers an available GPU lane. */
    bool prefer_gpu_for_hp;

    /**
     * @brief Creates the default one-GPU/automatic-CPU routing configuration.
     * @return Default scheduler configuration value.
     * @throws Nothing.
     * @note GPU availability remains runtime-dependent after attachment.
     */
    Config() : gpu_workers(1), cpu_workers(0), prefer_gpu_for_hp(true) {}
  };

  /**
   * @brief Configures a stopped GPU-pipeline scheduler.
   * @param config Worker counts and HP routing preference copied by value.
   * @return Stopped scheduler with no owned worker threads.
   * @throws Nothing directly; zero CPU workers fall back to at least one.
   * @note No runtime, graph/cache state, or task executor is owned yet.
   */
  explicit GpuPipelineScheduler(const Config& config = Config());

  /**
   * @brief Stops and releases all scheduler-owned CPU/GPU workers.
   * @return Nothing.
   * @throws Nothing by destructor contract for valid built-in worker ownership.
   * @note Borrowed host context and task executors must satisfy their lifecycle
   * contracts before destruction begins.
   */
  ~GpuPipelineScheduler() override;

  /** @brief Prevents copying worker, queue, and borrowed-host ownership. */
  GpuPipelineScheduler(const GpuPipelineScheduler&) = delete;

  /** @brief Prevents copy assignment of worker and routing state. */
  GpuPipelineScheduler& operator=(const GpuPipelineScheduler&) = delete;

  // ---------------------------------------------------------------------------
  // IScheduler implementation
  // ---------------------------------------------------------------------------
  /**
   * @brief Attaches a borrowed host context and starts available GPU workers.
   * @param host Host context that outlives shutdown and detach.
   * @return Nothing.
   * @throws std::bad_alloc if an already-running scheduler cannot stage GPU
   * worker storage.
   * @throws std::system_error if an attached GPU worker cannot be created.
   * @note Host/graph/cache ownership is not transferred. Failure leaves
   * the scheduler stopped after joining any partially created worker set.
   */
  void attach(SchedulerHostContext& host) override;

  /**
   * @brief Clears the borrowed host capability/context pointer.
   * @return Nothing.
   * @throws Nothing.
   * @note The owning host orders shutdown before detach; queues are unchanged.
   */
  void detach() override;
  /**
   * @brief Starts CPU workers and any currently available GPU workers.
   *
   * @return Nothing.
   * @throws std::bad_alloc if worker storage cannot grow.
   * @throws std::system_error if a worker thread cannot be created.
   * @note Worker containers are reserved before publication. Partially created
   * workers use `worker_loop_active_`, while public `running_` remains false
   * until both complete worker vectors are installed. Any CPU/GPU thread
   * creation failure stops and joins the partial set, clears queues/counters,
   * and preserves the original exception for retry.
   */
  void start() override;

  /**
   * @brief Stops CPU/GPU dispatch and joins every scheduler-owned worker.
   *
   * The stop state is published while holding the mutexes used by idle CPU,
   * idle GPU, and completion waiters. After releasing those mutexes, shutdown
   * wakes each condition variable, joins the workers, and discards queued
   * callbacks that were not executed.
   *
   * @return Nothing.
   * @throws std::system_error if mutex acquisition or joining a valid worker
   * thread fails.
   * @note This lifecycle boundary prevents a worker from observing the old
   * running state between its predicate check and condition-variable sleep.
   * The caller must not invoke shutdown from a scheduler-owned worker. The
   * scheduler owns worker threads and queued callbacks, but not host context.
   */
  void shutdown() override;
  /**
   * @brief Returns the stable pipeline scheduler display name.
   * @return `GpuPipelineScheduler`.
   * @throws std::bad_alloc if result string allocation fails.
   * @note The name contains no lifecycle, graph, cache, or queue ownership.
   */
  std::string name() const override;

  /**
   * @brief Formats CPU/GPU worker, ready, execution, and scheduling counters.
   * @return Human-readable diagnostic snapshot.
   * @throws std::bad_alloc if stream/string growth fails.
   * @note Relaxed counters are observability only, not completion predicates.
   */
  std::string get_stats() const override;

  /**
   * @brief Reports whether the complete pipeline worker set is running.
   * @return Acquire-loaded lifecycle state.
   * @throws Nothing.
   * @note Failed start/attach worker creation restores false before returning.
   */
  bool is_running() const override;

  // ---------------------------------------------------------------------------
  // Built-in scheduler API
  // ---------------------------------------------------------------------------
  /** @brief Internal callback alias retained for built-in compute helpers. */
  using Task = SchedulerTaskRuntime::Task;
  /** @brief Internal priority alias shared with public runtime submissions. */
  using TaskPriority = SchedulerTaskPriority;

  /**
   * @brief Enqueues one high-priority real-time CPU callback.
   *
   * @param task Callback whose ownership moves into the RT queue.
   * @param epoch Originating batch epoch, or zero to use the active epoch.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note Enqueue, ready-count publication, exception-claim rejection, and
   * stale-epoch rejection share `rt_queue_mutex_` with the CPU wait predicate.
   */
  void submit_rt_task(Task&& task, uint64_t epoch = 0);

  /**
   * @brief Enqueues one RT task handle on the high-priority CPU queue.
   *
   * @param handle Dispatcher-owned ready task handle.
   * @param epoch Scheduler epoch used for lazy cancellation.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note The handle path avoids per-entry closure allocation for tile-level
   * ready work. Publication and the CPU wait predicate share
   * `rt_queue_mutex_`.
   */
  void submit_rt_task_handle(TaskHandle handle, uint64_t epoch = 0);

  /**
   * @brief Enqueues one high-precision callback on the CPU fallback queue.
   *
   * @param task Callback whose ownership moves into the HP-CPU queue.
   * @param epoch Originating batch epoch, or zero to use the active epoch.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note HP-CPU and RT publication use the same `rt_queue_mutex_`/`rt_cv_`
   * predicate handshake, and reject claimed or stale batches under that lock.
   */
  void submit_hp_task(Task&& task, uint64_t epoch = 0);

  /**
   * @brief Enqueues one HP task handle on the normal-priority CPU queue.
   *
   * @param handle Dispatcher-owned ready task handle.
   * @param epoch Scheduler epoch used for lazy cancellation.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note Publication shares `rt_queue_mutex_`/`rt_cv_` with RT work.
   */
  void submit_hp_task_handle(TaskHandle handle, uint64_t epoch = 0);

  /**
   * @brief Enqueues one high-precision callback on the GPU queue.
   *
   * @param task Callback whose ownership moves into the GPU queue.
   * @param epoch Originating batch epoch, or zero to use the active epoch.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note The GPU queue mutex serializes publication with exception cleanup
   * and rejects claimed or stale batches before mutating queue state.
   */
  void submit_gpu_task(Task&& task, uint64_t epoch = 0);

  /**
   * @brief Enqueues one task handle on the GPU queue.
   *
   * @param handle Dispatcher-owned ready task handle.
   * @param epoch Scheduler epoch used for lazy cancellation.
   * @return Nothing.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note Publication is serialized with GPU exception cleanup.
   */
  void submit_gpu_task_handle(TaskHandle handle, uint64_t epoch = 0);

  /**
   * @brief Waits for completion or consumes the batch's first exception.
   *
   * @return Nothing for a completed/stopped batch.
   * @throws The exact first worker exception published by CPU or GPU paths.
   * @note Success and failure both wait for the batch's in-flight CPU/GPU
   * callbacks to settle. Failure additionally requires every queue to be
   * drained before the exception flag is visible. Pointer/epoch/flag
   * consumption is mutex-consistent, so immediate next-batch reuse cannot be
   * affected by an old CPU or GPU publisher. Remaining-count cleanup is also
   * mutex/epoch guarded against a newer batch.
   */
  void wait_for_completion() override;

  /**
   * @brief Decrements the active batch's logical completion count.
   *
   * @return Nothing.
   * @throws Nothing under valid completion-mutex state.
   * @note Calls from a nonzero stale worker epoch and decrements at zero are
   * ignored. The waiter also requires every dequeued CPU/GPU callback to leave
   * `in_flight_tasks_`.
   */
  void dec_tasks_to_complete() override;

  /**
   * @brief Adds logical work to the active pipeline batch.
   *
   * @param delta Positive number of logical tasks added by the caller.
   * @return Nothing.
   * @throws std::overflow_error if a current-epoch addition would exceed
   * `INT_MAX`.
   * @throws std::system_error only if locking a valid mutex fails.
   * @note Nonpositive deltas and calls from nonzero stale worker epochs are
   * ignored. Validation and mutation share the new-batch publication mutex, so
   * old callbacks cannot pass validation and then change a later batch.
   */
  void inc_tasks_to_complete(int delta) override;

  /**
   * @brief Publishes the first CPU/GPU worker exception for this batch.
   *
   * @param e Non-null worker exception identity.
   * @return Nothing.
   * @throws Nothing under valid scheduler mutex state.
   * @note Null input is ignored. Otherwise, the publishing worker epoch must
   * match the active batch. A separate claim latch selects the first publisher;
   * it stores pointer and epoch, drains RT, HP-CPU, and GPU queues, marks
   * cleanup complete, then publishes the release-visible flag. Duplicate and
   * stale publishers are ignored.
   */
  void set_exception(std::exception_ptr e) override;

  /**
   * @brief Begins a batch and routes its initial callbacks by priority/device.
   *
   * @param tasks Immediately ready callbacks transferred to the scheduler.
   * @param total_task_count Logical completion count for the whole batch.
   * @param priority High selects RT CPU; normal selects GPU when available or
   * the HP-CPU fallback otherwise.
   * @return Nothing.
   * @throws std::logic_error if the scheduler is not running.
   * @throws std::invalid_argument if the count is negative or smaller than the
   *         number of valid callbacks.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note Validation precedes lane selection and state mutation. The selected
   * RT/HP-CPU/GPU deque records its original size and rolls back without
   * allocation on failure. Epoch, counters, and notification publish only
   * after the entire batch commits.
   */
  void submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count,
                            TaskPriority priority = TaskPriority::Normal);

  /**
   * @brief Begins a batch and routes its initial dispatcher-owned handles.
   *
   * @param handles Immediately ready task handles; empty handles are skipped.
   * @param total_task_count Logical completion count for the whole batch.
   * @param priority High selects RT CPU; normal selects GPU or HP-CPU fallback.
   * @return Nothing.
   * @throws std::logic_error if the scheduler is not running.
   * @throws std::invalid_argument if the count is negative or smaller than the
   *         number of valid handles.
   * @throws std::bad_alloc if queue growth fails before publication.
   * @note Validation precedes lane selection and state mutation. Handles borrow
   * dispatcher-owned executors for the batch lifetime. Failed insertion
   * restores the selected lane before epoch/counter publish, so no partial
   * handle can execute after caller stack unwinding.
   */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief Routes one worker-produced ready callback within its current epoch.
   * @param task Callback transferred to the selected queue.
   * @param priority High selects RT CPU; normal selects HP routing.
   * @return Nothing.
   * @throws std::bad_alloc if selected queue growth fails.
   * @note The worker TLS epoch is forwarded for stale-batch rejection.
   */
  void submit_ready_task_from_worker(
      Task&& task, TaskPriority priority = TaskPriority::Normal);

  /**
   * @brief Routes one worker-produced ready handle within its current epoch.
   * @param handle Dispatcher-owned task handle.
   * @param priority High selects RT CPU; normal selects HP routing.
   * @return Nothing.
   * @throws std::bad_alloc if selected queue growth fails.
   * @note The worker TLS epoch is forwarded for stale-batch rejection.
   */
  void submit_ready_task_handle_from_worker(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal);

  /**
   * @brief Routes a worker-produced batch of ready handles in one publication.
   * @param handles Dispatcher-owned handles; empty handles are skipped.
   * @param priority High selects RT CPU; normal selects HP routing.
   * @return Nothing.
   * @throws std::bad_alloc if selected queue growth fails.
   * @note The worker TLS epoch is forwarded for stale-batch rejection.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal) override;

  /**
   * @brief Routes one ready callback submitted from any thread.
   * @param task Callback transferred to the selected queue.
   * @param priority High selects RT CPU; normal selects HP routing.
   * @param epoch Optional originating epoch; active epoch is used when absent.
   * @return Nothing.
   * @throws std::bad_alloc if selected queue growth fails.
   * @note Selected queue publication rejects claimed and stale batches under
   * the same mutex used by exception cleanup.
   */
  void submit_ready_task_any_thread(
      Task&& task, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override;

  /**
   * @brief Routes one ready handle submitted from any thread.
   * @param handle Dispatcher-owned task handle.
   * @param priority High selects RT CPU; normal selects HP routing.
   * @param epoch Optional originating epoch; active epoch is used when absent.
   * @return Nothing.
   * @throws std::bad_alloc if selected queue growth fails.
   * @note Selected queue publication is serialized with exception cleanup.
   */
  void submit_ready_task_handle_any_thread(
      TaskHandle handle, TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt);

  /**
   * @brief Routes a batch of ready handles submitted from any thread.
   * @param handles Dispatcher-owned handles; empty handles are skipped.
   * @param priority High selects RT CPU; normal selects HP routing.
   * @param epoch Optional originating epoch; active epoch is used when absent.
   * @return Nothing.
   * @throws std::bad_alloc if selected queue growth fails.
   * @note Queue, ready predicate, claim check, and epoch check share one mutex
   * per selected execution lane. Original-size rollback removes every partial
   * entry before rethrow; ready counts and notification publish only on commit.
   */
  void submit_ready_task_handles_any_thread(
      std::vector<TaskHandle>&& handles,
      TaskPriority priority = TaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt);

  /**
   * @brief Copies one scheduler action into the attached host trace.
   * @param action Scheduler-facing action category.
   * @param node_id Graph node identifier associated with the action.
   * @return Nothing.
   * @throws Nothing.
   * @note A detached scheduler drops the event; worker id/epoch are copied from
   * TLS without transferring graph, cache, or runtime ownership.
   */
  void log_event(SchedulerTraceAction action, int node_id) override;

  // ---------------------------------------------------------------------------
  // Epoch management
  // ---------------------------------------------------------------------------
  /**
   * @brief Reads the currently committed pipeline batch epoch.
   * @return Active epoch using acquire ordering.
   * @throws Nothing.
   * @note Failed queue transactions do not advance the epoch.
   */
  uint64_t active_epoch() const;

  /**
   * @brief Commits the next nonzero pipeline epoch.
   * @return Newly active nonzero epoch.
   * @throws Nothing.
   * @note The sequence wraps `UINT64_MAX` to one. Nonempty transactional
   * batches publish their staged epoch only after the selected queue commits.
   */
  uint64_t begin_new_epoch();

  /**
   * @brief Checks whether a nonzero queued epoch is stale.
   * @param epoch Epoch carried by a CPU/GPU queue entry.
   * @return True when a nonzero epoch differs from the active epoch.
   * @throws Nothing.
   * @note Dropping stale work does not mutate compute graphs or cache state.
   */
  bool should_cancel_epoch(uint64_t epoch) const;

  /**
   * @brief Reads the current callback's worker-local batch epoch.
   * @return TLS epoch, or zero outside callback execution.
   * @throws Nothing.
   * @note The value is copied diagnostic metadata and owns no batch state.
   */
  static uint64_t this_task_epoch();

  /**
   * @brief Reads the current CPU/GPU worker identifier.
   * @return TLS worker id, or -1 outside scheduler-owned threads.
   * @throws Nothing.
   * @note Worker ids remain valid only for the active worker lifecycle.
   */
  static int this_worker_id();

  // ---------------------------------------------------------------------------
  // Device capability queries
  // ---------------------------------------------------------------------------

  /**
   * @brief Checks whether the attached runtime exposes a Metal device.
   * @return True only for a non-null runtime/device on Apple platforms.
   * @throws Nothing.
   * @note This is resource availability, not ownership or cache authority.
   */
  bool is_gpu_available() const;

  /**
   * @brief Returns devices that HP production compute may target here.
   *
   * @return CPU plus GPU_METAL only when a Metal device is attached and HP GPU
   * dispatch is enabled by config and worker availability.
   * @throws std::bad_alloc if vector allocation fails.
   * @note This list feeds TaskSubmissionPlan operation resolution. It must
   * remain aligned with can_dispatch_hp_to_gpu() so disabled GPU workers or
   * CPU-for-HP configurations do not select GPU implementations for CPU queues.
   */
  std::vector<Device> get_available_devices() const;

  /**
   * @brief Reports devices available to compute tasks submitted here.
   *
   * @return Same list as get_available_devices().
   * @throws std::bad_alloc if vector allocation fails.
   * @note This overrides SchedulerTaskRuntime so production operation
   * resolution can choose registered per-device implementations only for
   * devices this scheduler is currently willing to dispatch.
   */
  std::vector<Device> available_devices() const override;

 private:
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  /** @brief Grants the BUILD_TESTING consistency probe mutex-safe read access.
   */
  friend testing::SchedulerExceptionPublicationSnapshot
  testing::gpu_scheduler_exception_publication_snapshot(
      void* scheduler) noexcept;
  friend testing::SchedulerTransactionalStateSnapshot
  testing::gpu_scheduler_transactional_snapshot(void* scheduler) noexcept;
  friend void testing::set_gpu_scheduler_epoch_for_testing(
      void* scheduler, std::uint64_t epoch) noexcept;
#endif

  /**
   * @brief Queue-owned callback or borrowed task handle with intent and epoch.
   *
   * Callback entries own moved `std::function` storage. Handle entries borrow a
   * dispatcher `TaskExecutor` and cannot become worker-visible before the full
   * enclosing queue transaction commits.
   *
   * @return Movable RT/HP queue value.
   * @throws std::bad_alloc only while callback storage is constructed.
   * @note The entry owns no graph, task graph, cache, or runtime state.
   */
  struct ScheduledTask {
    /** @brief Batch epoch used for stale rejection and exception fencing. */
    uint64_t epoch{0};
    /** @brief Owned callback for closure-based submission. */
    Task task;
    /** @brief Non-owning dispatcher executor/id tuple. */
    TaskHandle handle;
    /** @brief Selects borrowed handle execution over the owned callback. */
    bool use_handle{false};
    /** @brief RT/HP intent used for queue policy statistics. */
    ComputeIntent intent{ComputeIntent::GlobalHighPrecision};

    /**
     * @brief Constructs an empty dequeue target.
     * @return Empty scheduled task.
     * @throws Nothing.
     * @note Owns and borrows nothing until move-assigned.
     */
    ScheduledTask() = default;
    /**
     * @brief Binds an owned callback to one intent/epoch.
     * @param e Scheduler batch epoch.
     * @param t Callback whose ownership moves into this entry.
     * @param i Compute intent used for routing/statistics.
     * @return Callback-backed scheduled task.
     * @throws Nothing when `std::function` move is non-throwing.
     * @note Captured request/cache state remains caller-defined and
     * batch-fenced.
     */
    ScheduledTask(uint64_t e, Task&& t,
                  ComputeIntent i = ComputeIntent::GlobalHighPrecision)
        : epoch(e), task(std::move(t)), intent(i) {}
    /**
     * @brief Binds a borrowed dispatcher handle to one intent/epoch.
     * @param e Scheduler batch epoch.
     * @param h Non-owning dispatcher handle copied into the queue entry.
     * @param i Compute intent used for routing/statistics.
     * @return Handle-backed scheduled task.
     * @throws Nothing.
     * @note The handle must not outlive matching batch settlement.
     */
    ScheduledTask(uint64_t e, TaskHandle h,
                  ComputeIntent i = ComputeIntent::GlobalHighPrecision)
        : epoch(e), handle(h), use_handle(true), intent(i) {}
    /**
     * @brief Tests validity of the selected callback/handle representation.
     * @return True for a callable task or valid borrowed handle.
     * @throws Nothing.
     * @note The test does not dereference the borrowed executor.
     */
    explicit operator bool() const {
      return use_handle ? static_cast<bool>(handle) : static_cast<bool>(task);
    }
    /**
     * @brief Invokes the selected callback or borrowed task executor.
     * @return Nothing.
     * @throws Any exception propagated by the callback or executor.
     * @note Caller owns worker TLS and in-flight settlement fencing.
     */
    void run() {
      if (use_handle) {
        handle.run();
      } else if (task) {
        task();
      }
    }
  };

  /**
   * @brief Runs one CPU worker over the shared RT/HP-CPU predicate.
   * @param thread_id Stable scheduler worker identifier.
   * @return Nothing until shutdown.
   * @throws Nothing escapes; callback exceptions enter `set_exception()`.
   * @note Dequeue increments the in-flight fence under `rt_queue_mutex_` and
   * settlement decrements it only after callback return.
   */
  void cpu_run_loop(int thread_id);

  /**
   * @brief Runs one GPU worker over the GPU queue predicate.
   * @param thread_id Stable scheduler worker identifier.
   * @return Nothing until shutdown.
   * @throws Nothing escapes; callback exceptions enter `set_exception()`.
   * @note Dequeue increments the shared in-flight fence under
   * `gpu_queue_mutex_` and settlement follows callback return.
   */
  void gpu_run_loop(int thread_id);

  /**
   * @brief Starts an available GPU worker set after runtime attachment.
   * @return Nothing.
   * @throws std::bad_alloc if staged worker storage cannot reserve.
   * @throws std::system_error if a GPU worker thread cannot be created.
   * @note Partial creation stops/joins all pipeline workers, clears lifecycle
   * counters, preserves the original exception, and supports full start retry.
   */
  void start_gpu_workers_if_available();

  /**
   * @brief Evaluates whether new HP work may enter the GPU queue.
   * @return True when policy, device, configured workers, and lifecycle allow.
   * @throws Nothing.
   * @note The decision routes ready work only; operation/cache ownership
   * remains in compute-service collaborators.
   */
  bool can_dispatch_hp_to_gpu() const;

  /**
   * @brief Clears synchronized exception state before a new pipeline batch.
   *
   * @return Nothing.
   * @throws std::system_error only if locking the valid exception mutex fails.
   * @note Reset covers pointer, exception epoch, cleanup state, visible flag,
   * and claim latch shared by CPU/GPU paths. Nonempty initial submission calls
   * it only after every selected-lane insertion succeeds and while both queue
   * mutexes remain held, immediately before epoch/counter commit. Start may
   * call it while the scheduler is stopped.
   */
  void reset_exception_state();

  /**
   * @brief Marks one dequeued CPU/GPU callback as fully settled.
   *
   * @return Nothing.
   * @throws Nothing under valid completion-mutex state.
   * @note Completion waiters require this count to reach zero before returning
   * either success or the batch exception, preventing old publishers from
   * crossing into a later batch.
   */
  void finish_in_flight_task() noexcept;

  /**
   * @brief Reserved helper for a future pipeline CPU work-stealing policy.
   * @param stealer_id Worker id that would request peer work.
   * @return A stolen entry or `std::nullopt` when no policy is implemented.
   * @throws Nothing in the current declaration-only unused path.
   * @note Current pipeline workers consume RT and HP-CPU FIFO queues directly.
   */
  std::optional<ScheduledTask> steal_task(int stealer_id);

  // ---------------------------------------------------------------------------
  // Member state
  // ---------------------------------------------------------------------------
  /** @brief Borrowed host context; owns no graph or cache state. */
  SchedulerHostContext* host_context_ = nullptr;
  /** @brief Scheduler-owned copy of worker/routing configuration. */
  Config config_;

  // CPU workers
  /** @brief CPU threads published only after complete start commit. */
  std::vector<std::thread> cpu_workers_;
  /** @brief Active CPU worker count, reset on failure and shutdown. */
  unsigned int num_cpu_workers_{0};

  // GPU workers
  /** @brief Optional GPU submission threads owned by this scheduler. */
  std::vector<std::thread> gpu_workers_;
  /** @brief Committed GPU worker count used by safe routing decisions. */
  unsigned int num_gpu_workers_{0};

  /** @brief Public lifecycle flag release-published after full worker install.
   */
  std::atomic<bool> running_{false};
  /** @brief Internal loop gate enabled while staged/committed workers may run.
   */
  std::atomic<bool> worker_loop_active_{false};

  // High-priority RT queue handled by CPU workers
  /** @brief High-priority RT CPU FIFO with transactional batch publication. */
  std::deque<ScheduledTask> rt_queue_;
  /**
   * @brief Shared CPU wait mutex for both RT and HP-CPU queue predicates.
   *
   * Every publisher that changes either ready predicate holds this mutex before
   * notifying `rt_cv_`, closing the predicate-check-to-wait lost-wakeup window.
   */
  std::mutex rt_queue_mutex_;
  /** @brief Sole condition variable used by CPU workers for RT/HP-CPU work. */
  std::condition_variable rt_cv_;

  /** @brief Normal-priority HP queue guarded by `rt_queue_mutex_`. */
  std::deque<ScheduledTask> hp_cpu_queue_;

  // GPU queue preferred for HP work
  /** @brief Normal-priority HP GPU FIFO with transactional publication. */
  std::deque<ScheduledTask> gpu_queue_;
  /** @brief Serializes GPU queue mutation, cleanup, and wait predicate. */
  std::mutex gpu_queue_mutex_;
  /** @brief Notification paired with `gpu_ready_count_` under GPU mutex. */
  std::condition_variable gpu_cv_;

  // Queue counters
  /** @brief Committed RT queue entries visible to CPU wait predicates. */
  std::atomic<int> rt_ready_count_{0};
  /** @brief Committed HP-CPU entries visible to the shared CPU predicate. */
  std::atomic<int> hp_cpu_ready_count_{0};
  /** @brief Committed GPU entries visible to the GPU wait predicate. */
  std::atomic<int> gpu_ready_count_{0};
  /** @brief Diagnostic count of CPU workers in idle-wait handling. */
  std::atomic<int> sleeping_cpu_count_{0};
  /** @brief Diagnostic count of GPU workers in idle-wait handling. */
  std::atomic<int> sleeping_gpu_count_{0};

  // Completion synchronization
  /**
   * @brief Serializes active-epoch/count publication with callback mutations.
   * @note Initial submissions acquire both queue gates before this mutex.
   *       Counter mutations acquire only this mutex, preventing cross-epoch
   *       writes.
   */
  std::mutex completion_count_mutex_;
  /** @brief Serializes logical/in-flight completion wait transitions. */
  std::mutex completion_mutex_;
  /** @brief Wakes waiters after completion, settlement, failure, or stop. */
  std::condition_variable cv_completion_;
  /** @brief Logical batch count published only after queue commit. */
  std::atomic<int> tasks_to_complete_{0};
  /** @brief Dequeued callbacks not yet returned to their worker loop. */
  std::atomic<int> in_flight_tasks_{0};

  // Epoch management
  /** @brief Last committed nonzero epoch; the sequence wraps maximum to one. */
  std::atomic<uint64_t> epoch_counter_{0};
  /** @brief Active epoch used by CPU/GPU stale and exception fencing. */
  std::atomic<uint64_t> active_epoch_{0};

  // Exception transport
  /** @brief Protects exact exception pointer publication and consumption. */
  std::mutex exception_mutex_;
  /** @brief First exact CPU/GPU worker exception for the active batch. */
  std::exception_ptr first_exception_;
  /** @brief First-publisher latch retained until the next batch reset. */
  std::atomic<bool> exception_claimed_{false};
  /** @brief Release-visible flag stored only after first_exception_. */
  std::atomic<bool> has_exception_{false};
  /** @brief Batch epoch paired with `first_exception_`. */
  std::atomic<uint64_t> exception_epoch_{0};
  /** @brief True only after all claimed-batch queues have been drained. */
  std::atomic<bool> exception_cleanup_complete_{false};

  // Statistics
  /** @brief RT callbacks successfully run by CPU workers. */
  std::atomic<uint64_t> rt_tasks_executed_{0};
  /** @brief HP fallback callbacks successfully run by CPU workers. */
  std::atomic<uint64_t> hp_cpu_tasks_executed_{0};
  /** @brief HP callbacks successfully run by GPU workers. */
  std::atomic<uint64_t> gpu_tasks_executed_{0};
  /** @brief Aggregate diagnostic scheduling count retained for stats. */
  std::atomic<uint64_t> total_tasks_scheduled_{0};

  // Thread-local storage
  /** @brief Current pipeline worker id or -1 outside scheduler threads. */
  static thread_local int tls_worker_id_;
  /** @brief Epoch of the callback currently executing on this thread. */
  static thread_local uint64_t tls_active_epoch_;
  /** @brief True only while the current callback runs on a GPU worker. */
  static thread_local bool tls_is_gpu_worker_;
};

}  // namespace ps
