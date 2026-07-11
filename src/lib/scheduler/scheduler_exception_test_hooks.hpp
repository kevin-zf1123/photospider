#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#error "scheduler_exception_test_hooks.hpp is BUILD_TESTING-only"
#endif

#include <cstddef>
#include <cstdint>

namespace ps::testing {

/**
 * @brief Scheduler lifecycle and queue mutation points available to tests.
 *
 * @note The enum is compiled only into BUILD_TESTING products. Production
 * scheduler binaries contain neither the injection branches nor hook storage.
 */
enum class SchedulerFailurePoint {
  /** @brief Before one batch queue insertion that may allocate. */
  BatchQueuePush,
  /** @brief Before one start-time container allocation or reserve stage. */
  StartResourceAllocation,
  /** @brief Before one CPU worker thread construction. */
  CpuThreadCreate,
  /** @brief Before one GPU worker thread construction. */
  GpuThreadCreate,
};

/**
 * @brief Borrowed throwing hook for deterministic scheduler failure injection.
 *
 * The scheduler supplies a one-based attempt number scoped to each lifecycle
 * operation. Tests may throw `std::bad_alloc` or `std::system_error` from the
 * callback to model the corresponding standard-library failure.
 */
struct SchedulerFailureInjectionHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;
  /**
   * @brief Invoked immediately before the selected mutation or construction.
   *
   * @param context Borrowed pointer from `context`.
   * @param point Mutation/lifecycle point about to run.
   * @param attempt One-based attempt number for this operation and point.
   * @return Nothing when execution should continue.
   * @throws Any exception selected by the deterministic test.
   */
  void (*before)(void* context, SchedulerFailurePoint point,
                 std::size_t attempt) = nullptr;
};

/**
 * @brief Queue/counter/worker state used to prove transactional rollback.
 */
struct SchedulerTransactionalStateSnapshot {
  /** @brief Whether the scheduler currently publishes a running lifecycle. */
  bool running = false;
  /** @brief Whether partially staged workers are allowed to remain in loops. */
  bool worker_loop_active = false;
  /** @brief Total callbacks visible across scheduler queues. */
  std::size_t queued_tasks = 0;
  /** @brief Published ready predicate across all scheduler queues. */
  std::int64_t ready_tasks = 0;
  /** @brief Logical completion count for the active batch. */
  std::int64_t tasks_to_complete = 0;
  /** @brief Dequeued callbacks that have not settled. */
  std::size_t in_flight_tasks = 0;
  /** @brief Active scheduler batch epoch. */
  std::uint64_t active_epoch = 0;
  /** @brief Last committed monotonically increasing scheduler epoch. */
  std::uint64_t epoch_counter = 0;
  /** @brief Number of scheduler-owned worker thread objects. */
  std::size_t worker_threads = 0;
  /** @brief Number of per-worker task queues. */
  std::size_t local_queues = 0;
  /** @brief Whether the active batch's first publisher latch is claimed. */
  bool exception_claimed = false;
  /** @brief Whether a batch exception is currently consumer-visible. */
  bool has_exception = false;
  /** @brief Whether the protected exception pointer is non-null. */
  bool has_exception_ptr = false;
  /** @brief Epoch associated with the protected exception publication. */
  std::uint64_t exception_epoch = 0;
  /** @brief Whether claimed-batch queue cleanup completed. */
  bool exception_cleanup_complete = false;
};

/**
 * @brief Immutable barrier callback used at exception-publication visibility.
 *
 * The callback runs after the scheduler's exception-visible flag has become
 * observable. Tests block the publishing worker here and inspect whether the
 * matching `std::exception_ptr` is already available under its mutex.
 */
struct SchedulerExceptionPublicationHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;
  /**
   * @brief Non-allocating callback invoked by the first exception publisher.
   *
   * @param context Borrowed pointer from `context`.
   * @return Nothing.
   * @throws Nothing; throwing from a test hook terminates the test process.
   */
  void (*after_flag_visible)(void* context) noexcept = nullptr;
  /**
   * @brief Callback invoked after queue cleanup and before flag publication.
   *
   * @param context Borrowed pointer from `context`.
   * @return Nothing.
   * @throws Nothing; throwing from a test hook terminates the test process.
   * @note The scheduler holds its complete queue transaction gate while this
   * callback runs. A concurrent initial-batch submitter must therefore block.
   */
  void (*before_flag_visible)(void* context) noexcept = nullptr;
};

/**
 * @brief Non-allocating barrier immediately before public running publication.
 *
 * The callback runs after the complete worker vector is installed but while
 * `is_running()` and `task_runtime_running()` must still return false. Tests
 * block start here and inspect the two-phase lifecycle state.
 */
struct SchedulerStartPublicationHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;
  /**
   * @brief Callback invoked immediately before the running release-store.
   * @param context Borrowed pointer from `context`.
   * @return Nothing.
   * @throws Nothing; throwing terminates the test process.
   */
  void (*before_running_visible)(void* context) noexcept = nullptr;
};

/**
 * @brief Consistent scheduler exception-publication state for tests.
 */
struct SchedulerExceptionPublicationSnapshot {
  /** @brief Whether consumers can observe an outstanding exception. */
  bool has_exception = false;
  /** @brief Whether the protected first-exception slot is non-null. */
  bool has_exception_ptr = false;
  /** @brief Whether queue cleanup finished before flag publication. */
  bool cleanup_complete = false;
  /** @brief Number of callbacks still executing in the observed batch. */
  std::size_t in_flight_tasks = 0;
  /** @brief Epoch paired with the protected exception pointer. */
  std::uint64_t exception_epoch = 0;
  /** @brief Queue-ready predicate remaining after exception cleanup. */
  std::int64_t ready_tasks = 0;
};

/**
 * @brief Non-allocating hook at the GPU CPU-worker wait handshake.
 *
 * The callback runs with the shared RT/HP-CPU queue mutex held immediately
 * before `rt_cv_` performs its atomic unlock-and-wait transition. A test can
 * attempt HP submission in this window and prove the publisher acquires the
 * same mutex before notifying.
 */
struct SchedulerCpuWaitHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;
  /** @brief Callback invoked immediately before CPU condition wait. */
  void (*before_wait)(void* context) noexcept = nullptr;
};

/**
 * @brief Non-allocating hook between CPU local enqueue and ready publication.
 *
 * The callback runs while both the global ready-predicate mutex and target
 * local-queue mutex are held. Tests start an exception publisher in this
 * window and prove cleanup cannot overtake the matching ready increment.
 */
struct SchedulerCpuLocalReadyHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;
  /** @brief Callback invoked after enqueue and before ready-count increment. */
  void (*between_enqueue_and_ready)(void* context) noexcept = nullptr;
};

/**
 * @brief Installs or clears the CPU scheduler publication barrier.
 *
 * @param hook Borrowed immutable hook, or nullptr to clear it.
 * @return Nothing.
 * @throws Nothing.
 * @note The caller must keep a non-null hook alive until it clears the hook and
 * all scheduler workers have left the callback.
 */
void set_cpu_scheduler_exception_publication_hook(
    const SchedulerExceptionPublicationHook* hook) noexcept;

/**
 * @brief Installs or clears the CPU start-publication barrier.
 * @param hook Borrowed immutable hook, or nullptr to clear it.
 * @return Nothing.
 * @throws Nothing.
 * @note The hook must remain alive until a blocked start is released and joins.
 */
void set_cpu_scheduler_start_publication_hook(
    const SchedulerStartPublicationHook* hook) noexcept;

/**
 * @brief Installs or clears the CPU local-ready publication hook.
 * @param hook Borrowed immutable hook, or nullptr to clear it.
 * @return Nothing.
 * @throws Nothing.
 * @note The caller releases any blocked callback before clearing the hook.
 */
void set_cpu_scheduler_local_ready_hook(
    const SchedulerCpuLocalReadyHook* hook) noexcept;

/**
 * @brief Installs or clears deterministic CPU scheduler failure injection.
 * @param hook Borrowed throwing hook, or nullptr to clear it.
 * @return Nothing.
 * @throws Nothing.
 * @note The hook must be cleared after the injecting call returns and before
 * its borrowed storage is destroyed.
 */
void set_cpu_scheduler_failure_injection_hook(
    const SchedulerFailureInjectionHook* hook) noexcept;

/**
 * @brief Reads CPU scheduler transactional state under all queue locks.
 * @param scheduler Opaque pointer to the concrete CPU scheduler.
 * @return Queue, counter, epoch, and worker ownership state.
 * @throws Nothing.
 */
SchedulerTransactionalStateSnapshot cpu_scheduler_transactional_snapshot(
    void* scheduler) noexcept;

/**
 * @brief Reads CPU scheduler publication state under its exception mutex.
 *
 * @param scheduler Opaque pointer to the concrete CPU scheduler under test.
 * @return Flag/pointer consistency snapshot.
 * @throws Nothing.
 * @note The pointer is opaque here to keep this test-only header independent
 * from the concrete scheduler header and avoid a production include cycle.
 */
SchedulerExceptionPublicationSnapshot
cpu_scheduler_exception_publication_snapshot(void* scheduler) noexcept;

/**
 * @brief Installs or clears the GPU-pipeline publication barrier.
 *
 * @param hook Borrowed immutable hook, or nullptr to clear it.
 * @return Nothing.
 * @throws Nothing.
 * @note The hook covers both CPU and available GPU worker exception entrances
 * because they converge on `GpuPipelineScheduler::set_exception`.
 */
void set_gpu_scheduler_exception_publication_hook(
    const SchedulerExceptionPublicationHook* hook) noexcept;

/**
 * @brief Installs or clears the GPU-pipeline start-publication barrier.
 * @param hook Borrowed immutable hook, or nullptr to clear it.
 * @return Nothing.
 * @throws Nothing.
 * @note The hook covers the initial complete CPU/GPU worker-set publication.
 */
void set_gpu_scheduler_start_publication_hook(
    const SchedulerStartPublicationHook* hook) noexcept;

/**
 * @brief Installs or clears the GPU pipeline CPU-wait handshake hook.
 * @param hook Borrowed immutable hook, or nullptr to clear it.
 * @return Nothing.
 * @throws Nothing.
 * @note This symbol is compiled only when BUILD_TESTING enables the internal
 * scheduler seams; production static libraries contain no hook state.
 */
void set_gpu_scheduler_cpu_wait_hook(const SchedulerCpuWaitHook* hook) noexcept;

/**
 * @brief Installs or clears deterministic GPU-pipeline failure injection.
 * @param hook Borrowed throwing hook, or nullptr to clear it.
 * @return Nothing.
 * @throws Nothing.
 */
void set_gpu_scheduler_failure_injection_hook(
    const SchedulerFailureInjectionHook* hook) noexcept;

/**
 * @brief Forces normal-priority batch routing to the GPU queue for tests.
 * @param enabled True to exercise GPU-queue transaction code without hardware.
 * @return Nothing.
 * @throws Nothing.
 * @note Production products contain no corresponding override branch.
 */
void set_gpu_scheduler_force_gpu_route(bool enabled) noexcept;

/**
 * @brief Reads GPU-pipeline transactional state under all queue locks.
 * @param scheduler Opaque pointer to the concrete pipeline scheduler.
 * @return Queue, counter, epoch, and worker ownership state.
 * @throws Nothing.
 */
SchedulerTransactionalStateSnapshot gpu_scheduler_transactional_snapshot(
    void* scheduler) noexcept;

/**
 * @brief Reads GPU-pipeline publication state under its exception mutex.
 *
 * @param scheduler Opaque pointer to the concrete GPU pipeline scheduler.
 * @return Flag/pointer consistency snapshot.
 * @throws Nothing.
 * @note This seam is compiled only into BUILD_TESTING static products.
 */
SchedulerExceptionPublicationSnapshot
gpu_scheduler_exception_publication_snapshot(void* scheduler) noexcept;

}  // namespace ps::testing
