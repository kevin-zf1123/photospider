#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "core/param_utils.hpp"
#include "core/ps_types.hpp"               // NOLINT(build/include_subdir)
#include "graph/graph_state_executor.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"                  // NOLINT(build/include_subdir)
#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
#include "graph/graph_state_executor_test_access.hpp"
#endif
#include "photospider/host/host.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"  // NOLINT(build/include_subdir)
#include "runtime/graph_runtime.hpp"  // NOLINT(build/include_subdir)
#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING) &&      \
    defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING) && \
    defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "runtime/kernel_required_target_test_access.hpp"
#endif
#include "scheduler/scheduler_plugin_loader.hpp"  // NOLINT(build/include_subdir)
#include "scheduler/serial_debug_scheduler.hpp"  // NOLINT(build/include_subdir)
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"

#ifndef PS_TEST_OP_PLUGIN_DIR
#define PS_TEST_OP_PLUGIN_DIR "build/test_plugins"
#endif

#ifndef PS_TEST_SCHEDULER_PLUGIN_PATH
#define PS_TEST_SCHEDULER_PLUGIN_PATH \
  "build/test_schedulers/libdestroy_count_scheduler_plugin.dylib"
#endif

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief BUILD_TESTING lifecycle-coordination events mirrored from the Host.
 * @throws Nothing; values are passed by value across a non-throwing callback.
 * @note The Host publishes events in close-serialization order. Test code uses
 *       atomic counters rather than retaining references to event values.
 */
enum class EmbeddedLifecycleTestEvent {
  /** @brief One close caller has claimed the session marker. */
  MarkerClaimed,
  /** @brief A duplicate close caller is about to wait for marker release. */
  DuplicateAboutToWait,
  /** @brief One synchronous session operation has entered admission. */
  SessionOperationAdmitted,
};

/**
 * @brief Borrowed callback installed for deterministic lifecycle coordination.
 * @throws Nothing; the callback contract is explicitly non-throwing.
 * @note Both `context` and this hook object remain owned by the test. Their
 *       lifetimes cover every serialized Host lifecycle callback until the
 *       hook is cleared.
 */
struct EmbeddedLifecycleTestHook {
  /** @brief Borrowed test context. */
  void* context = nullptr;
  /** @brief Non-throwing event callback. */
  void (*notify)(void* context,
                 EmbeddedLifecycleTestEvent event) noexcept = nullptr;
};

/**
 * @brief Installs or clears the embedded Host lifecycle test hook.
 * @param hook Hook that outlives concurrent lifecycle callbacks, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 */
void set_embedded_host_lifecycle_test_hook(
    const EmbeddedLifecycleTestHook* hook) noexcept;
#endif

#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
/**
 * @brief Host operations covered by deterministic admission-lifetime tests.
 * @throws Nothing for value construction and comparison.
 */
enum class EmbeddedOperationTestEvent {
  /** @brief Graph YAML reload operation. */
  ReloadGraph,
  /** @brief Graph YAML save operation. */
  SaveGraph,
  /** @brief Required-node YAML replacement operation. */
  SetNodeYaml,
  /** @brief Forward ROI projection operation. */
  ForwardRoiProjection,
  /** @brief Backward ROI projection operation. */
  BackwardRoiProjection,
  /** @brief Timing snapshot inspection operation. */
  Timing,
  /** @brief All-cache clearing operation. */
  ClearCache,
};

/**
 * @brief Checkpoints proving admission spans Kernel and public translation.
 * @throws Nothing for value construction and comparison.
 */
enum class EmbeddedOperationTestPhase {
  /** @brief Operation is admitted but has not entered Kernel. */
  BeforeKernelReady,
  /** @brief Blocker ended and pre-Kernel admission was sampled. */
  BeforeKernelAdmissionSnapshot,
  /** @brief Public result exists and pre-return admission was sampled. */
  AfterTranslationAdmissionSnapshot,
};

/**
 * @brief Borrowed blocking callback for one Host-operation checkpoint.
 * @throws Nothing for aggregate construction.
 */
struct EmbeddedOperationTestHook {
  /** @brief Borrowed test context. */
  void* context = nullptr;

  /**
   * @brief Observes and blocks one operation-lifetime checkpoint.
   * @param context Borrowed context supplied by the test.
   * @param event Operation reaching the checkpoint.
   * @param phase Admission-lifetime phase being observed.
   * @param admission_active Exact session-admission snapshot for snapshot
   *        phases; ignored for BeforeKernelReady.
   * @return Nothing.
   * @throws Nothing; implementations contain all synchronization failures.
   */
  void (*wait)(void* context, EmbeddedOperationTestEvent event,
               EmbeddedOperationTestPhase phase,
               bool admission_active) noexcept = nullptr;
};

/**
 * @brief Installs or clears the deterministic Host-operation test hook.
 * @param hook Hook that outlives all affected callbacks, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 */
void set_embedded_host_operation_test_hook(
    const EmbeddedOperationTestHook* hook) noexcept;
#endif

#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING) &&      \
    defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING) && \
    defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Test-owned gate for one required-target checkpoint.
 *
 * @throws std::bad_alloc if shared-future state copying allocates.
 * @note The immutable expected event filters unrelated operations. The release
 * future is fulfilled only after the competing public call has been started.
 */
struct RequiredTargetGateState {
  /** @brief Required-target checkpoint this gate accepts. */
  testing::RequiredTargetTestEvent expected;
  /** @brief Test-owned signal that releases the graph-state work item. */
  std::shared_future<void> release;
  /** @brief Number of matching checkpoints reached by the work item. */
  std::atomic<std::uint64_t> reached{0};
};

/**
 * @brief Blocks one matching required-target checkpoint without re-entry.
 *
 * @param context Borrowed RequiredTargetGateState pointer.
 * @param event Checkpoint reached by the real Kernel facade.
 * @return Nothing.
 * @throws Nothing; future wait failures are contained.
 */
void wait_at_required_target(void* context,
                             testing::RequiredTargetTestEvent event) noexcept {
  auto* gate = static_cast<RequiredTargetGateState*>(context);
  if (event != gate->expected) {
    return;
  }
  gate->reached.fetch_add(1, std::memory_order_release);
  try {
    if (gate->release.valid()) {
      gate->release.wait();
    }
  } catch (...) {
  }
}

/**
 * @brief Installs and clears one borrowed required-target gate.
 *
 * @throws Nothing after construction.
 * @note The owning test joins every affected future before this guard and its
 * borrowed state leave scope.
 */
class ScopedRequiredTargetTestHook final {
 public:
  /**
   * @brief Installs a gate backed by stable test-owned state.
   * @param state State that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedRequiredTargetTestHook(RequiredTargetGateState& state) noexcept
      : hook_{&state, &wait_at_required_target} {
    testing::set_required_target_test_hook(&hook_);
  }

  /** @brief Clears the hook before borrowed storage is destroyed. */
  ~ScopedRequiredTargetTestHook() noexcept {
    testing::set_required_target_test_hook(nullptr);
  }

  /**
   * @brief Disables duplicate installation ownership.
   * @param other Guard that retains hook ownership.
   * @throws Nothing because construction is unavailable.
   */
  ScopedRequiredTargetTestHook(const ScopedRequiredTargetTestHook& other) =
      delete;

  /**
   * @brief Disables replacement of an installed hook.
   * @param other Guard that retains hook ownership.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedRequiredTargetTestHook& operator=(
      const ScopedRequiredTargetTestHook& other) = delete;

 private:
  /** @brief Stable hook object borrowed by Kernel until guard destruction. */
  testing::RequiredTargetTestHook hook_;
};
#endif

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
/**
 * @brief Updates one atomic maximum without locking or allocation.
 * @param target Maximum observed by previous callbacks.
 * @param candidate Newly observed scalar value.
 * @return Nothing.
 * @throws Nothing.
 */
void update_atomic_max(std::atomic<std::size_t>& target,
                       std::size_t candidate) noexcept {
  std::size_t observed = target.load(std::memory_order_relaxed);
  while (observed < candidate &&
         !target.compare_exchange_weak(observed, candidate,
                                       std::memory_order_release,
                                       std::memory_order_relaxed)) {
  }
}

/**
 * @brief Retains bounded GraphStateExecutor observations for lane tests.
 * @throws Nothing for default construction and atomic updates.
 * @note The callback records only scalar maxima and queue events so it remains
 *       valid while invoked under the executor state mutex.
 */
struct GraphStateExecutorLaneState {
  /** @brief Number of nonempty TaskQueued checkpoints. */
  std::atomic<std::uint64_t> queued_events{0};
  /** @brief Fixed queue capacity reported by the observed executor. */
  std::atomic<std::size_t> queue_capacity{0};
  /** @brief Greatest waiting-task count observed. */
  std::atomic<std::size_t> max_queued_tasks{0};
  /** @brief Greatest active-task count observed. */
  std::atomic<std::size_t> max_active_tasks{0};
  /** @brief Greatest live lane-worker count observed. */
  std::atomic<std::size_t> max_worker_threads{0};
  /** @brief Number of close callers observed at lifecycle coordination. */
  std::atomic<std::uint64_t> close_wait_events{0};
  /** @brief Number of worker-stop checkpoints observed. */
  std::atomic<std::uint64_t> worker_stopped_events{0};
  /** @brief Number of failed-close rollback reopen checkpoints observed. */
  std::atomic<std::uint64_t> reopened_events{0};
};

/**
 * @brief Records one exact bounded-lane snapshot without blocking.
 * @param context Borrowed GraphStateExecutorLaneState pointer.
 * @param snapshot Exact scalar state captured by the executor.
 * @return Nothing.
 * @throws Nothing.
 */
void record_graph_state_executor_snapshot(
    void* context,
    const testing::GraphStateExecutorTestSnapshot& snapshot) noexcept {
  auto* state = static_cast<GraphStateExecutorLaneState*>(context);
  state->queue_capacity.store(snapshot.queue_capacity,
                              std::memory_order_release);
  update_atomic_max(state->max_queued_tasks, snapshot.queued_tasks);
  update_atomic_max(state->max_active_tasks, snapshot.active_tasks);
  update_atomic_max(state->max_worker_threads, snapshot.worker_threads);
  if (snapshot.event == testing::GraphStateExecutorTestEvent::TaskQueued &&
      snapshot.queued_tasks > 0) {
    state->queued_events.fetch_add(1, std::memory_order_release);
  }
  if (snapshot.event ==
      testing::GraphStateExecutorTestEvent::CloseCallerWaiting) {
    state->close_wait_events.fetch_add(1, std::memory_order_release);
  }
  if (snapshot.event == testing::GraphStateExecutorTestEvent::WorkerStopped) {
    state->worker_stopped_events.fetch_add(1, std::memory_order_release);
  }
  if (snapshot.event == testing::GraphStateExecutorTestEvent::Reopened) {
    state->reopened_events.fetch_add(1, std::memory_order_release);
  }
}

/**
 * @brief Installs and clears one deterministic bounded-lane observer.
 * @throws Nothing after construction.
 * @note The callback performs only atomic scalar updates and never blocks or
 *       re-enters the executor while its state mutex is held.
 */
class ScopedGraphStateExecutorTestHook final {
 public:
  /**
   * @brief Installs an observer backed by stable test-owned state.
   * @param state State that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedGraphStateExecutorTestHook(
      GraphStateExecutorLaneState& state) noexcept
      : hook_{&state, &record_graph_state_executor_snapshot} {
    testing::set_graph_state_executor_test_hook(&hook_);
  }

  /** @brief Clears the observer before borrowed state is destroyed. */
  ~ScopedGraphStateExecutorTestHook() noexcept {
    testing::set_graph_state_executor_test_hook(nullptr);
  }

  /**
   * @brief Disables duplicate observer ownership.
   * @param other Guard that retains observer ownership.
   * @throws Nothing because construction is unavailable.
   */
  ScopedGraphStateExecutorTestHook(
      const ScopedGraphStateExecutorTestHook& other) = delete;

  /**
   * @brief Disables replacement of an installed observer.
   * @param other Guard that retains observer ownership.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedGraphStateExecutorTestHook& operator=(
      const ScopedGraphStateExecutorTestHook& other) = delete;

 private:
  /** @brief Stable hook borrowed until guard destruction. */
  testing::GraphStateExecutorTestHook hook_;
};

/**
 * @brief State used to restart one fully joined lane before waiter
 * notification.
 * @throws Nothing for aggregate construction and atomic access.
 * @note The callback borrows `executor` and performs at most one restart. The
 *       owning test keeps the executor alive until every close future joins.
 */
struct ClosePublishRestartState {
  /** @brief Executor restarted in the deterministic publication window. */
  GraphStateExecutor* executor = nullptr;
  /** @brief Whether the callback has claimed its single restart attempt. */
  std::atomic<bool> restart_claimed{false};
  /** @brief Whether restart completed without throwing. */
  std::atomic<bool> restart_succeeded{false};
  /** @brief Whether restart raised an unexpected exception. */
  std::atomic<bool> restart_failed{false};
};

/**
 * @brief Restarts one joined executor before its close waiters are notified.
 * @param context Borrowed ClosePublishRestartState pointer.
 * @return Nothing.
 * @throws Nothing; every restart failure is recorded in atomic state.
 */
void restart_before_close_waiter_notification(void* context) noexcept {
  auto* state = static_cast<ClosePublishRestartState*>(context);
  bool expected = false;
  if (!state->restart_claimed.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return;
  }
  try {
    state->executor->restart_after_close_failure();
    state->restart_succeeded.store(true, std::memory_order_release);
  } catch (...) {
    state->restart_failed.store(true, std::memory_order_release);
  }
}

/**
 * @brief Installs one deterministic unlocked close-publication restart hook.
 * @throws Nothing after construction.
 * @note Destruction clears the process-global borrowed hook. The test must
 *       close the replacement worker before the borrowed state expires.
 */
class ScopedClosePublishRestartHook final {
 public:
  /**
   * @brief Installs a hook backed by stable test-owned restart state.
   * @param state State that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedClosePublishRestartHook(
      ClosePublishRestartState& state) noexcept
      : hook_{&state, &restart_before_close_waiter_notification} {
    testing::set_graph_state_executor_close_publish_test_hook(&hook_);
  }

  /** @brief Clears the hook before borrowed storage is destroyed. */
  ~ScopedClosePublishRestartHook() noexcept {
    testing::set_graph_state_executor_close_publish_test_hook(nullptr);
  }

  /**
   * @brief Disables duplicate hook ownership.
   * @param other Guard that retains the installed hook.
   * @throws Nothing because construction is unavailable.
   */
  ScopedClosePublishRestartHook(const ScopedClosePublishRestartHook& other) =
      delete;

  /**
   * @brief Disables replacement of an installed hook.
   * @param other Guard whose hook remains installed.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedClosePublishRestartHook& operator=(
      const ScopedClosePublishRestartHook& other) = delete;

 private:
  /** @brief Stable unlocked-publication hook borrowed by the executor. */
  testing::GraphStateExecutorClosePublishTestHook hook_;
};
#endif

namespace {

/** @brief Serializes access to the Host blocking-operation release future. */
std::mutex g_host_blocking_source_mutex;

/** @brief Test-controlled release observed by the blocking Host operation. */
std::shared_future<void> g_host_blocking_source_release;

/** @brief Publishes entry into the blocking Host operation callback. */
std::atomic<bool> g_host_blocking_source_started{false};

/** @brief Fill value written by the offset tiled Host test operation. */
std::atomic<int> g_offset_tiled_output_value{3};

/** @brief Number of offset tiled Host test operation invocations. */
std::atomic<int> g_offset_tiled_invocation_count{0};

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Events published by the current embedded close coordination hook.
 * @throws Nothing; fixed-size atomic counters do not allocate.
 * @note The hook borrows this state until guard destruction. Release/acquire
 *       ordering makes each serialized callback observation visible to the
 *       polling test thread.
 */
struct EmbeddedLifecycleEventState {
  /** @brief Number of callers that have claimed the marker. */
  std::atomic<std::uint64_t> marker_claimed{0};
  /** @brief Number of duplicate callers that reached a condition wait. */
  std::atomic<std::uint64_t> duplicate_about_to_wait{0};
  /** @brief Number of synchronous operations admitted after hook install. */
  std::atomic<std::uint64_t> session_operation_admitted{0};
};

/**
 * @brief Records one embedded lifecycle event without allocating or blocking.
 * @param context Borrowed EmbeddedLifecycleEventState pointer.
 * @param event Coordination point reached by the Host.
 * @return Nothing.
 * @throws Nothing.
 */
void record_embedded_lifecycle_event(
    void* context, EmbeddedLifecycleTestEvent event) noexcept {
  auto* state = static_cast<EmbeddedLifecycleEventState*>(context);
  if (event == EmbeddedLifecycleTestEvent::MarkerClaimed) {
    state->marker_claimed.fetch_add(1, std::memory_order_release);
  } else if (event == EmbeddedLifecycleTestEvent::DuplicateAboutToWait) {
    state->duplicate_about_to_wait.fetch_add(1, std::memory_order_release);
  } else {
    state->session_operation_admitted.fetch_add(1, std::memory_order_release);
  }
}

/**
 * @brief Installs and assertion-safely clears one embedded close test hook.
 * @throws Nothing after construction.
 * @note The guard borrows its event state and owns the stable hook object.
 *       Destruction clears the process-global hook before either can expire.
 */
class ScopedEmbeddedLifecycleTestHook final {
 public:
  /**
   * @brief Installs a hook backed by the supplied event state.
   * @param state State that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedEmbeddedLifecycleTestHook(
      EmbeddedLifecycleEventState& state) noexcept
      : hook_{&state, &record_embedded_lifecycle_event} {
    set_embedded_host_lifecycle_test_hook(&hook_);
  }

  /**
   * @brief Clears the borrowed hook before its state can be destroyed.
   * @throws Nothing.
   */
  ~ScopedEmbeddedLifecycleTestHook() noexcept {
    set_embedded_host_lifecycle_test_hook(nullptr);
  }

  /**
   * @brief Prevents duplicate hook-installation ownership.
   * @param other Guard that remains installed.
   * @throws Nothing because construction is unavailable.
   */
  ScopedEmbeddedLifecycleTestHook(
      const ScopedEmbeddedLifecycleTestHook& other) = delete;

  /**
   * @brief Prevents replacing one installed hook.
   * @param other Guard whose hook remains installed.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedEmbeddedLifecycleTestHook& operator=(
      const ScopedEmbeddedLifecycleTestHook& other) = delete;

 private:
  /** @brief Hook object whose address remains stable while installed. */
  EmbeddedLifecycleTestHook hook_;
};

#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
/**
 * @brief Test-owned three-phase gate for one admitted Host operation.
 * @throws std::bad_alloc if shared-future state copying allocates.
 * @note Each release future is fulfilled by the orchestration thread only
 *       after it has observed the corresponding atomic checkpoint.
 */
struct EmbeddedOperationGateState {
  /** @brief Operation event accepted by this gate. */
  EmbeddedOperationTestEvent expected;
  /** @brief Releases the admitted operation before it enters Kernel. */
  std::shared_future<void> before_kernel_ready_release;
  /** @brief Releases the pre-Kernel exact-admission snapshot. */
  std::shared_future<void> before_kernel_snapshot_release;
  /** @brief Releases the post-translation exact-admission snapshot. */
  std::shared_future<void> after_translation_snapshot_release;
  /** @brief Number of BeforeKernelReady callbacks reached. */
  std::atomic<std::uint64_t> before_kernel_ready_reached{0};
  /** @brief Number of pre-Kernel snapshot callbacks reached. */
  std::atomic<std::uint64_t> before_kernel_snapshot_reached{0};
  /** @brief Number of post-translation snapshot callbacks reached. */
  std::atomic<std::uint64_t> after_translation_snapshot_reached{0};
  /** @brief Exact admission value captured immediately before Kernel. */
  std::atomic<bool> before_kernel_admission_active{false};
  /** @brief Exact admission value captured after public result construction. */
  std::atomic<bool> after_translation_admission_active{false};
};

/**
 * @brief Blocks one matching Host-operation phase on its test-owned release.
 * @param context Borrowed EmbeddedOperationGateState pointer.
 * @param event Operation reaching the checkpoint.
 * @param phase Admission-lifetime phase being observed.
 * @param admission_active Exact session-admission snapshot for snapshot phases.
 * @return Nothing.
 * @throws Nothing; future wait failures are contained.
 */
void wait_at_embedded_operation_phase(void* context,
                                      EmbeddedOperationTestEvent event,
                                      EmbeddedOperationTestPhase phase,
                                      bool admission_active) noexcept {
  auto* gate = static_cast<EmbeddedOperationGateState*>(context);
  if (event != gate->expected) {
    return;
  }
  try {
    if (phase == EmbeddedOperationTestPhase::BeforeKernelReady) {
      gate->before_kernel_ready_reached.fetch_add(1, std::memory_order_release);
      gate->before_kernel_ready_release.wait();
    } else if (phase ==
               EmbeddedOperationTestPhase::BeforeKernelAdmissionSnapshot) {
      gate->before_kernel_admission_active.store(admission_active,
                                                 std::memory_order_release);
      gate->before_kernel_snapshot_reached.fetch_add(1,
                                                     std::memory_order_release);
      gate->before_kernel_snapshot_release.wait();
    } else {
      gate->after_translation_admission_active.store(admission_active,
                                                     std::memory_order_release);
      gate->after_translation_snapshot_reached.fetch_add(
          1, std::memory_order_release);
      gate->after_translation_snapshot_release.wait();
    }
  } catch (...) {
  }
}

/**
 * @brief Installs and clears one borrowed Host-operation gate.
 * @throws Nothing after construction.
 * @note The owning race scope joins every affected future before this guard is
 *       destroyed, so neither the hook nor context can expire mid-callback.
 */
class ScopedEmbeddedOperationTestHook final {
 public:
  /**
   * @brief Installs a gate backed by stable test-owned state.
   * @param state State that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedEmbeddedOperationTestHook(
      EmbeddedOperationGateState& state) noexcept
      : hook_{&state, &wait_at_embedded_operation_phase} {
    set_embedded_host_operation_test_hook(&hook_);
  }

  /** @brief Clears the hook after the race scope has joined all callbacks. */
  ~ScopedEmbeddedOperationTestHook() noexcept {
    set_embedded_host_operation_test_hook(nullptr);
  }

  /**
   * @brief Disables duplicate hook ownership.
   * @param other Guard retaining the installed hook.
   * @throws Nothing because construction is unavailable.
   */
  ScopedEmbeddedOperationTestHook(
      const ScopedEmbeddedOperationTestHook& other) = delete;

  /**
   * @brief Disables replacement of an installed hook.
   * @param other Guard retaining the installed hook.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedEmbeddedOperationTestHook& operator=(
      const ScopedEmbeddedOperationTestHook& other) = delete;

 private:
  /** @brief Stable hook object borrowed until guard destruction. */
  EmbeddedOperationTestHook hook_;
};
#endif

/**
 * @brief Waits until an atomic event reaches one occurrence count.
 * @param event Event counter to observe.
 * @param expected Minimum count required for success.
 * @param timeout Maximum monotonic wait duration.
 * @return True when the count becomes visible before the deadline.
 * @throws Nothing.
 */
bool wait_for_atomic_event_count(const std::atomic<std::uint64_t>& event,
                                 std::uint64_t expected,
                                 std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (event.load(std::memory_order_acquire) >= expected) {
      return true;
    }
    std::this_thread::yield();
  }
  return event.load(std::memory_order_acquire) >= expected;
}
#endif

/** @brief Environment key selecting scheduler fixture lifecycle failures. */
constexpr const char* kSchedulerFailureEnvironment =  // NOLINT
    "PS_DESTROY_COUNT_SCHEDULER_FAILURE";             // NOLINT

/** @brief Scheduler type exported by the deterministic close-failure fixture.
 */
constexpr const char* kDestroyCountSchedulerType = "destroy_count_test";

/**
 * @brief Configures one deterministic blocking Host operation invocation.
 *
 * @param release Shared future whose readiness releases the operation.
 * @return Nothing.
 * @throws Nothing except implementation-defined mutex system errors.
 * @note The operation copies this future while holding the same mutex and then
 * waits without the mutex, so test cleanup can always release it.
 */
void configure_host_blocking_source(std::shared_future<void> release) {
  std::lock_guard<std::mutex> lock(g_host_blocking_source_mutex);
  g_host_blocking_source_started.store(false, std::memory_order_release);
  g_host_blocking_source_release = std::move(release);
}

/**
 * @brief Clears the deterministic blocking Host operation state.
 *
 * @return Nothing.
 * @throws Nothing except implementation-defined mutex system errors.
 * @note Tests call this only after every operation using the prior future has
 * completed.
 */
void reset_host_blocking_source() {
  std::lock_guard<std::mutex> lock(g_host_blocking_source_mutex);
  g_host_blocking_source_release = std::shared_future<void>();
  g_host_blocking_source_started.store(false, std::memory_order_release);
}

/**
 * @brief Waits for the blocking Host operation to enter its callback.
 *
 * @param timeout Maximum monotonic duration to poll.
 * @return True when callback entry is observed before the deadline.
 * @throws Nothing.
 * @note Five-millisecond polling bounds test latency without imposing a fixed
 * callback execution sleep.
 */
bool wait_for_host_blocking_source(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (g_host_blocking_source_started.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return g_host_blocking_source_started.load(std::memory_order_acquire);
}

/**
 * @brief Registers deterministic operations used by embedded Host tests.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry storage allocation fails.
 * @note The operation is intentionally tiny and CPU-only so Host seam tests
 *       exercise frontend behavior without depending on external plugins or
 *       GPU availability. The `resource_exhausted` operation deliberately
 *       throws std::bad_alloc from real node execution so the public Host
 *       exception contract is tested through the complete backend chain.
 */
void register_host_adapter_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "source",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          const plugin::ParameterMap& params = node.runtime_parameters.empty()
                                                   ? node.parameters
                                                   : node.runtime_parameters;
          const int width = as_int_flexible(params, "width", 6);
          const int height = as_int_flexible(params, "height", 4);
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              width, height, 1, DataType::FLOAT32);
          cv::Mat mat = toCvMat(output.image_buffer);
          mat.setTo(7.0f);
          output.space.absolute_roi = PixelRect{0, 0, width, height};
          output.debug.compute_device = "host-adapter-test";
          return output;
        }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "slow_source",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          const plugin::ParameterMap& params = node.runtime_parameters.empty()
                                                   ? node.parameters
                                                   : node.runtime_parameters;
          std::this_thread::sleep_for(std::chrono::milliseconds(
              as_int_flexible(params, "sleep_ms", 50)));
          const int width = as_int_flexible(params, "width", 5);
          const int height = as_int_flexible(params, "height", 3);
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              width, height, 1, DataType::FLOAT32);
          cv::Mat mat = toCvMat(output.image_buffer);
          mat.setTo(3.0f);
          output.space.absolute_roi = PixelRect{0, 0, width, height};
          output.debug.compute_device = "host-adapter-slow-test";
          return output;
        }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "blocking_source",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          std::shared_future<void> release;
          {
            std::lock_guard<std::mutex> lock(g_host_blocking_source_mutex);
            release = g_host_blocking_source_release;
          }
          g_host_blocking_source_started.store(true, std::memory_order_release);
          if (release.valid()) {
            release.wait();
          }
          const plugin::ParameterMap& params = node.runtime_parameters.empty()
                                                   ? node.parameters
                                                   : node.runtime_parameters;
          const int width = as_int_flexible(params, "width", 5);
          const int height = as_int_flexible(params, "height", 3);
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              width, height, 1, DataType::FLOAT32);
          toCvMat(output.image_buffer).setTo(4.0f);
          output.space.absolute_roi = PixelRect{0, 0, width, height};
          output.debug.compute_device = "host-adapter-blocking-test";
          return output;
        }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "resized_extent",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          const plugin::ParameterMap& params = node.runtime_parameters.empty()
                                                   ? node.parameters
                                                   : node.runtime_parameters;
          const int width = as_int_flexible(params, "width", 6);
          const int height = as_int_flexible(params, "height", 4);
          const int roi_width = as_int_flexible(params, "roi_width", 12);
          const int roi_height = as_int_flexible(params, "roi_height", 9);
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              width, height, 1, DataType::FLOAT32);
          cv::Mat mat = toCvMat(output.image_buffer);
          mat.setTo(5.0f);
          output.space.absolute_roi = PixelRect{0, 0, roi_width, roi_height};
          output.space.inverse_matrix = {2.0, 0.0, 5.0, 0.0, 3.0,
                                         7.0, 0.0, 0.0, 1.0};
          output.space.local_inverse_matrix = {1.0,  0.0, 11.0, 0.0, 1.0,
                                               13.0, 0.0, 0.0,  1.0};
          output.debug.compute_device = "host-adapter-resized-test";
          return output;
        }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "no_image",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return NodeOutput{};
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "resource_exhausted",
        MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&)
                             -> NodeOutput { throw std::bad_alloc{}; }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "identity",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty() || inputs.front() == nullptr) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "host adapter identity requires one input");
              }
              const NodeOutput& input = *inputs.front();
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  input.image_buffer.width, input.image_buffer.height,
                  input.image_buffer.channels, input.image_buffer.type);
              toCvMat(input.image_buffer).copyTo(toCvMat(output.image_buffer));
              output.space.absolute_roi = input.space.absolute_roi;
              output.debug.compute_device = "host-adapter-identity-test";
              (void)node;
              return output;
            }));
    OpRegistry::instance().register_dirty_propagator(
        "host_adapter_test", "identity",
        DirtyRoiPropFunc(
            [](const Node&, const PixelRect& roi, const GraphModel&,
               const PixelSize&, const std::vector<PixelSize>&,
               const plugin::ParameterMap&,
               const std::vector<const NodeOutput*>*) { return roi; }));
    OpRegistry::instance().register_forward_propagator(
        "host_adapter_test", "identity",
        ForwardRoiPropFunc([](const Node&, const PixelRect& roi,
                              const GraphModel&, const PixelSize&,
                              const PixelSize&, size_t,
                              const std::vector<PixelSize>&,
                              const plugin::ParameterMap&) { return roi; }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "offset_identity",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>& inputs) {
          if (inputs.empty() || inputs.front() == nullptr) {
            throw GraphError(GraphErrc::InvalidParameter,
                             "host adapter offset_identity requires one input");
          }
          const NodeOutput& input = *inputs.front();
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              input.image_buffer.width, input.image_buffer.height,
              input.image_buffer.channels, input.image_buffer.type);
          toCvMat(input.image_buffer).copyTo(toCvMat(output.image_buffer));
          output.space.absolute_roi = input.space.absolute_roi;
          output.debug.compute_device = "host-adapter-offset-identity-test";
          (void)node;
          return output;
        }));
    OpRegistry::instance().register_dirty_propagator(
        "host_adapter_test", "offset_identity",
        DirtyRoiPropFunc([](const Node&, const PixelRect& roi,
                            const GraphModel&, const PixelSize&,
                            const std::vector<PixelSize>&,
                            const plugin::ParameterMap&,
                            const std::vector<const NodeOutput*>*) {
          return PixelRect{roi.x + 64, roi.y, roi.width, roi.height};
        }));
    OpMetadata offset_tiled_metadata;
    offset_tiled_metadata.tile_preference = TileSizePreference::MICRO;
    OpRegistry::instance().register_op_hp_tiled(
        "host_adapter_test", "offset_tiled_identity",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>& input_tiles) {
          if (input_tiles.size() != 1u) {
            throw GraphError(
                GraphErrc::InvalidParameter,
                "host adapter offset_tiled_identity requires one input");
          }
          g_offset_tiled_invocation_count.fetch_add(1,
                                                    std::memory_order_relaxed);
          const float output_value = static_cast<float>(
              g_offset_tiled_output_value.load(std::memory_order_relaxed));
          toCvMat(output_tile).setTo(output_value);
        }),
        offset_tiled_metadata);
    OpRegistry::instance().register_dirty_propagator(
        "host_adapter_test", "offset_tiled_identity",
        DirtyRoiPropFunc([](const Node&, const PixelRect& roi,
                            const GraphModel&, const PixelSize&,
                            const std::vector<PixelSize>&,
                            const plugin::ParameterMap&,
                            const std::vector<const NodeOutput*>*) {
          return PixelRect{roi.x + 64, roi.y, roi.width, roi.height};
        }));
  });
}

/**
 * @brief Owns a unique temporary directory for one Host adapter test.
 *
 * @throws std::filesystem::filesystem_error if setup cleanup or directory
 *         creation fails.
 * @note The destructor uses an error_code cleanup path so test assertions are
 *       not masked by best-effort removal failures.
 */
class ScopedTempDir {
 public:
  /**
   * @brief Creates an empty unique temporary directory.
   *
   * @param name Directory name below the platform temporary directory.
   * @throws std::filesystem::filesystem_error if directory creation fails.
   */
  explicit ScopedTempDir(const std::string& name)
      : root_(std::filesystem::temp_directory_path() /
              (name + "_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /**
   * @brief Prevents two owners from deleting the same temporary directory.
   * @param other Owner that retains cleanup responsibility.
   * @throws Nothing because construction is unavailable.
   */
  ScopedTempDir(const ScopedTempDir& other) = delete;

  /**
   * @brief Prevents replacing temporary-directory cleanup ownership.
   * @param other Owner whose root remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedTempDir& operator=(const ScopedTempDir& other) = delete;

  /**
   * @brief Removes the temporary directory.
   *
   * @throws Nothing.
   * @note Cleanup is best-effort so it cannot hide a test failure.
   */
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  /**
   * @brief Returns the root path for the temporary directory.
   *
   * @return Temporary root path.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Temporary directory root owned by this helper. */
  std::filesystem::path root_;
};

/**
 * @brief Temporarily sets one scheduler-fixture environment value.
 *
 * @throws std::bad_alloc if the key or previous value cannot be copied.
 * @throws std::runtime_error if the platform environment update fails.
 * @note Tests using this helper are process-serial because environment values
 *       are global. Destruction restores the exact prior value best-effort.
 */
class ScopedEnvironmentValue final {
 public:
  /**
   * @brief Saves the current value and installs one fixture selection.
   * @param name Environment key copied for this guard's lifetime.
   * @param value New value visible to the scheduler plugin.
   * @throws std::bad_alloc if owned strings cannot be allocated.
   * @throws std::runtime_error if the environment cannot be updated.
   */
  ScopedEnvironmentValue(const char* name, const std::string& value)
      : name_(name) {
    if (const char* previous = std::getenv(name)) {
      previous_ = std::string(previous);
    }
    set(value);
  }

  /**
   * @brief Restores the saved environment state without hiding test failures.
   * @throws Nothing; platform restoration failures are suppressed.
   */
  ~ScopedEnvironmentValue() noexcept {
    try {
      if (previous_) {
        set(*previous_);
      } else {
        clear();
      }
    } catch (...) {
    }
  }

  /**
   * @brief Prevents duplicate restoration ownership.
   * @param other Guard that remains the sole restoration owner.
   * @throws Nothing because construction is unavailable.
   */
  ScopedEnvironmentValue(const ScopedEnvironmentValue& other) = delete;

  /**
   * @brief Prevents replacing one active environment guard.
   * @param other Guard whose environment key remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedEnvironmentValue& operator=(const ScopedEnvironmentValue& other) =
      delete;

 private:
  /**
   * @brief Installs a new value for the owned key.
   * @param value Value to publish process-wide.
   * @return Nothing.
   * @throws std::runtime_error if the platform call fails.
   */
  void set(const std::string& value) {
#if defined(_WIN32)
    if (_putenv_s(name_.c_str(), value.c_str()) != 0) {
      throw std::runtime_error("_putenv_s failed");
    }
#else
    if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
      throw std::runtime_error("setenv failed");
    }
#endif
  }

  /**
   * @brief Removes the owned environment key.
   * @return Nothing.
   * @throws std::runtime_error if the platform call fails.
   */
  void clear() {
#if defined(_WIN32)
    if (_putenv_s(name_.c_str(), "") != 0) {
      throw std::runtime_error("_putenv_s clear failed");
    }
#else
    if (unsetenv(name_.c_str()) != 0) {
      throw std::runtime_error("unsetenv failed");
    }
#endif
  }

  /** @brief Environment key retained through restoration. */
  std::string name_;
  /** @brief Previous value, or nullopt when the key was absent. */
  std::optional<std::string> previous_;
};

/**
 * @brief Writes a single-node Host adapter test graph.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note The graph has one ending node so traversal, dependency-tree, compute,
 *       and image-returning Host APIs all observe the same deterministic node.
 */
void write_host_adapter_graph(const std::filesystem::path& path, int width = 6,
                              int height = 4,
                              const std::string& subtype = "source",
                              int slow_sleep_ms = 75) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: host_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: " << subtype << "\n"
      << "  parameters:\n"
      << "    width: " << width << "\n"
      << "    height: " << height << "\n";
  if (subtype == "slow_source") {
    out << "    sleep_ms: " << slow_sleep_ms << "\n";
  }
  if (subtype == "resized_extent") {
    out << "    roi_width: 12\n"
        << "    roi_height: 9\n";
  }
}

/**
 * @brief Writes a graph whose node has no registered operation.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Loading succeeds because operation lookup is deferred to compute, so
 *       Host image-compute failure mapping can verify GraphErrc::NoOperation.
 */
void write_host_adapter_unregistered_op_graph(
    const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: missing_op_source\n"
      << "  type: host_adapter_missing\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 6\n"
      << "    height: 4\n";
}

/**
 * @brief Writes a two-node graph with identity ROI propagation.
 *
 * @param path YAML file path to create.
 * @param source_subtype Source-node operation subtype.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Node 2 depends on node 1 through an image input and uses an explicit
 *       identity propagator, giving Host ROI tests deterministic forward and
 *       backward rectangles.
 */
void write_host_adapter_roi_graph(
    const std::filesystem::path& path,
    const std::string& source_subtype = "source") {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: roi_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: " << source_subtype << "\n"
      << "  parameters:\n"
      << "    width: 8\n"
      << "    height: 6\n"
      << "- id: 2\n"
      << "  name: roi_identity\n"
      << "  type: host_adapter_test\n"
      << "  subtype: identity\n"
      << "  image_inputs:\n"
      << "    - from_node_id: 1\n";
}

/**
 * @brief Writes a two-node graph whose backward dirty ROI differs by edge side.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Node 2 uses a tiled callback plus a deterministic test-only dirty
 *       propagator that shifts the upstream demand by one HP micro-tile, so
 *       Host conversion tests can observe generic tiled execution and catch
 *       accidental swaps of `from_roi` and `to_roi`.
 */
void write_host_adapter_offset_roi_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: roi_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 256\n"
      << "    height: 128\n"
      << "- id: 2\n"
      << "  name: roi_offset_identity\n"
      << "  type: host_adapter_test\n"
      << "  subtype: offset_tiled_identity\n"
      << "  image_inputs:\n"
      << "    - from_node_id: 1\n";
}

/**
 * @brief Returns the lifecycle operation plugin fixture directory.
 *
 * @return Directory containing the platform-specific lifecycle plugin library.
 * @throws std::bad_alloc if path construction allocates and fails.
 * @note CMake injects `PS_TEST_OP_PLUGIN_DIR` so the path follows the active
 *       binary directory instead of assuming a source-relative `build/` tree.
 */
std::filesystem::path lifecycle_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR) / "lifecycle";
}

/**
 * @brief Returns the replacement lifecycle plugin fixture directory.
 *
 * @return Directory containing the platform-specific override plugin library.
 * @throws std::bad_alloc if path construction allocates and fails.
 * @note The fixture replaces the same canonical operation key as the lifecycle
 *       plugin so multiple Host instances can exercise one restoration chain.
 */
std::filesystem::path override_lifecycle_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR) / "override";
}

/**
 * @brief Returns the deterministic scheduler lifecycle fixture library path.
 *
 * @return Platform-specific path below the CMake test scheduler directory.
 * @throws std::bad_alloc if path or filename construction cannot allocate.
 * @note The existing fixture can throw from real scheduler shutdown while
 *       retaining running state, allowing a later close retry to prove that
 *       shutdown is attempted again.
 */
std::filesystem::path destroy_count_scheduler_plugin_path() {
  return std::filesystem::path(PS_TEST_SCHEDULER_PLUGIN_PATH);
}

/**
 * @brief Owns fixture lifecycle exports resolved from the exact plugin path.
 *
 * @throws std::runtime_error when the library or a required export cannot be
 *         opened.
 * @note The diagnostic handle remains open while the Host loader owns its own
 *       mapping, then closes independently after all counter reads finish.
 */
class SchedulerFixtureExports final {
 public:
  /**
   * @brief Opens one scheduler fixture and resolves reset/shutdown counters.
   * @param path Complete platform-specific library path injected by CMake.
   * @throws std::runtime_error when opening or symbol lookup fails.
   */
  explicit SchedulerFixtureExports(const std::filesystem::path& path) {
#if defined(_WIN32)
    handle_ = LoadLibrary(path.string().c_str());
    if (handle_ != nullptr) {
      reset_counts_ = reinterpret_cast<void (*)()>(
          GetProcAddress(handle_, "ps_test_scheduler_reset_counts"));
      shutdown_count_ = reinterpret_cast<int (*)()>(
          GetProcAddress(handle_, "ps_test_scheduler_shutdown_count"));
    }
#else
    handle_ = dlopen(path.string().c_str(), RTLD_LAZY);
    if (handle_ != nullptr) {
      reset_counts_ = reinterpret_cast<void (*)()>(
          dlsym(handle_, "ps_test_scheduler_reset_counts"));
      shutdown_count_ = reinterpret_cast<int (*)()>(
          dlsym(handle_, "ps_test_scheduler_shutdown_count"));
    }
#endif
    if (handle_ == nullptr || reset_counts_ == nullptr ||
        shutdown_count_ == nullptr) {
      close();
      throw std::runtime_error(
          "failed to resolve scheduler lifecycle fixture exports: " +
          path.string());
    }
  }

  /** @brief Closes the diagnostic library handle. @throws Nothing. */
  ~SchedulerFixtureExports() noexcept { close(); }

  /**
   * @brief Prevents two owners from closing the same diagnostic handle.
   * @param other Owner that retains the native library handle.
   * @throws Nothing because construction is unavailable.
   */
  SchedulerFixtureExports(const SchedulerFixtureExports& other) = delete;

  /**
   * @brief Prevents replacing diagnostic-handle ownership.
   * @param other Owner whose handle remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  SchedulerFixtureExports& operator=(const SchedulerFixtureExports& other) =
      delete;

  /**
   * @brief Resets fixture counters while no scheduler instance is active.
   * @return Nothing.
   * @throws Nothing.
   */
  void reset_counts() const noexcept { reset_counts_(); }

  /**
   * @brief Returns the number of explicit scheduler shutdown calls.
   * @return Current fixture shutdown count.
   * @throws Nothing.
   */
  int shutdown_count() const noexcept { return shutdown_count_(); }

 private:
  /**
   * @brief Releases the native diagnostic handle when present.
   * @return Nothing.
   * @throws Nothing; platform close failures are intentionally contained.
   */
  void close() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr) {
      FreeLibrary(handle_);
      handle_ = nullptr;
    }
#else
    if (handle_ != nullptr) {
      dlclose(handle_);
      handle_ = nullptr;
    }
#endif
  }

#if defined(_WIN32)
  /** @brief Native Windows dynamic-library handle. */
  HMODULE handle_ = nullptr;
#else
  /** @brief Native POSIX dynamic-library handle. */
  void* handle_ = nullptr;
#endif
  /** @brief Fixture counter reset export. */
  void (*reset_counts_)() = nullptr;
  /** @brief Fixture shutdown counter export. */
  int (*shutdown_count_)() = nullptr;
};

/**
 * @brief Clears process-global scheduler plugins on every test exit.
 *
 * @throws Nothing.
 * @note Declare this guard before the Host owner. Reverse destruction then
 *       destroys Host graph runtimes first and clears loader mappings second.
 */
class ScopedSchedulerPluginCleanup final {
 public:
  /**
   * @brief Clears stale scheduler plugin state before a fixture test begins.
   * @throws Nothing; cleanup failures are suppressed for assertion safety.
   */
  ScopedSchedulerPluginCleanup() noexcept { clear(); }

  /**
   * @brief Clears scheduler state after later-declared Host destruction.
   * @throws Nothing; cleanup failures are contained.
   */
  ~ScopedSchedulerPluginCleanup() noexcept { clear(); }

 private:
  /**
   * @brief Clears plugin mappings and diagnostics behind a no-throw fence.
   * @return Nothing.
   * @throws Nothing; loader failures are caught and suppressed.
   */
  static void clear() noexcept {
    try {
      SchedulerPluginLoader::instance().clear_plugins();
      SchedulerPluginLoader::instance().clear_errors();
    } catch (...) {
    }
  }

 public:
  /**
   * @brief Prevents duplicate process-global cleanup ownership.
   * @param other Guard that remains responsible for cleanup.
   * @throws Nothing because construction is unavailable.
   */
  ScopedSchedulerPluginCleanup(const ScopedSchedulerPluginCleanup& other) =
      delete;

  /**
   * @brief Prevents replacing process-global cleanup ownership.
   * @param other Guard whose cleanup responsibility remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedSchedulerPluginCleanup& operator=(
      const ScopedSchedulerPluginCleanup& other) = delete;
};

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
/**
 * @brief Delegates scheduling while observing shutdown versus lane-stop order.
 * @throws Whatever the delegated `SerialDebugScheduler` operation throws.
 * @note The test owns this scheduler through `GraphRuntime`. Borrowed atomic
 *       observations outlive runtime destruction and are touched only by the
 *       externally serialized scheduler lifecycle path.
 */
class LaneOrderObservingScheduler final : public IScheduler {
 public:
  /**
   * @brief Binds lifecycle observations to stable test-owned atomics.
   * @param worker_stopped_events Lane-stop count published by the test hook.
   * @param shutdown_called Set true when scheduler shutdown begins.
   * @param shutdown_after_lane_stopped Set from the lane-stop count at
   * shutdown.
   * @throws Nothing.
   */
  LaneOrderObservingScheduler(
      const std::atomic<std::uint64_t>& worker_stopped_events,
      std::atomic<bool>& shutdown_called,
      std::atomic<bool>& shutdown_after_lane_stopped) noexcept
      : worker_stopped_events_(worker_stopped_events),
        shutdown_called_(shutdown_called),
        shutdown_after_lane_stopped_(shutdown_after_lane_stopped) {}

  /** @brief Releases delegated scheduler state. @throws Nothing. */
  ~LaneOrderObservingScheduler() noexcept override = default;

  /** @copydoc SchedulerTaskRuntime::available_devices */
  std::vector<Device> available_devices() const override {
    return delegate_.available_devices();
  }

  /** @copydoc IScheduler::attach */
  void attach(SchedulerHostContext& host) override { delegate_.attach(host); }

  /** @copydoc IScheduler::detach */
  void detach() override { delegate_.detach(); }

  /** @copydoc IScheduler::start */
  void start() override { delegate_.start(); }

  /**
   * @brief Records lane-stop ordering, then delegates scheduler shutdown.
   * @return Nothing.
   * @throws Whatever `SerialDebugScheduler::shutdown` throws.
   * @note Acquire loads observe the executor callback's release publications.
   */
  void shutdown() override {
    bool first_shutdown = false;
    if (shutdown_called_.compare_exchange_strong(first_shutdown, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
      shutdown_after_lane_stopped_.store(
          worker_stopped_events_.load(std::memory_order_acquire) > 0,
          std::memory_order_release);
    }
    delegate_.shutdown();
  }

  /** @copydoc IScheduler::name */
  std::string name() const override { return "lane_order_observer"; }

  /** @copydoc IScheduler::get_stats */
  std::string get_stats() const override { return delegate_.get_stats(); }

  /** @copydoc IScheduler::is_running */
  bool is_running() const override { return delegate_.is_running(); }

  /** @copydoc SchedulerTaskRuntime::submit_initial_task_handles */
  void submit_initial_task_handles(std::vector<TaskHandle>&& handles,
                                   int total_task_count,
                                   SchedulerTaskPriority priority) override {
    delegate_.submit_initial_task_handles(std::move(handles), total_task_count,
                                          priority);
  }

  /** @copydoc SchedulerTaskRuntime::submit_ready_task_handles_from_worker */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority) override {
    delegate_.submit_ready_task_handles_from_worker(std::move(handles),
                                                    priority);
  }

  /** @copydoc SchedulerTaskRuntime::submit_ready_task_any_thread */
  void submit_ready_task_any_thread(
      Task&& task, SchedulerTaskPriority priority,
      std::optional<std::uint64_t> epoch) override {
    delegate_.submit_ready_task_any_thread(std::move(task), priority, epoch);
  }

  /** @copydoc SchedulerTaskRuntime::wait_for_completion */
  void wait_for_completion() override { delegate_.wait_for_completion(); }

  /** @copydoc SchedulerTaskRuntime::set_exception */
  void set_exception(std::exception_ptr error) override {
    delegate_.set_exception(std::move(error));
  }

  /** @copydoc SchedulerTaskRuntime::inc_tasks_to_complete */
  void inc_tasks_to_complete(int delta) override {
    delegate_.inc_tasks_to_complete(delta);
  }

  /** @copydoc SchedulerTaskRuntime::dec_tasks_to_complete */
  void dec_tasks_to_complete() override { delegate_.dec_tasks_to_complete(); }

  /** @copydoc SchedulerTaskRuntime::log_event */
  void log_event(SchedulerTraceAction action, int node_id) override {
    delegate_.log_event(action, node_id);
  }

 private:
  /** @brief Lane-stop counter borrowed from the installed executor hook. */
  const std::atomic<std::uint64_t>& worker_stopped_events_;
  /** @brief Test-owned publication that shutdown has begun. */
  std::atomic<bool>& shutdown_called_;
  /** @brief Test-owned snapshot of ordering at shutdown entry. */
  std::atomic<bool>& shutdown_after_lane_stopped_;
  /** @brief Complete scheduler behavior delegated by this observer. */
  SerialDebugScheduler delegate_;
};
#endif

/**
 * @brief Writes a graph backed by the dynamically loaded lifecycle operation.
 *
 * @param path YAML file path to create.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error if parent directory creation fails.
 * @throws std::ios_base::failure if opening or writing the graph file fails.
 * @note The operation returns debug metadata without requiring image inputs, so
 *       Host inspection can distinguish original and replacement callbacks.
 */
void write_lifecycle_plugin_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out;
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.open(path);
  out << "- id: 1\n"
      << "  name: lifecycle_plugin_node\n"
      << "  type: plugin_lifecycle\n"
      << "  subtype: op\n";
}

/**
 * @brief Cleans process-global operation plugins through one public Host.
 *
 * Construction removes stale plugins and converts a non-OK public status into
 * `std::runtime_error`. Destruction retries global cleanup but suppresses all
 * exceptions so assertion unwinding remains visible.
 *
 * @throws std::bad_alloc when construction cannot copy Host status storage.
 * @throws std::runtime_error when initial public cleanup reports failure.
 * @note The referenced Host must outlive this guard. Cleanup deliberately uses
 *       the public global-unload surface exercised by the test; every Host sees
 *       the same process-owner state.
 */
class ScopedHostPluginCleanup final {
 public:
  /**
   * @brief Removes stale process plugins before a multi-Host scenario.
   *
   * @param host Long-lived Host used for cleanup.
   * @throws std::bad_alloc if the Host boundary cannot construct a status.
   * @throws std::runtime_error if the public cleanup status is not OK; the
   *         exception copies that status message for test diagnostics.
   * @note The borrowed Host is retained by reference and must remain alive
   * until this guard's destructor has finished.
   */
  explicit ScopedHostPluginCleanup(Host& host) : host_(host) {
    const auto cleanup = host_.plugins_unload_all();
    if (!cleanup.status.ok) {
      throw std::runtime_error(cleanup.status.message);
    }
  }

  /**
   * @brief Performs best-effort public global cleanup after assertions.
   *
   * @throws Nothing; Host exceptions are caught and suppressed.
   * @note Cleanup runs while `host_` is still alive and does not replace an
   *       exception already unwinding from the test body.
   */
  ~ScopedHostPluginCleanup() noexcept {
    try {
      (void)host_.plugins_unload_all();
    } catch (...) {
      // Test teardown must not hide the assertion that triggered unwinding.
    }
  }

  /**
   * @brief Prevents duplicating cleanup ownership for one borrowed Host.
   *
   * @param other Guard that remains the sole cleanup owner.
   * @note Deletion prevents two destructors from racing global cleanup.
   */
  ScopedHostPluginCleanup(const ScopedHostPluginCleanup& other) = delete;

  /**
   * @brief Prevents retargeting an active cleanup guard.
   *
   * @param other Guard whose borrowed Host must remain unchanged.
   * @return No value because this operation is deleted.
   * @note Lexical lifetime remains paired with the Host supplied at
   *       construction.
   */
  ScopedHostPluginCleanup& operator=(const ScopedHostPluginCleanup& other) =
      delete;

  /**
   * @brief Prevents transferring cleanup ownership away from its lexical Host.
   *
   * @param other Guard that remains paired with its borrowed Host.
   * @note Deletion keeps one deterministic destructor cleanup point.
   */
  ScopedHostPluginCleanup(ScopedHostPluginCleanup&& other) = delete;

  /**
   * @brief Prevents replacing cleanup ownership through move assignment.
   *
   * @param other Guard whose borrowed Host remains unchanged.
   * @return No value because this operation is deleted.
   * @note Neither guard can become responsible for a different Host.
   */
  ScopedHostPluginCleanup& operator=(ScopedHostPluginCleanup&& other) = delete;

 private:
  /**
   * @brief Public Host borrowed for initial and final process-global cleanup.
   * @note The surrounding test owns the Host and keeps it alive past this
   * guard.
   */
  Host& host_;
};

/**
 * @brief Returns YAML text for replacing the single Host test node.
 *
 * @param name Replacement node name.
 * @param width Replacement width parameter.
 * @param height Replacement height parameter.
 * @return YAML text accepted by set_node_yaml().
 * @throws std::bad_alloc if string construction allocates and fails.
 * @note The backend preserves the target node id supplied separately.
 */
std::string replacement_node_yaml(const std::string& name, int width,
                                  int height) {
  std::ostringstream out;
  out << R"YAML(
id: 1
name: )YAML"
      << name << R"YAML(
type: host_adapter_test
subtype: source
parameters:
  width: )YAML"
      << width << R"YAML(
  height: )YAML"
      << height << "\n";
  return out.str();
}

/**
 * @brief Loads a deterministic Host adapter graph.
 *
 * @param host Host under test.
 * @param root Temporary root containing source and session folders.
 * @param session Session label to load.
 * @param subtype Operation subtype to write into the graph YAML.
 * @param slow_sleep_ms Milliseconds used by the slow_source fixture op.
 * @return Loaded session id.
 * @throws std::bad_alloc if path or diagnostic strings allocate and fail.
 * @note Test assertions fail immediately if loading is rejected.
 */
GraphSessionId load_test_graph(Host& host, const std::filesystem::path& root,
                               const std::string& session,
                               const std::string& subtype = "source",
                               int slow_sleep_ms = 75) {
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = (root / "source" / (session + ".yaml")).string();
  request.cache_root_dir = (root / "cache").string();
  write_host_adapter_graph(request.yaml_path, 6, 4, subtype, slow_sleep_ms);
  auto loaded = host.load_graph(request);
  EXPECT_TRUE(loaded.status.ok) << loaded.status.message;
  EXPECT_EQ(loaded.value.value, session);
  return loaded.value;
}

/**
 * @brief Builds the graph load request used by Host adapter tests.
 *
 * @param root Temporary root containing source, sessions, and cache folders.
 * @return GraphLoadRequest pointing at a deterministic single-node graph.
 * @throws std::bad_alloc if path string conversion allocates and fails.
 * @note The request exercises the embedded adapter's copy/load path by
 *       providing an explicit YAML source file.
 */
GraphLoadRequest make_load_request(const std::filesystem::path& root) {
  const auto yaml_path = root / "source" / "host_graph.yaml";
  write_host_adapter_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"host_adapter_graph"};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (root / "cache").string();
  return request;
}

/**
 * @brief Builds a compute request for the Host adapter test graph.
 *
 * @param session Session to compute.
 * @return HostComputeRequest for node 1 with timing enabled.
 * @throws std::bad_alloc if precision string allocation fails.
 */
HostComputeRequest make_compute_request(const GraphSessionId& session) {
  HostComputeRequest request;
  request.session = session;
  request.node = NodeId{1};
  request.cache.precision = "fp32";
  request.telemetry.enable_timing = true;
  return request;
}

/**
 * @brief Owns one promise that can be fulfilled idempotently without throwing.
 * @throws std::future_error if future() is requested more than once.
 * @note signal() contains duplicate-fulfillment and broken-state failures so it
 *       is safe for noexcept race cleanup.
 */
class OneShotSignal final {
 public:
  /** @brief Creates one unsignalled promise. @throws Nothing. */
  OneShotSignal() = default;

  /**
   * @brief Returns the shared future observed by a blocking test callback.
   * @return Shared future made ready by signal().
   * @throws std::future_error if the future was already retrieved.
   * @throws std::bad_alloc if shared-state conversion allocates.
   */
  std::shared_future<void> future() { return promise_.get_future().share(); }

  /**
   * @brief Fulfils the promise at most once.
   * @return Nothing.
   * @throws Nothing; promise failures are contained for cleanup safety.
   */
  void signal() noexcept {
    if (signalled_) {
      return;
    }
    signalled_ = true;
    try {
      promise_.set_value();
    } catch (...) {
    }
  }

  /**
   * @brief Copying would duplicate one promise owner and is disabled.
   * @param other Signal retaining the promise.
   * @throws Nothing because construction is unavailable.
   */
  OneShotSignal(const OneShotSignal& other) = delete;

  /**
   * @brief Copy assignment would replace one promise owner and is disabled.
   * @param other Signal retaining the promise.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  OneShotSignal& operator=(const OneShotSignal& other) = delete;

 private:
  /** @brief Promise whose shared state releases one race phase. */
  std::promise<void> promise_;
  /** @brief Whether signal() already attempted promise fulfilment. */
  bool signalled_ = false;
};

/**
 * @brief Releases and closes a partially constructed Host submission storm.
 * @throws Nothing from destruction; backend/status failures are contained.
 * @note The scope is declared after the installed lane hook. Its destructor
 *       releases the blocking operation, waits any outer producer, closes the
 *       session so Host joins all status workers, and finally resets the
 *       process-global blocking fixture before hook storage can expire.
 */
class HostSubmissionStormScope final {
 public:
  /**
   * @brief Binds cleanup to one loaded Host session and release signal.
   * @param host Host whose session must be closed during cleanup.
   * @param session Loaded session used by every storm submission.
   * @param release Signal that releases the active blocking operation.
   * @throws Nothing.
   */
  HostSubmissionStormScope(Host& host, const GraphSessionId& session,
                           OneShotSignal& release) noexcept
      : host_(host), session_(session), release_(release) {}

  /**
   * @brief Releases work and performs best-effort session/fixture cleanup.
   * @throws Nothing; cleanup failures are contained for assertion safety.
   */
  ~HostSubmissionStormScope() noexcept {
    release_.signal();
    try {
      if (blocked_submission.valid()) {
        blocked_submission.wait();
      }
    } catch (...) {
    }
    try {
      (void)host_.close_graph(session_);
    } catch (...) {
    }
    try {
      reset_host_blocking_source();
    } catch (...) {
    }
  }

  /**
   * @brief Prevents duplicate storm cleanup ownership.
   * @param other Scope retaining cleanup responsibility.
   * @throws Nothing because construction is unavailable.
   */
  HostSubmissionStormScope(const HostSubmissionStormScope& other) = delete;

  /**
   * @brief Prevents replacing cleanup ownership for a running storm.
   * @param other Scope whose Host/session binding remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  HostSubmissionStormScope& operator=(const HostSubmissionStormScope& other) =
      delete;

  /** @brief Accepted caller-visible status futures retained through cleanup. */
  std::vector<std::future<OperationStatus>> accepted;

  /** @brief Producer blocked while the graph-state queue is full. */
  std::future<Result<std::future<OperationStatus>>> blocked_submission;

 private:
  /** @brief Host owning the loaded session and async tracking table. */
  Host& host_;
  /** @brief Loaded session to close after all producers can finish. */
  GraphSessionId session_;
  /** @brief Idempotent release for the active blocking operation. */
  OneShotSignal& release_;
};

/**
 * @brief Waits for one valid asynchronous test future during noexcept cleanup.
 * @tparam Result Value carried by the future.
 * @param future Future to wait when it owns shared state.
 * @return Nothing.
 * @throws Nothing; invalid-state and wait failures are contained.
 */
template <typename Result>
void wait_for_race_future_noexcept(std::future<Result>& future) noexcept {
  try {
    if (future.valid()) {
      future.wait();
    }
  } catch (...) {
  }
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING) && \
    defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
/**
 * @brief Owns every future and release signal in an admission-close race.
 *
 * @tparam OperationResult Result returned by the admitted Host operation.
 * @throws Nothing for construction and destruction.
 * @note This scope is declared after borrowed hook guards. Its noexcept
 *       destructor therefore releases every phase, waits all valid async work,
 *       and resets blocking-source state before hook storage is destroyed.
 */
template <typename OperationResult>
class AdmittedOperationRaceScope final {
 public:
  /**
   * @brief Binds cleanup to all signals controlling the race.
   * @param compute_release Releases the blocking compute.
   * @param before_kernel_ready_release Releases the first operation gate.
   * @param before_kernel_snapshot_release Releases the pre-Kernel snapshot.
   * @param after_translation_release Releases the post-translation snapshot.
   * @throws Nothing.
   */
  AdmittedOperationRaceScope(OneShotSignal& compute_release,
                             OneShotSignal& before_kernel_ready_release,
                             OneShotSignal& before_kernel_snapshot_release,
                             OneShotSignal& after_translation_release) noexcept
      : compute_release_(compute_release),
        before_kernel_ready_release_(before_kernel_ready_release),
        before_kernel_snapshot_release_(before_kernel_snapshot_release),
        after_translation_release_(after_translation_release) {}

  /**
   * @brief Releases, joins, and resets all partially constructed race state.
   * @throws Nothing; signal, wait, and reset failures are contained.
   */
  ~AdmittedOperationRaceScope() noexcept {
    compute_release_.signal();
    before_kernel_ready_release_.signal();
    before_kernel_snapshot_release_.signal();
    after_translation_release_.signal();
    wait_for_race_future_noexcept(compute_future);
    wait_for_race_future_noexcept(operation_future);
    wait_for_race_future_noexcept(close_future);
    if (blocking_source_configured_) {
      try {
        reset_host_blocking_source();
      } catch (...) {
      }
    }
  }

  /**
   * @brief Records that global blocking-source state now requires cleanup.
   * @return Nothing.
   * @throws Nothing.
   */
  void mark_blocking_source_configured() noexcept {
    blocking_source_configured_ = true;
  }

  /** @brief Future for the real blocking compute. */
  std::future<VoidResult> compute_future;
  /** @brief Future for the admitted operation under test. */
  std::future<OperationResult> operation_future;
  /** @brief Future for close waiting on lifecycle admission. */
  std::future<VoidResult> close_future;

 private:
  /** @brief Idempotent release for the blocking compute. */
  OneShotSignal& compute_release_;
  /** @brief Idempotent release for the first operation checkpoint. */
  OneShotSignal& before_kernel_ready_release_;
  /** @brief Idempotent release for the pre-Kernel admission snapshot. */
  OneShotSignal& before_kernel_snapshot_release_;
  /** @brief Idempotent release for the post-translation snapshot. */
  OneShotSignal& after_translation_release_;
  /** @brief Whether destructor must reset global blocking-source state. */
  bool blocking_source_configured_ = false;
};

/**
 * @brief Captures the three completed calls in an admission-versus-close race.
 *
 * @tparam OperationResult Public Host result returned by the admitted call.
 * @throws Nothing directly; member construction follows each result type.
 */
template <typename OperationResult>
struct AdmittedOperationCloseRaceResult {
  /** @brief Result from the blocking compute that owns graph-state execution.
   */
  VoidResult blocker;
  /** @brief Result from the synchronous operation admitted before close. */
  OperationResult operation;
  /** @brief Optional result from repeating after close claims its marker. */
  std::optional<OperationResult> rejected_while_closing;
  /** @brief Result from close after the admitted operation finishes. */
  VoidResult close;
};

/**
 * @brief Runs one deterministic admitted-operation-versus-close race.
 *
 * The helper first holds GraphStateExecutor with a real blocking compute. The
 * target operation then stops after admission but before Kernel entry. After
 * close claims its marker, the helper releases and joins the blocker, proves
 * the target admission remains active with GSE idle, lets Kernel and public
 * result construction finish, and proves the same admission remains active at
 * a second GSE-idle checkpoint before Host return.
 *
 * @tparam OperationCall Nullary callable invoking one synchronous Host API.
 * @param host Host owning the loaded session.
 * @param session Session shared by compute, operation, and close.
 * @param event Target Host operation selected by the three-phase gate.
 * @param operation_name Stable API name used in deterministic failure text.
 * @param operation_call Callable whose result is retained for test assertions.
 * @param repeat_after_close_marker Whether to invoke the same public operation
 *        after close has claimed its marker and retain the rejected result.
 * @return Results from the blocker, admitted operation, optional post-marker
 *         rejection, and close.
 * @throws std::runtime_error if a required event is absent or a supposedly
 *         blocked call completes before release.
 * @throws Any exception propagated by asynchronous Host calls or allocation.
 * @note A noexcept race scope releases every phase, joins all valid futures,
 *       and resets blocking-source state on all exits, including partial
 *       std::async construction. When repeat_after_close_marker is true, the
 *       callable must permit one concurrent invocation while the admitted
 *       invocation remains stopped at the pre-Kernel gate.
 */
template <typename OperationCall>
AdmittedOperationCloseRaceResult<std::invoke_result_t<OperationCall&>>
run_admitted_operation_close_race(Host& host, const GraphSessionId& session,
                                  EmbeddedOperationTestEvent event,
                                  const char* operation_name,
                                  OperationCall operation_call,
                                  bool repeat_after_close_marker = false) {
  using OperationResult = std::invoke_result_t<OperationCall&>;

  OneShotSignal compute_release;
  OneShotSignal before_kernel_ready_release;
  OneShotSignal before_kernel_snapshot_release;
  OneShotSignal after_translation_release;
  EmbeddedOperationGateState operation_gate{
      event, before_kernel_ready_release.future(),
      before_kernel_snapshot_release.future(),
      after_translation_release.future()};
  EmbeddedLifecycleEventState lifecycle_events;
  ScopedEmbeddedLifecycleTestHook lifecycle_hook(lifecycle_events);
  ScopedEmbeddedOperationTestHook operation_hook(operation_gate);
  AdmittedOperationRaceScope<OperationResult> race_scope(
      compute_release, before_kernel_ready_release,
      before_kernel_snapshot_release, after_translation_release);

  race_scope.mark_blocking_source_configured();
  configure_host_blocking_source(compute_release.future());
  HostComputeRequest request = make_compute_request(session);
  request.cache.force_recache = true;
  race_scope.compute_future = std::async(
      std::launch::async, [&host, request] { return host.compute(request); });

  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    throw std::runtime_error(
        "blocking compute did not acquire graph-state execution");
  }

  race_scope.operation_future = std::async(
      std::launch::async, [&operation_call] { return operation_call(); });
  if (!wait_for_atomic_event_count(lifecycle_events.session_operation_admitted,
                                   2, std::chrono::seconds(2))) {
    throw std::runtime_error(std::string(operation_name) +
                             " did not publish synchronous admission");
  }
  if (!wait_for_atomic_event_count(operation_gate.before_kernel_ready_reached,
                                   1, std::chrono::seconds(2))) {
    throw std::runtime_error(std::string(operation_name) +
                             " did not reach its pre-Kernel gate");
  }

  race_scope.close_future = std::async(std::launch::async, [&host, &session] {
    return host.close_graph(session);
  });
  if (!wait_for_atomic_event_count(lifecycle_events.marker_claimed, 1,
                                   std::chrono::seconds(2))) {
    throw std::runtime_error(
        "close did not claim marker while admitted operation was pending");
  }

  std::optional<OperationResult> rejected_while_closing;
  if (repeat_after_close_marker) {
    rejected_while_closing.emplace(operation_call());
  }

  if (race_scope.operation_future.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::timeout ||
      race_scope.close_future.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::timeout) {
    throw std::runtime_error(
        "admitted operation or close completed before blocker release");
  }

  compute_release.signal();
  VoidResult blocker = race_scope.compute_future.get();
  before_kernel_ready_release.signal();
  if (!wait_for_atomic_event_count(
          operation_gate.before_kernel_snapshot_reached, 1,
          std::chrono::seconds(2))) {
    throw std::runtime_error(std::string(operation_name) +
                             " did not publish its pre-Kernel admission");
  }
  if (!operation_gate.before_kernel_admission_active.load(
          std::memory_order_acquire)) {
    throw std::runtime_error(std::string(operation_name) +
                             " released admission before Kernel entry");
  }
  if (race_scope.operation_future.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::timeout ||
      race_scope.close_future.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::timeout) {
    throw std::runtime_error(
        "operation or close completed at the pre-Kernel admission gate");
  }

  before_kernel_snapshot_release.signal();
  if (!wait_for_atomic_event_count(
          operation_gate.after_translation_snapshot_reached, 1,
          std::chrono::seconds(2))) {
    throw std::runtime_error(std::string(operation_name) +
                             " did not reach its post-translation gate");
  }
  if (!operation_gate.after_translation_admission_active.load(
          std::memory_order_acquire)) {
    throw std::runtime_error(std::string(operation_name) +
                             " released admission before Host return");
  }
  if (race_scope.operation_future.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::timeout ||
      race_scope.close_future.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::timeout) {
    throw std::runtime_error(
        "operation or close completed at the post-translation admission gate");
  }

  after_translation_release.signal();
  OperationResult operation = race_scope.operation_future.get();
  VoidResult close = race_scope.close_future.get();
  return {std::move(blocker), std::move(operation),
          std::move(rejected_while_closing), std::move(close)};
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING) &&      \
    defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING) && \
    defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Owns both futures and the gate release in a required-target race.
 *
 * @tparam TargetResult Result returned by the lookup-and-use operation.
 * @tparam CompetitorResult Result returned by clear or reload.
 * @throws Nothing for construction and destruction.
 * @note Declaring this scope after both hook guards makes its destructor
 * release and join all work before borrowed hook storage is cleared.
 */
template <typename TargetResult, typename CompetitorResult>
class RequiredTargetRaceScope final {
 public:
  /**
   * @brief Binds cleanup to the required-target release signal.
   * @param target_release Signal that releases the target checkpoint.
   * @throws Nothing.
   */
  explicit RequiredTargetRaceScope(OneShotSignal& target_release) noexcept
      : target_release_(target_release) {}

  /**
   * @brief Releases and joins every partially constructed race future.
   * @throws Nothing; signal and wait failures are contained.
   */
  ~RequiredTargetRaceScope() noexcept {
    target_release_.signal();
    wait_for_race_future_noexcept(target_future);
    wait_for_race_future_noexcept(competitor_future);
  }

  /** @brief Future for the required-target operation. */
  std::future<TargetResult> target_future;
  /** @brief Future for clear or reload. */
  std::future<CompetitorResult> competitor_future;

 private:
  /** @brief Idempotent release for the required-target checkpoint. */
  OneShotSignal& target_release_;
};

/**
 * @brief Captures both calls in a required-target serialization race.
 *
 * @tparam TargetResult Result returned by the lookup-and-use operation.
 * @tparam CompetitorResult Result returned by clear or reload.
 * @throws Nothing directly; member construction follows each result type.
 */
template <typename TargetResult, typename CompetitorResult>
struct RequiredTargetRaceResult {
  /** @brief Lookup-and-use result produced before graph replacement. */
  TargetResult target;
  /** @brief Clear or reload result produced after the target call. */
  CompetitorResult competitor;
};

/**
 * @brief Proves required-target use excludes one clear or reload operation.
 *
 * The target call is held immediately after required-node resolution while it
 * still owns GraphStateExecutor. The competing call must enter the real FIFO
 * queue and remain pending until the target is released.
 *
 * @tparam TargetCall Nullary callable invoking set-node or ROI projection.
 * @tparam CompetitorCall Nullary callable invoking clear or reload.
 * @param event Required-target checkpoint expected from the target call.
 * @param target_name Stable target name used in deterministic failure text.
 * @param competitor_name Stable competitor name used in failure text.
 * @param target_call Target callable retained until its future completes.
 * @param competitor_call Competing callable started after target resolution.
 * @return Results in target-before-competitor completion order.
 * @throws std::runtime_error if either observation is absent or the competitor
 *         completes before target release.
 * @throws Any exception propagated by asynchronous calls or allocation.
 * @note A noexcept race scope releases the target gate and joins every valid
 *       future on all exits, including partial std::async construction.
 */
template <typename TargetCall, typename CompetitorCall>
auto run_required_target_race(testing::RequiredTargetTestEvent event,
                              const char* target_name,
                              const char* competitor_name,
                              TargetCall target_call,
                              CompetitorCall competitor_call) {
  using TargetResult = std::invoke_result_t<TargetCall&>;
  using CompetitorResult = std::invoke_result_t<CompetitorCall&>;

  OneShotSignal target_release;
  RequiredTargetGateState gate{event, target_release.future()};
  ScopedRequiredTargetTestHook target_hook(gate);
  GraphStateExecutorLaneState lane_state;
  ScopedGraphStateExecutorTestHook lane_hook(lane_state);
  RequiredTargetRaceScope<TargetResult, CompetitorResult> race_scope(
      target_release);

  race_scope.target_future = std::async(
      std::launch::async, [target_call = std::move(target_call)]() mutable {
        return target_call();
      });
  if (!wait_for_atomic_event_count(gate.reached, 1, std::chrono::seconds(2))) {
    throw std::runtime_error(std::string(target_name) +
                             " did not reach its required-target checkpoint");
  }
  lane_state.queued_events.store(0, std::memory_order_release);

  race_scope.competitor_future =
      std::async(std::launch::async,
                 [competitor_call = std::move(competitor_call)]() mutable {
                   return competitor_call();
                 });
  if (!wait_for_atomic_event_count(lane_state.queued_events, 1,
                                   std::chrono::seconds(2))) {
    throw std::runtime_error(std::string(competitor_name) +
                             " did not queue on GraphStateExecutor");
  }

  const bool competitor_pending =
      race_scope.competitor_future.wait_for(std::chrono::milliseconds(0)) ==
      std::future_status::timeout;
  target_release.signal();
  TargetResult target = race_scope.target_future.get();
  CompetitorResult competitor = race_scope.competitor_future.get();
  if (!competitor_pending) {
    throw std::runtime_error(std::string(competitor_name) +
                             " completed before required-target release");
  }
  return RequiredTargetRaceResult<TargetResult, CompetitorResult>{
      std::move(target), std::move(competitor)};
}
#endif

/**
 * @brief Reports whether a string vector contains a value.
 *
 * @param values Values to search.
 * @param needle String to find.
 * @return True when needle is present.
 * @throws Nothing directly.
 */
bool contains_string(const std::vector<std::string>& values,
                     const std::string& needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

/**
 * @brief Proves admitted graph-state work executes FIFO on one owned worker.
 * @return Nothing; GoogleTest assertions report ordering or worker-identity
 *         failures.
 * @throws std::bad_alloc, std::system_error, or std::future_error if fixture,
 *         executor, or synchronization setup fails.
 * @note The first task blocks before later submissions so the test observes a
 *       real backlog rather than sequentially submitting already-finished work.
 */
TEST(GraphStateExecutorLane, ExecutesFifoOnOneWorker) {
  ScopedTempDir temp("photospider_graph_state_fifo_test");
  GraphModel model(temp.root() / "cache");
  GraphStateExecutor executor(model);

  OneShotSignal release_first;
  const std::shared_future<void> first_release = release_first.future();
  std::promise<void> first_started;
  std::future<void> first_started_future = first_started.get_future();
  std::mutex observations_mutex;
  std::vector<int> completion_order;
  std::set<std::thread::id> worker_ids;

  auto first = executor.submit([&](GraphModel&) {
    first_started.set_value();
    first_release.wait();
    std::lock_guard<std::mutex> lock(observations_mutex);
    completion_order.push_back(0);
    worker_ids.insert(std::this_thread::get_id());
    return 0;
  });
  ASSERT_EQ(first_started_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  std::vector<std::future<int>> later;
  constexpr int kLaterTaskCount = 8;
  later.reserve(kLaterTaskCount);
  for (int index = 1; index <= kLaterTaskCount; ++index) {
    later.push_back(executor.submit([&, index](GraphModel&) {
      std::lock_guard<std::mutex> lock(observations_mutex);
      completion_order.push_back(index);
      worker_ids.insert(std::this_thread::get_id());
      return index;
    }));
  }

  release_first.signal();
  EXPECT_EQ(first.get(), 0);
  for (int index = 1; index <= kLaterTaskCount; ++index) {
    EXPECT_EQ(later[static_cast<std::size_t>(index - 1)].get(), index);
  }

  ASSERT_EQ(completion_order.size(),
            static_cast<std::size_t>(kLaterTaskCount + 1));
  for (int index = 0; index <= kLaterTaskCount; ++index) {
    EXPECT_EQ(completion_order[static_cast<std::size_t>(index)], index);
  }
  EXPECT_EQ(worker_ids.size(), 1u);
}

/**
 * @brief Proves packaged futures preserve reference, void, and exception
 *        results without retiring the lane worker after a callable failure.
 * @return Nothing; GoogleTest assertions report result or recovery failures.
 * @throws std::bad_alloc, std::system_error, or std::future_error if fixture,
 *         executor, or synchronization setup fails.
 * @note The task submitted after the throwing callable must complete on the
 *       same still-accepting lane.
 */
TEST(GraphStateExecutorLane, PreservesFutureResultsAndSurvivesExceptions) {
  ScopedTempDir temp("photospider_graph_state_future_fidelity_test");
  GraphModel model(temp.root() / "cache");
  GraphStateExecutor executor(model);
  int referenced_value = 41;

  auto reference =
      executor.submit([&](GraphModel&) -> int& { return referenced_value; });
  int& returned_reference = reference.get();
  EXPECT_EQ(&returned_reference, &referenced_value);

  auto completed = executor.submit([&](GraphModel&) { ++referenced_value; });
  completed.get();
  EXPECT_EQ(referenced_value, 42);

  auto failed = executor.submit([](GraphModel&) -> int {
    throw std::runtime_error("graph-state future fidelity");
  });
  try {
    (void)failed.get();
    FAIL() << "throwing graph-state task returned a value";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "graph-state future fidelity");
  }

  auto later = executor.submit([](GraphModel&) { return 43; });
  EXPECT_EQ(later.get(), 43);
}

/**
 * @brief Proves a full graph-state queue blocks admission until one slot frees.
 * @return Nothing; GoogleTest assertions report premature admission, result,
 *         or FIFO failures.
 * @throws std::bad_alloc, std::system_error, or std::future_error if fixture,
 *         executor, or synchronization setup fails.
 * @note Capacity is injected as two waiting tasks. One active task holds the
 *       worker while a fourth producer demonstrates blocking backpressure.
 */
TEST(GraphStateExecutorLane, FullQueueBlocksUntilAQueuedSlotIsAvailable) {
  ScopedTempDir temp("photospider_graph_state_backpressure_test");
  GraphModel model(temp.root() / "cache");
  GraphStateExecutor executor(model, 2);

  OneShotSignal release_first;
  const std::shared_future<void> first_release = release_first.future();
  std::promise<void> first_started;
  std::future<void> first_started_future = first_started.get_future();
  std::mutex order_mutex;
  std::vector<int> completion_order;

  auto first = executor.submit([&](GraphModel&) {
    first_started.set_value();
    first_release.wait();
    std::lock_guard<std::mutex> lock(order_mutex);
    completion_order.push_back(0);
    return 0;
  });
  ASSERT_EQ(first_started_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  auto second = executor.submit([&](GraphModel&) {
    std::lock_guard<std::mutex> lock(order_mutex);
    completion_order.push_back(1);
    return 1;
  });
  auto third = executor.submit([&](GraphModel&) {
    std::lock_guard<std::mutex> lock(order_mutex);
    completion_order.push_back(2);
    return 2;
  });

  std::promise<void> fourth_entered;
  std::future<void> fourth_entered_future = fourth_entered.get_future();
  auto fourth_submission = std::async(std::launch::async, [&] {
    fourth_entered.set_value();
    return executor.submit([&](GraphModel&) {
      std::lock_guard<std::mutex> lock(order_mutex);
      completion_order.push_back(3);
      return 3;
    });
  });
  ASSERT_EQ(fourth_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(fourth_submission.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_first.signal();
  std::future<int> fourth = fourth_submission.get();
  EXPECT_EQ(first.get(), 0);
  EXPECT_EQ(second.get(), 1);
  EXPECT_EQ(third.get(), 2);
  EXPECT_EQ(fourth.get(), 3);
  EXPECT_EQ(completion_order, (std::vector<int>{0, 1, 2, 3}));
}

/**
 * @brief Proves close rejects blocked/new producers and drains admitted work.
 * @return Nothing; GoogleTest assertions report admission or drain-order
 *         failures.
 * @throws std::bad_alloc, std::system_error, or std::future_error if fixture,
 *         executor, or synchronization setup fails.
 * @note A one-slot queue makes the third producer block deterministically while
 *       close begins on another thread and the first task remains active.
 */
TEST(GraphStateExecutorLane, CloseWakesBlockedProducerAndDrainsAdmittedWork) {
  ScopedTempDir temp("photospider_graph_state_close_drain_test");
  GraphModel model(temp.root() / "cache");
  GraphStateExecutor executor(model, 1);

  OneShotSignal release_first;
  const std::shared_future<void> first_release = release_first.future();
  std::promise<void> first_started;
  std::future<void> first_started_future = first_started.get_future();
  std::mutex order_mutex;
  std::vector<int> completion_order;

  auto first = executor.submit([&](GraphModel&) {
    first_started.set_value();
    first_release.wait();
    std::lock_guard<std::mutex> lock(order_mutex);
    completion_order.push_back(0);
    return 0;
  });
  ASSERT_EQ(first_started_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  auto second = executor.submit([&](GraphModel&) {
    std::lock_guard<std::mutex> lock(order_mutex);
    completion_order.push_back(1);
    return 1;
  });

  std::promise<void> third_entered;
  std::future<void> third_entered_future = third_entered.get_future();
  auto third_submission = std::async(std::launch::async, [&] {
    third_entered.set_value();
    return executor.submit([](GraphModel&) { return 2; });
  });
  ASSERT_EQ(third_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(third_submission.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  std::promise<void> close_entered;
  std::future<void> close_entered_future = close_entered.get_future();
  auto close = std::async(std::launch::async, [&] {
    close_entered.set_value();
    executor.close_and_drain();
  });
  ASSERT_EQ(close_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(third_submission.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_THROW((void)third_submission.get(), std::runtime_error);
  EXPECT_EQ(close.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  release_first.signal();
  EXPECT_EQ(first.get(), 0);
  EXPECT_EQ(second.get(), 1);
  close.get();
  EXPECT_EQ(completion_order, (std::vector<int>{0, 1}));
  EXPECT_THROW((void)executor.submit([](GraphModel&) { return 3; }),
               std::runtime_error);
}

/**
 * @brief Proves a lane task cannot recursively submit to its own worker.
 * @return Nothing; GoogleTest assertions report missing fail-fast behavior.
 * @throws std::bad_alloc or std::system_error if fixture or worker setup fails.
 * @note The outer task does not wait on a nested future; the expected
 *       `std::logic_error` must therefore come from explicit re-entry
 * detection, not from a queue-full side effect.
 */
TEST(GraphStateExecutorLane, WorkerSubmissionReentryFailsBeforeWaiting) {
  ScopedTempDir temp("photospider_graph_state_submit_reentry_test");
  GraphModel model(temp.root() / "cache");
  GraphStateExecutor executor(model);

  auto outer = executor.submit([&](GraphModel&) {
    try {
      (void)executor.submit([](GraphModel&) { return 99; });
    } catch (const std::logic_error&) {
      return true;
    }
    return false;
  });

  EXPECT_TRUE(outer.get());
}

/**
 * @brief Proves a lane task cannot close and join its own worker thread.
 * @return Nothing; GoogleTest assertions report missing fail-fast behavior or
 *         accidental admission closure.
 * @throws std::bad_alloc or std::system_error if fixture or worker setup fails.
 * @note A later task must still run, proving the rejected nested close did not
 *       mutate the accepting lifecycle state.
 */
TEST(GraphStateExecutorLane, WorkerCloseReentryFailsWithoutClosingAdmission) {
  ScopedTempDir temp("photospider_graph_state_close_reentry_test");
  GraphModel model(temp.root() / "cache");
  GraphStateExecutor executor(model);

  auto outer = executor.submit([&](GraphModel&) {
    try {
      executor.close_and_drain();
    } catch (const std::logic_error&) {
      return true;
    }
    return false;
  });
  EXPECT_TRUE(outer.get());

  auto later = executor.submit([](GraphModel&) { return 7; });
  EXPECT_EQ(later.get(), 7);
}

/**
 * @brief Proves concurrent and repeated close calls share one joined state.
 * @return Nothing; GoogleTest assertions report premature close completion,
 *         join failures, or accidental admission reopening.
 * @throws std::bad_alloc, std::system_error, or std::future_error if fixture,
 *         executor, or synchronization setup fails.
 * @note Three callers enter close while one task owns the worker. Every close
 *       must remain pending until the same task is released and worker joined.
 */
TEST(GraphStateExecutorLane, ConcurrentAndRepeatedCloseShareOneJoin) {
  ScopedTempDir temp("photospider_graph_state_concurrent_close_test");
  GraphModel model(temp.root() / "cache");
  GraphStateExecutor executor(model);

  OneShotSignal release_task;
  const std::shared_future<void> task_release = release_task.future();
  std::promise<void> task_started;
  std::future<void> task_started_future = task_started.get_future();
  auto task = executor.submit([&](GraphModel&) {
    task_started.set_value();
    task_release.wait();
  });
  ASSERT_EQ(task_started_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  constexpr std::size_t kCloserCount = 3;
  std::vector<std::promise<void>> closer_entered(kCloserCount);
  std::vector<std::future<void>> closer_entered_futures;
  std::vector<std::future<void>> closers;
  closer_entered_futures.reserve(kCloserCount);
  closers.reserve(kCloserCount);
  for (std::size_t index = 0; index < kCloserCount; ++index) {
    closer_entered_futures.push_back(closer_entered[index].get_future());
    closers.push_back(std::async(std::launch::async, [&, index] {
      closer_entered[index].set_value();
      executor.close_and_drain();
    }));
  }
  for (auto& entered : closer_entered_futures) {
    ASSERT_EQ(entered.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
  }
  for (auto& closer : closers) {
    EXPECT_EQ(closer.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);
  }

  release_task.signal();
  task.get();
  for (auto& closer : closers) {
    closer.get();
  }

  executor.close_and_drain();
  executor.close_and_drain();
  EXPECT_THROW((void)executor.submit([](GraphModel&) {}), std::runtime_error);
}

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING) && \
    defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Proves close waiters finish their own joined generation after restart.
 * @return Nothing; GoogleTest assertions report stuck waiters, restart, worker,
 *         or cleanup failures.
 * @throws std::bad_alloc, std::system_error, or std::future_error if fixture,
 *         executor, task, or close-thread setup fails.
 * @note Sixteen close callers wait behind one active task. The BUILD_TESTING
 *       hook restarts the fully joined lane in the exact unlocked window before
 *       those callers are notified. Every caller must still recognize its
 *       original completed close generation. On regression the test detects
 *       pending callers, closes the replacement generation to release them,
 *       and only then reports failure.
 */
TEST(GraphStateExecutorLane,
     ConcurrentCloseWaitersFinishJoinedGenerationAfterImmediateRestart) {
  ScopedTempDir temp("photospider_graph_state_close_generation_test");
  GraphModel model(temp.root() / "cache");
  GraphStateExecutor executor(model);
  GraphStateExecutorLaneState lane_state;
  ScopedGraphStateExecutorTestHook lane_hook(lane_state);
  ClosePublishRestartState restart_state;
  restart_state.executor = &executor;
  ScopedClosePublishRestartHook restart_hook(restart_state);

  OneShotSignal release_task;
  const std::shared_future<void> task_release = release_task.future();
  std::promise<void> task_started;
  std::future<void> task_started_future = task_started.get_future();
  auto task = executor.submit([&](GraphModel&) {
    task_started.set_value();
    task_release.wait();
  });
  ASSERT_EQ(task_started_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  constexpr std::size_t kCloserCount = 16;
  std::vector<std::future<void>> closers;
  closers.reserve(kCloserCount);
  for (std::size_t index = 0; index < kCloserCount; ++index) {
    closers.push_back(
        std::async(std::launch::async, [&] { executor.close_and_drain(); }));
  }
  const bool all_close_callers_waiting = wait_for_atomic_event_count(
      lane_state.close_wait_events, kCloserCount, std::chrono::seconds(2));

  release_task.signal();
  task.get();
  const bool restart_observed = wait_for_atomic_event_count(
      lane_state.reopened_events, 1, std::chrono::seconds(2));
  const auto close_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool all_close_callers_ready = true;
  for (auto& closer : closers) {
    if (closer.wait_until(close_deadline) != std::future_status::ready) {
      all_close_callers_ready = false;
      break;
    }
  }

  if (!all_close_callers_ready) {
    executor.close_and_drain();
  }
  for (auto& closer : closers) {
    closer.get();
  }
  executor.close_and_drain();

  EXPECT_TRUE(all_close_callers_waiting);
  EXPECT_TRUE(restart_observed);
  EXPECT_TRUE(restart_state.restart_succeeded.load(std::memory_order_acquire));
  EXPECT_FALSE(restart_state.restart_failed.load(std::memory_order_acquire));
  EXPECT_TRUE(all_close_callers_ready);
  EXPECT_LE(lane_state.max_worker_threads.load(std::memory_order_acquire), 1u);
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_GRAPH_STATE_EXECUTOR_TESTING)
/**
 * @brief Proves real Host submission pressure respects graph-state lane bounds.
 * @return Nothing; GoogleTest assertions report capacity, worker, admission, or
 *         status failures.
 * @throws std::bad_alloc, std::system_error, or filesystem exceptions if Host,
 *         graph, worker, or fixture setup fails.
 * @note Caller threads and Host status-mapping workers are separate ownership
 *       domains. This test observes only the worker and queue owned by the real
 *       per-Graph `GraphStateExecutor` reached through the public Host path.
 */
TEST(EmbeddedHostAdapter, SubmissionStormKeepsGraphStateLaneBounded) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_graph_state_storm_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "graph_state_storm", "blocking_source");

  GraphStateExecutorLaneState lane_state;
  ScopedGraphStateExecutorTestHook lane_hook(lane_state);
  OneShotSignal release_active;
  configure_host_blocking_source(release_active.future());
  HostSubmissionStormScope cleanup(*host, session, release_active);

  HostComputeRequest request = make_compute_request(session);
  request.cache.force_recache = true;
  auto active = host->compute_async(request);
  ASSERT_TRUE(active.status.ok) << active.status.message;
  cleanup.accepted.push_back(std::move(active.value));
  ASSERT_TRUE(wait_for_host_blocking_source(std::chrono::seconds(2)));

  cleanup.accepted.reserve(GraphStateExecutor::kDefaultQueueCapacity + 2);
  for (std::size_t index = 0; index < GraphStateExecutor::kDefaultQueueCapacity;
       ++index) {
    auto queued = host->compute_async(request);
    ASSERT_TRUE(queued.status.ok) << queued.status.message;
    cleanup.accepted.push_back(std::move(queued.value));
  }

  EXPECT_EQ(lane_state.queue_capacity.load(std::memory_order_acquire),
            GraphStateExecutor::kDefaultQueueCapacity);
  EXPECT_EQ(lane_state.max_queued_tasks.load(std::memory_order_acquire),
            GraphStateExecutor::kDefaultQueueCapacity);
  EXPECT_EQ(lane_state.max_active_tasks.load(std::memory_order_acquire), 1u);
  EXPECT_EQ(lane_state.max_worker_threads.load(std::memory_order_acquire), 1u);

  std::promise<void> blocked_entered;
  std::future<void> blocked_entered_future = blocked_entered.get_future();
  cleanup.blocked_submission = std::async(std::launch::async, [&] {
    blocked_entered.set_value();
    return host->compute_async(request);
  });
  ASSERT_EQ(blocked_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(cleanup.blocked_submission.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_active.signal();
  auto final_submission = cleanup.blocked_submission.get();
  ASSERT_TRUE(final_submission.status.ok) << final_submission.status.message;
  cleanup.accepted.push_back(std::move(final_submission.value));
  for (auto& status_future : cleanup.accepted) {
    const OperationStatus status = status_future.get();
    EXPECT_TRUE(status.ok) << status.message;
  }

  EXPECT_LE(lane_state.max_queued_tasks.load(std::memory_order_acquire),
            GraphStateExecutor::kDefaultQueueCapacity);
  EXPECT_LE(lane_state.max_active_tasks.load(std::memory_order_acquire), 1u);
  EXPECT_LE(lane_state.max_worker_threads.load(std::memory_order_acquire), 1u);
  const VoidResult closed = host->close_graph(session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
  reset_host_blocking_source();
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Proves close stops full-lane admission before waiting for async state.
 * @return Nothing; GoogleTest assertions report close-marker starvation,
 *         producer rejection, drain, or public-status failures.
 * @throws std::bad_alloc, std::system_error, or filesystem exceptions if Host,
 *         graph, worker, or synchronization setup fails.
 * @note One public async compute holds the real graph-state worker while 64
 *       later computes fill the production FIFO. A sixty-sixth producer then
 *       blocks in admission. Close must claim its Host marker and reject that
 *       blocked producer before the active compute releases queue space. The
 *       test always releases and joins every participant before assertions so
 *       a regression fails without leaving background work behind.
 */
TEST(EmbeddedHostAdapter,
     CloseStopsFullLaneBeforeWaitingForBlockedAsyncPlaceholder) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_full_lane_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "host_full_lane_close", "blocking_source");

  GraphStateExecutorLaneState lane_state;
  ScopedGraphStateExecutorTestHook lane_hook(lane_state);
  EmbeddedLifecycleEventState lifecycle_events;
  ScopedEmbeddedLifecycleTestHook lifecycle_hook(lifecycle_events);
  OneShotSignal release_active;
  configure_host_blocking_source(release_active.future());
  HostSubmissionStormScope cleanup(*host, session, release_active);

  HostComputeRequest request = make_compute_request(session);
  request.cache.force_recache = true;
  auto active = host->compute_async(request);
  ASSERT_TRUE(active.status.ok) << active.status.message;
  cleanup.accepted.push_back(std::move(active.value));
  ASSERT_TRUE(wait_for_host_blocking_source(std::chrono::seconds(2)));

  cleanup.accepted.reserve(GraphStateExecutor::kDefaultQueueCapacity + 1);
  for (std::size_t index = 0; index < GraphStateExecutor::kDefaultQueueCapacity;
       ++index) {
    auto queued = host->compute_async(request);
    ASSERT_TRUE(queued.status.ok) << queued.status.message;
    cleanup.accepted.push_back(std::move(queued.value));
  }
  ASSERT_EQ(lane_state.max_queued_tasks.load(std::memory_order_acquire),
            GraphStateExecutor::kDefaultQueueCapacity);

  std::promise<void> blocked_entered;
  std::future<void> blocked_entered_future = blocked_entered.get_future();
  cleanup.blocked_submission = std::async(std::launch::async, [&] {
    blocked_entered.set_value();
    return host->compute_async(request);
  });
  ASSERT_EQ(blocked_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(cleanup.blocked_submission.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  auto close = std::async(std::launch::async,
                          [&] { return host->close_graph(session); });
  const bool marker_claimed_before_queue_space = wait_for_atomic_event_count(
      lifecycle_events.marker_claimed, 1, std::chrono::seconds(2));
  const bool blocked_submit_ready_before_queue_space =
      cleanup.blocked_submission.wait_for(std::chrono::seconds(2)) ==
      std::future_status::ready;

  release_active.signal();
  auto blocked_result = cleanup.blocked_submission.get();
  if (blocked_result.status.ok) {
    cleanup.accepted.push_back(std::move(blocked_result.value));
  }
  for (auto& status_future : cleanup.accepted) {
    const OperationStatus status = status_future.get();
    EXPECT_TRUE(status.ok) << status.message;
  }
  const VoidResult close_result = close.get();
  reset_host_blocking_source();

  EXPECT_TRUE(marker_claimed_before_queue_space);
  EXPECT_TRUE(blocked_submit_ready_before_queue_space);
  EXPECT_FALSE(blocked_result.status.ok);
  if (!blocked_result.status.ok) {
    EXPECT_EQ(checked_graph_error_code(blocked_result.status),
              GraphErrc::NotFound);
  }
  EXPECT_TRUE(close_result.status.ok) << close_result.status.message;
}
#endif

/**
 * @brief Proves runtime destruction drains dropped-future work before scheduler
 *        teardown.
 * @return Nothing; GoogleTest assertions report premature destruction or
 *         teardown-order failures.
 * @throws std::bad_alloc, std::system_error, or filesystem exceptions if the
 *         runtime, scheduler, task, or synchronization fixture cannot start.
 * @note The graph-state future is intentionally discarded. Only runtime-owned
 *       lane lifecycle can keep the task alive and join it before shutdown.
 */
TEST(GraphStateExecutorLane,
     RuntimeDestructionDrainsDiscardedFutureBeforeSchedulerShutdown) {
  ScopedTempDir temp("photospider_graph_state_runtime_teardown_test");
  GraphStateExecutorLaneState lane_state;
  ScopedGraphStateExecutorTestHook lane_hook(lane_state);
  std::atomic<bool> shutdown_called{false};
  std::atomic<bool> shutdown_after_lane_stopped{false};

  GraphRuntime::Info info;
  info.name = "graph_state_runtime_teardown";
  info.root = temp.root() / "session";
  info.cache_root = temp.root() / "cache";
  auto runtime = std::make_unique<GraphRuntime>(info);
  runtime->set_scheduler(ComputeIntent::GlobalHighPrecision,
                         std::make_unique<LaneOrderObservingScheduler>(
                             lane_state.worker_stopped_events, shutdown_called,
                             shutdown_after_lane_stopped));

  OneShotSignal release_task;
  const std::shared_future<void> task_release = release_task.future();
  std::promise<void> task_started;
  std::future<void> task_started_future = task_started.get_future();
  auto discarded_future = runtime->graph_state().submit([&](GraphModel&) {
    task_started.set_value();
    task_release.wait();
  });
  if (task_started_future.wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    release_task.signal();
    FAIL() << "discarded-future graph-state task did not start";
  }

  auto discard_operation = std::async(
      std::launch::async, [future = std::move(discarded_future)]() mutable {
        future = std::future<void>{};
      });
  if (discard_operation.wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    release_task.signal();
    discard_operation.wait();
    FAIL() << "destroying packaged-task future waited for task completion";
  }
  discard_operation.get();

  auto destruction =
      std::async(std::launch::async, [&runtime] { runtime.reset(); });
  const std::future_status pending =
      destruction.wait_for(std::chrono::milliseconds(100));
  const bool shutdown_seen_before_release =
      shutdown_called.load(std::memory_order_acquire);
  const bool ordered_before_release =
      shutdown_after_lane_stopped.load(std::memory_order_acquire);

  release_task.signal();
  destruction.get();

  EXPECT_EQ(pending, std::future_status::timeout);
  EXPECT_FALSE(shutdown_seen_before_release);
  EXPECT_FALSE(ordered_before_release);
  EXPECT_TRUE(shutdown_called.load(std::memory_order_acquire));
  EXPECT_TRUE(shutdown_after_lane_stopped.load(std::memory_order_acquire));
  EXPECT_GE(lane_state.worker_stopped_events.load(std::memory_order_acquire),
            1u);
}

/**
 * @brief Proves explicit Kernel close joins the lane before scheduler shutdown.
 * @return Nothing; GoogleTest assertions report load, close, or first-shutdown
 *         ordering failures.
 * @throws std::bad_alloc, std::system_error, GraphError, or filesystem
 *         exceptions if Kernel/runtime/scheduler setup fails.
 * @note The scheduler records only its first shutdown entry so the later
 *       destructor cleanup cannot mask an earlier stop performed on the lane
 *       worker.
 */
TEST(GraphStateExecutorLane, KernelCloseJoinsLaneBeforeSchedulerShutdown) {
  ScopedTempDir temp("photospider_graph_state_kernel_close_order_test");
  GraphStateExecutorLaneState lane_state;
  ScopedGraphStateExecutorTestHook lane_hook(lane_state);
  std::atomic<bool> shutdown_called{false};
  std::atomic<bool> shutdown_after_lane_stopped{false};

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const std::string graph_name = "graph_state_kernel_close_order";
  const auto loaded =
      kernel.load_graph(graph_name, (temp.root() / "sessions").string(), "", "",
                        (temp.root() / "cache").string());
  ASSERT_TRUE(loaded.has_value());
  GraphRuntime& runtime =
      testing::KernelTestAccess::runtime(kernel, graph_name);
  runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                        std::make_unique<LaneOrderObservingScheduler>(
                            lane_state.worker_stopped_events, shutdown_called,
                            shutdown_after_lane_stopped));

  ASSERT_TRUE(kernel.close_graph(graph_name));
  EXPECT_TRUE(shutdown_called.load(std::memory_order_acquire));
  EXPECT_TRUE(shutdown_after_lane_stopped.load(std::memory_order_acquire));
  EXPECT_GE(lane_state.worker_stopped_events.load(std::memory_order_acquire),
            1u);
}
#endif

TEST(EmbeddedHostAdapter,
     CoversInteractionCoreWithPublicSnapshotsAndNoKernelExposure) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphLoadRequest load_request = make_load_request(temp.root());
  const GraphSessionId session = load_request.session;
  auto loaded = host->load_graph(load_request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  EXPECT_EQ(loaded.value.value, session.value);

  auto graphs = host->list_graphs();
  ASSERT_TRUE(graphs.status.ok) << graphs.status.message;
  ASSERT_EQ(graphs.value.size(), 1u);
  EXPECT_EQ(graphs.value.front().value, session.value);

  auto ids = host->list_node_ids(session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  ASSERT_EQ(ids.value.size(), 1u);
  EXPECT_EQ(ids.value.front().value, 1);

  auto ending = host->ending_nodes(session);
  ASSERT_TRUE(ending.status.ok) << ending.status.message;
  ASSERT_EQ(ending.value.size(), 1u);
  EXPECT_EQ(ending.value.front().value, 1);

  auto graph_view = host->inspect_graph(session);
  ASSERT_TRUE(graph_view.status.ok) << graph_view.status.message;
  ASSERT_EQ(graph_view.value.nodes.size(), 1u);
  EXPECT_EQ(graph_view.value.session.value, session.value);
  EXPECT_EQ(graph_view.value.nodes.front().name, "host_source");
  EXPECT_EQ(graph_view.value.nodes.front().parameters.at("width"), "6");

  auto node_view = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node_view.status.ok) << node_view.status.message;
  EXPECT_EQ(node_view.value.type, "host_adapter_test");
  EXPECT_EQ(node_view.value.subtype, "source");

  auto tree = host->dependency_tree(session, std::nullopt, true);
  ASSERT_TRUE(tree.status.ok) << tree.status.message;
  EXPECT_EQ(tree.value.scope, HostDependencyTreeScope::EndingNodes);
  ASSERT_EQ(tree.value.entries.size(), 1u);
  EXPECT_EQ(tree.value.entries.front().node.id.value, 1);

  auto traversal = host->traversal_orders(session);
  ASSERT_TRUE(traversal.status.ok) << traversal.status.message;
  ASSERT_EQ(traversal.value.size(), 1u);
  ASSERT_EQ(traversal.value.at(1).size(), 1u);
  EXPECT_EQ(traversal.value.at(1).front().value, 1);

  const HostComputeRequest compute_request = make_compute_request(session);
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto image = host->compute_and_get_image(compute_request);
  ASSERT_TRUE(image.status.ok) << image.status.message;
  EXPECT_EQ(image.value.width, 6);
  EXPECT_EQ(image.value.height, 4);
  EXPECT_EQ(image.value.channels, 1);
  EXPECT_EQ(image.value.device, Device::CPU);
  ASSERT_NE(image.value.data, nullptr);

  auto async_compute = host->compute_async(compute_request);
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;
  OperationStatus async_status = async_compute.value.get();
  EXPECT_TRUE(async_status.ok) << async_status.message;

  auto timing = host->timing(session);
  ASSERT_TRUE(timing.status.ok) << timing.status.message;
  EXPECT_FALSE(timing.value.node_timings.empty());

  auto io_time = host->last_io_time(session);
  ASSERT_TRUE(io_time.status.ok) << io_time.status.message;
  EXPECT_GE(io_time.value, 0.0);

  auto invalid_event_limit = host->drain_compute_events(session, 0);
  EXPECT_FALSE(invalid_event_limit.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_event_limit.status),
            GraphErrc::InvalidParameter);

  auto missing_events = host->drain_compute_events(
      GraphSessionId{"missing-event-session"}, kComputeEventDrainMaxLimit);
  EXPECT_FALSE(missing_events.status.ok);
  EXPECT_EQ(missing_events.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_events.status),
            GraphErrc::NotFound);

  auto events = host->drain_compute_events(session, kComputeEventDrainMaxLimit);
  ASSERT_TRUE(events.status.ok) << events.status.message;
  ASSERT_FALSE(events.value.events.empty());
  EXPECT_GT(events.value.events.front().sequence, 0u);
  EXPECT_LT(events.value.events.back().sequence, kObservationSequenceExhausted);

  auto dirty = host->dirty_region_snapshot(session);
  ASSERT_TRUE(dirty.status.ok) << dirty.status.message;

  auto scheduler_types = host->scheduler_available_types();
  ASSERT_TRUE(scheduler_types.status.ok) << scheduler_types.status.message;
  EXPECT_TRUE(contains_string(scheduler_types.value, "serial_debug"));

  auto replaced = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "serial_debug");
  ASSERT_TRUE(replaced.status.ok) << replaced.status.message;

  auto scheduler_info =
      host->scheduler_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(scheduler_info.status.ok) << scheduler_info.status.message;
  EXPECT_EQ(scheduler_info.value.scheduler_name, "serial_debug");
  EXPECT_NE(scheduler_info.value.stats.find("SerialDebugScheduler"),
            std::string::npos);

  HostComputeRequest trace_request = make_compute_request(session);
  trace_request.cache.force_recache = true;
  trace_request.execution.parallel = true;
  trace_request.telemetry.enable_timing = false;
  auto trace_compute = host->compute(trace_request);
  ASSERT_TRUE(trace_compute.status.ok) << trace_compute.status.message;

  auto invalid_trace_limit = host->scheduler_trace(session, 0, 0);
  EXPECT_FALSE(invalid_trace_limit.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_trace_limit.status),
            GraphErrc::InvalidParameter);

  auto missing_trace = host->scheduler_trace(
      GraphSessionId{"missing-trace-session"}, 0, kSchedulerTraceMaxLimit);
  EXPECT_FALSE(missing_trace.status.ok);
  EXPECT_EQ(missing_trace.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_trace.status),
            GraphErrc::NotFound);

  auto scheduler_trace =
      host->scheduler_trace(session, 0, kSchedulerTraceMaxLimit);
  ASSERT_TRUE(scheduler_trace.status.ok) << scheduler_trace.status.message;
  ASSERT_FALSE(scheduler_trace.value.events.empty());
  EXPECT_GT(scheduler_trace.value.events.front().sequence, 0u);
  EXPECT_LT(scheduler_trace.value.events.back().sequence,
            kObservationSequenceExhausted);

  auto repeated_scheduler_trace =
      host->scheduler_trace(session, 0, kSchedulerTraceMaxLimit);
  ASSERT_TRUE(repeated_scheduler_trace.status.ok)
      << repeated_scheduler_trace.status.message;
  ASSERT_EQ(repeated_scheduler_trace.value.events.size(),
            scheduler_trace.value.events.size());
  EXPECT_EQ(repeated_scheduler_trace.value.events.front().sequence,
            scheduler_trace.value.events.front().sequence);

  auto future_trace = host->scheduler_trace(
      session, kObservationSequenceExhausted - 1, kSchedulerTraceMaxLimit);
  EXPECT_FALSE(future_trace.status.ok);
  EXPECT_EQ(future_trace.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(future_trace.status),
            GraphErrc::InvalidParameter);

  auto premature_terminal_trace = host->scheduler_trace(
      session, kObservationSequenceExhausted, kSchedulerTraceMaxLimit);
  EXPECT_FALSE(premature_terminal_trace.status.ok);
  EXPECT_EQ(premature_terminal_trace.status.domain,
            OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(premature_terminal_trace.status),
            GraphErrc::InvalidParameter);

  auto description = host->scheduler_description("serial_debug");
  ASSERT_TRUE(description.status.ok) << description.status.message;
  EXPECT_NE(description.value.find("Single-threaded"), std::string::npos);

  auto missing_description =
      host->scheduler_description("missing_scheduler_type");
  EXPECT_FALSE(missing_description.status.ok);
  EXPECT_EQ(missing_description.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_description.status),
            GraphErrc::NotFound);
  EXPECT_EQ(missing_description.status.name, "not_found");

  auto plugins = host->plugins_load_report({});
  ASSERT_TRUE(plugins.status.ok) << plugins.status.message;
  EXPECT_EQ(plugins.value.loaded, 0);

  auto seed = host->seed_builtin_ops();
  ASSERT_TRUE(seed.status.ok) << seed.status.message;

  auto op_sources = host->ops_combined_sources();
  ASSERT_TRUE(op_sources.status.ok) << op_sources.status.message;
  EXPECT_EQ(op_sources.value.at("host_adapter_test:source"), "built-in");

  auto yaml = host->get_node_yaml(session, NodeId{1});
  ASSERT_TRUE(yaml.status.ok) << yaml.status.message;
  EXPECT_NE(yaml.value.find("host_source"), std::string::npos);

  auto clear_memory = host->clear_memory_cache(session);
  ASSERT_TRUE(clear_memory.status.ok) << clear_memory.status.message;

  auto close = host->close_graph(session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

/**
 * @brief Verifies scheduler shutdown failure preserves a retryable Host
 * session.
 *
 * @throws Nothing when the fixture, Host status mapping, and cleanup behave as
 *         specified; GoogleTest records any mismatch.
 * @note The first close throws while the fixture remains running. The plugin
 * owner normalizes that runtime failure to ComputeError; the Host must reopen
 * admission, retain the graph, and invoke shutdown again after injection is
 * removed.
 */
TEST(EmbeddedHostAdapter, CloseShutdownFailureRetainsSessionAndAllowsRetry) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_close_failure_test");
  ScopedSchedulerPluginCleanup scheduler_cleanup;
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const std::filesystem::path plugin_path =
      destroy_count_scheduler_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "scheduler close-failure fixture was not built: " << plugin_path;
  SchedulerFixtureExports fixture(plugin_path);
  fixture.reset_counts();
  const VoidResult plugin_load = host->scheduler_load(plugin_path.string());
  ASSERT_TRUE(plugin_load.status.ok) << plugin_load.status.message;

  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = kDestroyCountSchedulerType;
  scheduler_config.rt_type = "serial_debug";
  const VoidResult configured =
      host->configure_scheduler_defaults(scheduler_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "close_failure_retry_graph");
  GraphStateExecutorLaneState lane_state;
  ScopedGraphStateExecutorTestHook lane_hook(lane_state);
  HostComputeRequest stale_error_request = make_compute_request(session);
  stale_error_request.node = NodeId{99};
  const Result<ImageBuffer> stale_compute =
      host->compute_and_get_image(stale_error_request);
  ASSERT_FALSE(stale_compute.status.ok);
  const OperationStatus last_error_before_failure = host->last_error(session);
  ASSERT_FALSE(last_error_before_failure.ok);

  {
    ScopedEnvironmentValue failure(kSchedulerFailureEnvironment,
                                   "shutdown_runtime_error");
    const VoidResult failed_close = host->close_graph(session);
    EXPECT_FALSE(failed_close.status.ok);
    EXPECT_EQ(failed_close.status.domain, OperationErrorDomain::Graph);
    EXPECT_EQ(checked_graph_error_code(failed_close.status),
              GraphErrc::ComputeError);
    EXPECT_EQ(failed_close.status.name, "compute_error");
    EXPECT_NE(failed_close.status.message.find("fixture shutdown failure"),
              std::string::npos);
    EXPECT_EQ(fixture.shutdown_count(), 1);
    EXPECT_EQ(lane_state.reopened_events.load(std::memory_order_acquire), 1u);
    EXPECT_LE(lane_state.max_worker_threads.load(std::memory_order_acquire),
              1u);
  }

  const Result<std::vector<GraphSessionId>> after_failure = host->list_graphs();
  ASSERT_TRUE(after_failure.status.ok) << after_failure.status.message;
  ASSERT_EQ(after_failure.value.size(), 1u);
  EXPECT_EQ(after_failure.value.front().value, session.value);

  const Result<SchedulerInfoSnapshot> admitted_after_failure =
      host->scheduler_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(admitted_after_failure.status.ok)
      << admitted_after_failure.status.message;
  EXPECT_EQ(admitted_after_failure.value.scheduler_name,
            kDestroyCountSchedulerType);
  const OperationStatus last_error_after_failure = host->last_error(session);
  EXPECT_EQ(last_error_after_failure.ok, last_error_before_failure.ok);
  EXPECT_EQ(last_error_after_failure.domain, last_error_before_failure.domain);
  EXPECT_EQ(last_error_after_failure.code, last_error_before_failure.code);
  EXPECT_EQ(last_error_after_failure.name, last_error_before_failure.name);
  EXPECT_EQ(last_error_after_failure.message,
            last_error_before_failure.message);

  const VoidResult retry_close = host->close_graph(session);
  EXPECT_TRUE(retry_close.status.ok) << retry_close.status.message;
  EXPECT_EQ(fixture.shutdown_count(), 2);
  EXPECT_GE(lane_state.worker_stopped_events.load(std::memory_order_acquire),
            2u);
  EXPECT_EQ(lane_state.reopened_events.load(std::memory_order_acquire), 1u);

  const Result<std::vector<GraphSessionId>> after_retry = host->list_graphs();
  ASSERT_TRUE(after_retry.status.ok) << after_retry.status.message;
  EXPECT_TRUE(after_retry.value.empty());
  const OperationStatus last_error_after_retry = host->last_error(session);
  EXPECT_TRUE(last_error_after_retry.ok) << last_error_after_retry.message;

  const VoidResult missing_close = host->close_graph(session);
  EXPECT_FALSE(missing_close.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_close.status),
            GraphErrc::NotFound);
}

/**
 * @brief Verifies a shutdown GraphError::NotFound cannot masquerade as absence.
 *
 * @throws Nothing when close remapping, graph retention, and retry hold;
 *         GoogleTest records any mismatch.
 * @note Only Kernel's explicit false result denotes an absent graph. A
 * scheduler GraphError::NotFound is remapped to Unknown and keeps the session
 * loaded.
 */
TEST(EmbeddedHostAdapter,
     CloseShutdownGraphNotFoundMapsUnknownAndRetainsGraph) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_close_graph_error_test");
  ScopedSchedulerPluginCleanup scheduler_cleanup;
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const std::filesystem::path plugin_path =
      destroy_count_scheduler_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "scheduler close-failure fixture was not built: " << plugin_path;
  SchedulerFixtureExports fixture(plugin_path);
  fixture.reset_counts();
  const VoidResult plugin_load = host->scheduler_load(plugin_path.string());
  ASSERT_TRUE(plugin_load.status.ok) << plugin_load.status.message;

  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = kDestroyCountSchedulerType;
  scheduler_config.rt_type = "serial_debug";
  const VoidResult configured =
      host->configure_scheduler_defaults(scheduler_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "close_graph_error_retry_graph");
  {
    ScopedEnvironmentValue failure(kSchedulerFailureEnvironment,
                                   "shutdown_graph_not_found");
    const VoidResult failed_close = host->close_graph(session);
    EXPECT_FALSE(failed_close.status.ok);
    EXPECT_EQ(checked_graph_error_code(failed_close.status),
              GraphErrc::Unknown);
    EXPECT_EQ(failed_close.status.name, "unknown");
    EXPECT_NE(failed_close.status.message.find(
                  "fixture shutdown graph-not-found failure"),
              std::string::npos);
    EXPECT_EQ(fixture.shutdown_count(), 1);
  }

  const Result<std::vector<GraphSessionId>> retained = host->list_graphs();
  ASSERT_TRUE(retained.status.ok) << retained.status.message;
  ASSERT_EQ(retained.value.size(), 1u);
  EXPECT_EQ(retained.value.front().value, session.value);

  const Result<SchedulerInfoSnapshot> admitted =
      host->scheduler_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(admitted.status.ok) << admitted.status.message;
  EXPECT_EQ(admitted.value.scheduler_name, kDestroyCountSchedulerType);

  const VoidResult retry = host->close_graph(session);
  EXPECT_TRUE(retry.status.ok) << retry.status.message;
  EXPECT_EQ(fixture.shutdown_count(), 2);
}

TEST(EmbeddedHostAdapter,
     SpatialSnapshotPreservesOutputExtentSeparatelyFromRoi) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_spatial_extent_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "spatial_extent", "resized_extent");
  const HostComputeRequest compute_request = make_compute_request(session);
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto node_view = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node_view.status.ok) << node_view.status.message;
  ASSERT_TRUE(node_view.value.space.has_value());
  EXPECT_EQ(node_view.value.space->extent.width, 6);
  EXPECT_EQ(node_view.value.space->extent.height, 4);
  EXPECT_EQ(node_view.value.space->absolute_roi.width, 12);
  EXPECT_EQ(node_view.value.space->absolute_roi.height, 9);
  EXPECT_EQ(node_view.value.space->inverse_matrix[2], 5.0);
  EXPECT_EQ(node_view.value.space->inverse_matrix[5], 7.0);
  EXPECT_EQ(node_view.value.space->local_inverse_matrix[2], 11.0);
  EXPECT_EQ(node_view.value.space->local_inverse_matrix[5], 13.0);

  auto graph_view = host->inspect_graph(session);
  ASSERT_TRUE(graph_view.status.ok) << graph_view.status.message;
  ASSERT_EQ(graph_view.value.nodes.size(), 1u);
  ASSERT_TRUE(graph_view.value.nodes.front().space.has_value());
  EXPECT_EQ(graph_view.value.nodes.front().space->extent.width, 6);
  EXPECT_EQ(graph_view.value.nodes.front().space->extent.height, 4);
  EXPECT_EQ(graph_view.value.nodes.front().space->absolute_roi.width, 12);
  EXPECT_EQ(graph_view.value.nodes.front().space->absolute_roi.height, 9);
  EXPECT_EQ(graph_view.value.nodes.front().space->local_inverse_matrix[2],
            11.0);
  EXPECT_EQ(graph_view.value.nodes.front().space->local_inverse_matrix[5],
            13.0);

  auto tree = host->dependency_tree(session, std::nullopt, true);
  ASSERT_TRUE(tree.status.ok) << tree.status.message;
  ASSERT_EQ(tree.value.entries.size(), 1u);
  ASSERT_TRUE(tree.value.entries.front().node.space.has_value());
  EXPECT_EQ(tree.value.entries.front().node.space->extent.width, 6);
  EXPECT_EQ(tree.value.entries.front().node.space->extent.height, 4);
  EXPECT_EQ(tree.value.entries.front().node.space->absolute_roi.width, 12);
  EXPECT_EQ(tree.value.entries.front().node.space->absolute_roi.height, 9);
  EXPECT_EQ(tree.value.entries.front().node.space->local_inverse_matrix[2],
            11.0);
  EXPECT_EQ(tree.value.entries.front().node.space->local_inverse_matrix[5],
            13.0);
}

TEST(EmbeddedHostAdapter, ComputePlanningSnapshotsUsePublicValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_planning_snapshot_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "planning_snapshot");

  auto before_compute = host->compute_planning_snapshot(session);
  ASSERT_TRUE(before_compute.status.ok) << before_compute.status.message;
  EXPECT_FALSE(before_compute.value.has_value());

  auto before_history = host->recent_compute_planning_snapshots(session);
  ASSERT_TRUE(before_history.status.ok) << before_history.status.message;
  EXPECT_TRUE(before_history.value.empty());

  HostComputeRequest compute_request = make_compute_request(session);
  compute_request.execution.parallel = true;
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto latest = host->compute_planning_snapshot(session);
  ASSERT_TRUE(latest.status.ok) << latest.status.message;
  ASSERT_TRUE(latest.value.has_value());
  EXPECT_EQ(latest.value->intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(latest.value->target_node.value, 1);
  EXPECT_TRUE(latest.value->parallel);
  EXPECT_EQ(latest.value->planned_node_count, 1u);
  EXPECT_GE(latest.value->task_count, 1u);
  EXPECT_GE(latest.value->active_task_count, 1u);
  ASSERT_FALSE(latest.value->planned_node_sample.empty());
  EXPECT_EQ(latest.value->planned_node_sample.front().value, 1);
  ASSERT_FALSE(latest.value->task_sample.empty());
  EXPECT_EQ(latest.value->task_sample.front().node.value, 1);
  EXPECT_FALSE(latest.value->task_sample.front().kind.empty());

  auto history = host->recent_compute_planning_snapshots(session);
  ASSERT_TRUE(history.status.ok) << history.status.message;
  ASSERT_FALSE(history.value.empty());
  EXPECT_EQ(history.value.back().target_node.value, 1);

  auto missing = host->compute_planning_snapshot(GraphSessionId{"missing"});
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, AsyncComputeCanFinishAfterCloseGraphRequest) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "async_close_graph", "slow_source");
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;

  auto async_compute = host->compute_async(request);
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;

  auto close = host->close_graph(session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
  EXPECT_EQ(async_compute.value.wait_for(std::chrono::milliseconds(0)),
            std::future_status::ready);

  OperationStatus async_status = async_compute.value.get();
  EXPECT_TRUE(async_status.ok) << async_status.message;

  auto ids_after_close = host->list_node_ids(session);
  EXPECT_FALSE(ids_after_close.status.ok);
  EXPECT_EQ(checked_graph_error_code(ids_after_close.status),
            GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, AsyncComputeRejectsNewWorkWhileCloseIsWaiting) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "async_close_gate_graph", "slow_source", 250);
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;

  auto initial_async = host->compute_async(request);
  ASSERT_TRUE(initial_async.status.ok) << initial_async.status.message;

  auto close_future = std::async(std::launch::async, [&host, session]() {
    return host->close_graph(session);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_EQ(close_future.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  auto rejected_async = host->compute_async(request);
  EXPECT_FALSE(rejected_async.status.ok);
  EXPECT_EQ(checked_graph_error_code(rejected_async.status),
            GraphErrc::NotFound);

  OperationStatus initial_status = initial_async.value.get();
  EXPECT_TRUE(initial_status.ok) << initial_status.message;

  auto close = close_future.get();
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

TEST(EmbeddedHostAdapter, AsyncComputeFailureStatusSurvivesCloseGraph) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_failure_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  GraphLoadRequest missing_op_load;
  missing_op_load.session = GraphSessionId{"async_missing_op_graph"};
  missing_op_load.root_dir = (temp.root() / "sessions").string();
  missing_op_load.yaml_path =
      (temp.root() / "source" / "async_missing_op_graph.yaml").string();
  missing_op_load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_unregistered_op_graph(missing_op_load.yaml_path);
  auto loaded_missing_op = host->load_graph(missing_op_load);
  ASSERT_TRUE(loaded_missing_op.status.ok) << loaded_missing_op.status.message;

  auto async_compute =
      host->compute_async(make_compute_request(missing_op_load.session));
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;

  auto close = host->close_graph(missing_op_load.session);
  ASSERT_TRUE(close.status.ok) << close.status.message;
  EXPECT_EQ(async_compute.value.wait_for(std::chrono::milliseconds(0)),
            std::future_status::ready);

  OperationStatus async_status = async_compute.value.get();
  EXPECT_FALSE(async_status.ok);
  EXPECT_EQ(checked_graph_error_code(async_status), GraphErrc::NoOperation);
  EXPECT_NE(async_status.message.find("No op"), std::string::npos);

  auto closed_error = host->last_error(missing_op_load.session);
  EXPECT_TRUE(closed_error.ok) << closed_error.message;
}

/**
 * @brief Verifies close waits for an admitted synchronous Host compute.
 *
 * @throws Nothing when close remains pending while the deterministic operation
 * holds graph-state execution, then both calls finish without runtime lifetime
 * overlap.
 * @note This exercises the embedded admission gate and Kernel close
 * serialization through public Host methods rather than direct runtime access.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedSynchronousCompute) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_sync_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "sync_close_gate_graph", "blocking_source");

  std::promise<void> release_compute;
  configure_host_blocking_source(release_compute.get_future().share());
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  auto compute_future = std::async(
      std::launch::async, [&host, request] { return host->compute(request); });

  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    reset_host_blocking_source();
    (void)host->close_graph(session);
    FAIL() << "blocking synchronous Host compute did not start";
  }

  std::promise<void> close_entered;
  auto close_entered_future = close_entered.get_future();
  auto close_future = std::async(std::launch::async, [&] {
    close_entered.set_value();
    return host->close_graph(session);
  });
  EXPECT_EQ(close_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(close_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const VoidResult compute = compute_future.get();
  EXPECT_TRUE(compute.status.ok) << compute.status.message;
  const VoidResult close = close_future.get();
  EXPECT_TRUE(close.status.ok) << close.status.message;

  reset_host_blocking_source();
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING) && \
    defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
/**
 * @brief Verifies close waits for a graph save admitted before its marker.
 *
 * @return Nothing; GoogleTest assertions report lifecycle ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note A real blocking compute holds GraphStateExecutor while save acquires
 * Host admission. The close marker must then wait for save to complete.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedSaveGraph) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_save_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "save_close_gate_graph", "blocking_source");
  const auto output = temp.root() / "saved_content.yaml";
  const auto race = run_admitted_operation_close_race(
      *host, session, EmbeddedOperationTestEvent::SaveGraph, "save_graph",
      [&] { return host->save_graph(session, output.string()); });

  EXPECT_TRUE(race.blocker.status.ok) << race.blocker.status.message;
  EXPECT_TRUE(race.operation.status.ok) << race.operation.status.message;
  EXPECT_TRUE(race.close.status.ok) << race.close.status.message;
}

/**
 * @brief Verifies reload admission spans Kernel work and public translation.
 *
 * @return Nothing; GoogleTest assertions report lifecycle result mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note A real blocking compute holds GraphStateExecutor after reload is
 *       admitted. Close must wait for that reload, while a second reload after
 *       the close marker is published must return public NotFound immediately.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedReloadGraph) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_reload_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "reload_close_gate_graph", "blocking_source");
  const auto reload_path = temp.root() / "source" / "admitted_reload.yaml";
  write_host_adapter_graph(reload_path, 13, 8);

  const auto race = run_admitted_operation_close_race(
      *host, session, EmbeddedOperationTestEvent::ReloadGraph, "reload_graph",
      [&] { return host->reload_graph(session, reload_path.string()); }, true);

  EXPECT_TRUE(race.blocker.status.ok) << race.blocker.status.message;
  EXPECT_TRUE(race.operation.status.ok) << race.operation.status.message;
  ASSERT_TRUE(race.rejected_while_closing.has_value());
  EXPECT_FALSE(race.rejected_while_closing->status.ok);
  EXPECT_EQ(checked_graph_error_code(race.rejected_while_closing->status),
            GraphErrc::NotFound);
  EXPECT_TRUE(race.close.status.ok) << race.close.status.message;
}

/**
 * @brief Verifies close waits for an admitted node-YAML replacement.
 *
 * @return Nothing; GoogleTest assertions report lifecycle ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The replacement waits behind a real blocking compute after admission;
 * close must retain the runtime until replacement and status mapping finish.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedSetNodeYaml) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_set_yaml_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "set_yaml_close_gate_graph", "blocking_source");
  const std::string replacement =
      replacement_node_yaml("admitted_replacement", 9, 5);
  const auto race = run_admitted_operation_close_race(
      *host, session, EmbeddedOperationTestEvent::SetNodeYaml, "set_node_yaml",
      [&] { return host->set_node_yaml(session, NodeId{1}, replacement); });

  EXPECT_TRUE(race.blocker.status.ok) << race.blocker.status.message;
  EXPECT_TRUE(race.operation.status.ok) << race.operation.status.message;
  EXPECT_TRUE(race.close.status.ok) << race.close.status.message;
}

/**
 * @brief Verifies close waits for an admitted forward ROI projection.
 *
 * @return Nothing; GoogleTest assertions report lifecycle ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note A blocking source compute holds graph-state execution while the
 * projection is admitted; close may remove the runtime only after projection.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedForwardRoiProjection) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_forward_roi_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  GraphLoadRequest load;
  load.session = GraphSessionId{"forward_roi_close_gate_graph"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = (temp.root() / "source" / "roi_graph.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_roi_graph(load.yaml_path, "blocking_source");
  const auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  const PixelRect expected{1, 1, 3, 2};
  const auto race = run_admitted_operation_close_race(
      *host, load.session, EmbeddedOperationTestEvent::ForwardRoiProjection,
      "project_roi", [&] {
        return host->project_roi(load.session, NodeId{1}, expected, NodeId{2});
      });

  EXPECT_TRUE(race.blocker.status.ok) << race.blocker.status.message;
  EXPECT_TRUE(race.operation.status.ok) << race.operation.status.message;
  EXPECT_EQ(race.operation.value.x, expected.x);
  EXPECT_EQ(race.operation.value.y, expected.y);
  EXPECT_EQ(race.operation.value.width, expected.width);
  EXPECT_EQ(race.operation.value.height, expected.height);
  EXPECT_TRUE(race.close.status.ok) << race.close.status.message;
}

/**
 * @brief Verifies close waits for an admitted backward ROI projection.
 *
 * @return Nothing; GoogleTest assertions report lifecycle ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The backward request waits behind real graph-state work after Host
 * admission, so close must preserve the runtime through public translation.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedBackwardRoiProjection) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_backward_roi_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  GraphLoadRequest load;
  load.session = GraphSessionId{"backward_roi_close_gate_graph"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = (temp.root() / "source" / "roi_graph.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_roi_graph(load.yaml_path, "blocking_source");
  const auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  const PixelRect expected{1, 1, 3, 2};
  const auto race = run_admitted_operation_close_race(
      *host, load.session, EmbeddedOperationTestEvent::BackwardRoiProjection,
      "project_roi_backward", [&] {
        return host->project_roi_backward(load.session, NodeId{2}, expected,
                                          NodeId{1});
      });

  EXPECT_TRUE(race.blocker.status.ok) << race.blocker.status.message;
  EXPECT_TRUE(race.operation.status.ok) << race.operation.status.message;
  EXPECT_EQ(race.operation.value.x, expected.x);
  EXPECT_EQ(race.operation.value.y, expected.y);
  EXPECT_EQ(race.operation.value.width, expected.width);
  EXPECT_EQ(race.operation.value.height, expected.height);
  EXPECT_TRUE(race.close.status.ok) << race.close.status.message;
}

/**
 * @brief Verifies close waits for timing admitted before its marker.
 *
 * @return Nothing; GoogleTest assertions report lifecycle ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The helper's blocking compute also publishes timing data before the
 * admitted inspection enters Kernel, making a successful snapshot observable.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedTimingInspection) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_timing_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "timing_close_gate_graph", "blocking_source");

  const auto race = run_admitted_operation_close_race(
      *host, session, EmbeddedOperationTestEvent::Timing, "timing",
      [&] { return host->timing(session); });

  EXPECT_TRUE(race.blocker.status.ok) << race.blocker.status.message;
  EXPECT_TRUE(race.operation.status.ok) << race.operation.status.message;
  EXPECT_FALSE(race.operation.value.node_timings.empty());
  EXPECT_TRUE(race.close.status.ok) << race.close.status.message;
}

/**
 * @brief Verifies close waits for all-cache clear admitted before its marker.
 *
 * @return Nothing; GoogleTest assertions report lifecycle ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The all-cache operation remains synchronous and holds only logical
 * session admission while waiting to enter backend graph-state work.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedAllCacheClear) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_clear_cache_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "clear_cache_close_gate_graph", "blocking_source");

  const auto race = run_admitted_operation_close_race(
      *host, session, EmbeddedOperationTestEvent::ClearCache, "clear_cache",
      [&] { return host->clear_cache(session); });

  EXPECT_TRUE(race.blocker.status.ok) << race.blocker.status.message;
  EXPECT_TRUE(race.operation.status.ok) << race.operation.status.message;
  EXPECT_TRUE(race.close.status.ok) << race.close.status.message;
}
#endif

/**
 * @brief Verifies concurrent closes settle as owner success then waiter absent.
 *
 * @throws Nothing when deterministic admission ordering and close results hold;
 *         GoogleTest records any mismatch.
 * @note A blocking admitted compute keeps the first close pending after it has
 *       claimed the close marker. A BUILD_TESTING callback proves the second
 *       close reaches duplicate-marker wait before the compute is released.
 */
TEST(EmbeddedHostAdapter, ConcurrentCloseOwnerSuccessMakesWaiterNotFound) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_concurrent_close_success_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "concurrent_close_success_graph", "blocking_source");

  std::promise<void> release_compute;
  configure_host_blocking_source(release_compute.get_future().share());
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  auto compute_future = std::async(
      std::launch::async, [&host, request] { return host->compute(request); });

  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    reset_host_blocking_source();
    (void)host->close_graph(session);
    FAIL() << "blocking synchronous Host compute did not start";
  }

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  EmbeddedLifecycleEventState close_events;
  ScopedEmbeddedLifecycleTestHook close_hook(close_events);
#endif
  auto owner = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_atomic_event_count(close_events.marker_claimed, 1,
                                   std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    reset_host_blocking_source();
    FAIL() << "owner close did not claim the session marker";
  }
#endif

  auto waiter = std::async(std::launch::async,
                           [&] { return host->close_graph(session); });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_atomic_event_count(close_events.duplicate_about_to_wait, 1,
                                   std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    (void)waiter.get();
    reset_host_blocking_source();
    FAIL() << "second close did not enter duplicate-marker wait";
  }
#endif

  release_compute.set_value();
  const VoidResult compute = compute_future.get();
  EXPECT_TRUE(compute.status.ok) << compute.status.message;
  const VoidResult owner_result = owner.get();
  EXPECT_TRUE(owner_result.status.ok) << owner_result.status.message;
  const VoidResult waiter_result = waiter.get();
  EXPECT_FALSE(waiter_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(waiter_result.status),
            GraphErrc::NotFound);
  reset_host_blocking_source();
}

/**
 * @brief Verifies every duplicate close waiter claims the marker exclusively.
 *
 * @throws Nothing when one owner succeeds and two waiters serialize to
 *         NotFound; GoogleTest records any mismatch.
 * @note Both waiters are observed inside the marker wait before compute is
 *       released. This catches a single-check implementation that wakes both
 *       waiters and lets them enter Kernel close concurrently.
 */
TEST(EmbeddedHostAdapter, ThreeConcurrentClosesSerializeEveryWaiter) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_three_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "three_close_graph", "blocking_source");

  std::promise<void> release_compute;
  configure_host_blocking_source(release_compute.get_future().share());
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  auto compute_future = std::async(
      std::launch::async, [&host, request] { return host->compute(request); });
  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    reset_host_blocking_source();
    (void)host->close_graph(session);
    FAIL() << "blocking synchronous Host compute did not start";
  }

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  EmbeddedLifecycleEventState close_events;
  ScopedEmbeddedLifecycleTestHook close_hook(close_events);
#endif
  auto owner = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_atomic_event_count(close_events.marker_claimed, 1,
                                   std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    reset_host_blocking_source();
    FAIL() << "owner close did not claim the session marker";
  }
#endif

  auto first_waiter = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
  auto second_waiter = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_atomic_event_count(close_events.duplicate_about_to_wait, 2,
                                   std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    (void)first_waiter.get();
    (void)second_waiter.get();
    reset_host_blocking_source();
    FAIL() << "both duplicate closes did not enter marker waits";
  }
#endif

  release_compute.set_value();
  EXPECT_TRUE(compute_future.get().status.ok);
  const VoidResult owner_result = owner.get();
  EXPECT_TRUE(owner_result.status.ok) << owner_result.status.message;
  const VoidResult first_result = first_waiter.get();
  const VoidResult second_result = second_waiter.get();
  EXPECT_FALSE(first_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(first_result.status), GraphErrc::NotFound);
  EXPECT_FALSE(second_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(second_result.status),
            GraphErrc::NotFound);
  reset_host_blocking_source();
}

/**
 * @brief Verifies a waiter retries after the owner consumes a one-shot failure.
 *
 * @throws Nothing when the owner returns normalized ComputeError and the waiter
 * closes the retained graph; GoogleTest records any mismatch.
 * @note The process-scoped fixture fails only its first shutdown invocation, so
 *       no environment mutation races the two close attempts.
 */
TEST(EmbeddedHostAdapter, ConcurrentCloseRetriesAfterEarlierShutdownFailure) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_concurrent_close_failure_test");
  ScopedSchedulerPluginCleanup scheduler_cleanup;
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const std::filesystem::path plugin_path =
      destroy_count_scheduler_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  SchedulerFixtureExports fixture(plugin_path);
  fixture.reset_counts();
  const VoidResult plugin_load = host->scheduler_load(plugin_path.string());
  ASSERT_TRUE(plugin_load.status.ok) << plugin_load.status.message;

  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = kDestroyCountSchedulerType;
  scheduler_config.rt_type = "serial_debug";
  ASSERT_TRUE(host->configure_scheduler_defaults(scheduler_config).status.ok);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "concurrent_close_failure_graph", "blocking_source");
  std::promise<void> release_compute;
  configure_host_blocking_source(release_compute.get_future().share());
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  auto compute_future = std::async(
      std::launch::async, [&host, request] { return host->compute(request); });
  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    reset_host_blocking_source();
    FAIL() << "blocking synchronous Host compute did not start";
  }

  ScopedEnvironmentValue failure(kSchedulerFailureEnvironment,
                                 "shutdown_runtime_error_once");
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  EmbeddedLifecycleEventState close_events;
  ScopedEmbeddedLifecycleTestHook close_hook(close_events);
#endif
  auto owner = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_atomic_event_count(close_events.marker_claimed, 1,
                                   std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    reset_host_blocking_source();
    FAIL() << "owner close did not claim the session marker";
  }
#endif

  auto waiter = std::async(std::launch::async,
                           [&] { return host->close_graph(session); });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_atomic_event_count(close_events.duplicate_about_to_wait, 1,
                                   std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    (void)waiter.get();
    reset_host_blocking_source();
    FAIL() << "second close did not enter duplicate-marker wait";
  }
#endif

  release_compute.set_value();
  EXPECT_TRUE(compute_future.get().status.ok);
  const VoidResult owner_result = owner.get();
  EXPECT_FALSE(owner_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(owner_result.status),
            GraphErrc::ComputeError);
  const VoidResult waiter_result = waiter.get();
  EXPECT_TRUE(waiter_result.status.ok) << waiter_result.status.message;
  EXPECT_EQ(fixture.shutdown_count(), 2);
  reset_host_blocking_source();
}

/**
 * @brief Verifies overlapping failures for one session keep distinct statuses.
 *
 * @throws Nothing when both public futures preserve their work-item-owned
 * status after the shared Kernel diagnostic has changed.
 * @note Both requests are accepted before either future is consumed. They use
 * one graph-state executor but fail with different stable GraphErrc values, so
 * reconstructing either result from the final shared LastError is invalid.
 */
TEST(EmbeddedHostAdapter, OverlappingAsyncFailuresOwnTheirExactStatus) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_exact_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  GraphLoadRequest load;
  load.session = GraphSessionId{"async_exact_status_graph"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path =
      (temp.root() / "source" / "async_exact_status_graph.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_unregistered_op_graph(load.yaml_path);
  auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest missing_op_request = make_compute_request(load.session);
  HostComputeRequest missing_node_request = missing_op_request;
  missing_node_request.node = NodeId{99};

  auto missing_op = host->compute_async(missing_op_request);
  auto missing_node = host->compute_async(missing_node_request);
  ASSERT_TRUE(missing_op.status.ok) << missing_op.status.message;
  ASSERT_TRUE(missing_node.status.ok) << missing_node.status.message;
  ASSERT_EQ(missing_op.value.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(missing_node.value.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  const OperationStatus missing_op_status = missing_op.value.get();
  const OperationStatus missing_node_status = missing_node.value.get();
  EXPECT_FALSE(missing_op_status.ok);
  EXPECT_EQ(missing_op_status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_op_status),
            GraphErrc::NoOperation);
  EXPECT_EQ(missing_op_status.name, "no_operation");
  EXPECT_FALSE(missing_op_status.message.empty());
  EXPECT_FALSE(missing_node_status.ok);
  EXPECT_EQ(missing_node_status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_node_status), GraphErrc::NotFound);
  EXPECT_EQ(missing_node_status.name, "not_found");
  EXPECT_FALSE(missing_node_status.message.empty());
  EXPECT_NE(missing_op_status.message, missing_node_status.message);

  auto close = host->close_graph(load.session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

TEST(EmbeddedHostAdapter, SyncComputePropagatesNodeExecutionBadAlloc) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_bad_alloc_sync");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "bad_alloc_sync", "resource_exhausted");
  const HostComputeRequest request = make_compute_request(session);

  try {
    const VoidResult result = host->compute(request);
    FAIL() << "std::bad_alloc was converted to Host status: code="
           << static_cast<int>(result.status.code)
           << " message=" << result.status.message;
  } catch (const std::bad_alloc&) {
    SUCCEED();
  }
}

TEST(EmbeddedHostAdapter, AsyncComputeFuturePropagatesNodeExecutionBadAlloc) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_bad_alloc_async");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "bad_alloc_async", "resource_exhausted");
  auto scheduled = host->compute_async(make_compute_request(session));
  ASSERT_TRUE(scheduled.status.ok) << scheduled.status.message;
  ASSERT_TRUE(scheduled.value.valid());

  try {
    const OperationStatus status = scheduled.value.get();
    FAIL() << "std::bad_alloc was converted by async Host path: code="
           << static_cast<int>(status.code) << " message=" << status.message;
  } catch (const std::bad_alloc&) {
    SUCCEED();
  }
}

TEST(EmbeddedHostAdapter, ComputeReturnsNotFoundForMissingSession) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_compute_missing_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  HostComputeRequest missing_request =
      make_compute_request(GraphSessionId{"missing_compute_graph"});
  auto missing = host->compute(missing_request);
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);
  auto missing_image = host->compute_and_get_image(missing_request);
  EXPECT_FALSE(missing_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_image.status),
            GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "closed_compute_graph");
  HostComputeRequest closed_request = make_compute_request(session);
  auto initial = host->compute(closed_request);
  ASSERT_TRUE(initial.status.ok) << initial.status.message;

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed = host->compute(closed_request);
  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed.status), GraphErrc::NotFound);
  auto closed_image = host->compute_and_get_image(closed_request);
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, CloseGraphClearsStaleLastErrorBeforeImageCompute) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_close_clears_error_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "close_clears_error_graph");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};

  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  ASSERT_FALSE(missing_node_image.status.ok);
  ASSERT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);

  auto stale_error = host->last_error(session);
  ASSERT_FALSE(stale_error.ok);
  ASSERT_EQ(checked_graph_error_code(stale_error), GraphErrc::NotFound);

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed_error = host->last_error(session);
  EXPECT_TRUE(closed_error.ok) << closed_error.message;

  auto closed_image =
      host->compute_and_get_image(make_compute_request(session));
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ComputeImagePreservesBackendFailureStatus) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_image_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "image_status_graph");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};

  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  EXPECT_FALSE(missing_node_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);
  EXPECT_FALSE(missing_node_image.status.message.empty());

  auto missing_node_error = host->last_error(session);
  EXPECT_FALSE(missing_node_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_error), GraphErrc::NotFound);
  EXPECT_FALSE(missing_node_error.message.empty());

  auto recovered_image =
      host->compute_and_get_image(make_compute_request(session));
  ASSERT_TRUE(recovered_image.status.ok) << recovered_image.status.message;
  auto cleared_error = host->last_error(session);
  EXPECT_TRUE(cleared_error.ok) << cleared_error.message;

  GraphLoadRequest missing_op_load;
  missing_op_load.session = GraphSessionId{"image_missing_op_graph"};
  missing_op_load.root_dir = (temp.root() / "sessions").string();
  missing_op_load.yaml_path =
      (temp.root() / "source" / "image_missing_op_graph.yaml").string();
  missing_op_load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_unregistered_op_graph(missing_op_load.yaml_path);
  auto loaded_missing_op = host->load_graph(missing_op_load);
  ASSERT_TRUE(loaded_missing_op.status.ok) << loaded_missing_op.status.message;

  auto missing_op_image = host->compute_and_get_image(
      make_compute_request(missing_op_load.session));
  EXPECT_FALSE(missing_op_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_op_image.status),
            GraphErrc::NoOperation);
  EXPECT_NE(missing_op_image.status.message.find("No op"), std::string::npos);

  auto missing_op_error = host->last_error(missing_op_load.session);
  EXPECT_FALSE(missing_op_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_op_error), GraphErrc::NoOperation);
  EXPECT_NE(missing_op_error.message.find("No op"), std::string::npos);
}

TEST(EmbeddedHostAdapter, ComputeImagePreservesSuccessfulEmptyOutput) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_empty_image_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "empty_image_graph", "no_image");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};
  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  ASSERT_FALSE(missing_node_image.status.ok);
  ASSERT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);

  auto empty_image = host->compute_and_get_image(make_compute_request(session));
  ASSERT_TRUE(empty_image.status.ok) << empty_image.status.message;
  EXPECT_EQ(empty_image.value.width, 0);
  EXPECT_EQ(empty_image.value.height, 0);
  EXPECT_EQ(empty_image.value.data, nullptr);

  auto cleared_error = host->last_error(session);
  EXPECT_TRUE(cleared_error.ok) << cleared_error.message;

  auto closed = host->close_graph(session);
  ASSERT_TRUE(closed.status.ok) << closed.status.message;

  auto closed_image =
      host->compute_and_get_image(make_compute_request(session));
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ReplaceSchedulerReturnsNotFoundForMissingSession) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_scheduler_missing_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  auto missing = host->replace_scheduler(
      GraphSessionId{"missing_scheduler_graph"},
      ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "closed_scheduler_graph");
  auto invalid_type = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "missing_scheduler_type");
  EXPECT_FALSE(invalid_type.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_type.status),
            GraphErrc::InvalidParameter);

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ReloadSaveSetNodeAndClearGraphReturnStatuses) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_graph_mutation_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "graph_mutation");

  auto set = host->set_node_yaml(session, NodeId{1},
                                 replacement_node_yaml("host_replaced", 9, 2));
  ASSERT_TRUE(set.status.ok) << set.status.message;

  auto node = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node.status.ok) << node.status.message;
  EXPECT_EQ(node.value.name, "host_replaced");
  EXPECT_EQ(node.value.parameters.at("width"), "9");

  const auto saved_path = temp.root() / "saved" / "saved_graph.yaml";
  std::filesystem::create_directories(saved_path.parent_path());
  auto save = host->save_graph(session, saved_path.string());
  ASSERT_TRUE(save.status.ok) << save.status.message;
  EXPECT_TRUE(std::filesystem::exists(saved_path));

  const auto reload_path = temp.root() / "source" / "reload_graph.yaml";
  write_host_adapter_graph(reload_path, 11, 5);
  auto reload = host->reload_graph(session, reload_path.string());
  ASSERT_TRUE(reload.status.ok) << reload.status.message;

  auto reloaded = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(reloaded.status.ok) << reloaded.status.message;
  EXPECT_EQ(reloaded.value.parameters.at("width"), "11");

  auto missing_reload =
      host->reload_graph(GraphSessionId{"missing_graph"}, reload_path.string());
  EXPECT_FALSE(missing_reload.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_reload.status),
            GraphErrc::NotFound);

  const auto missing_file_path =
      temp.root() / "source" / "missing_reload_graph.yaml";
  auto io_reload = host->reload_graph(session, missing_file_path.string());
  EXPECT_FALSE(io_reload.status.ok);
  EXPECT_EQ(checked_graph_error_code(io_reload.status), GraphErrc::Io);

  auto after_io_reload = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(after_io_reload.status.ok) << after_io_reload.status.message;
  EXPECT_EQ(after_io_reload.value.parameters.at("width"), "11");

  const auto invalid_yaml_path =
      temp.root() / "source" / "invalid_reload_graph.yaml";
  {
    std::ofstream invalid_yaml(invalid_yaml_path);
    invalid_yaml << "not: a sequence\n";
  }
  auto invalid_reload = host->reload_graph(session, invalid_yaml_path.string());
  EXPECT_FALSE(invalid_reload.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_reload.status),
            GraphErrc::InvalidYaml);

  auto after_invalid_reload = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(after_invalid_reload.status.ok)
      << after_invalid_reload.status.message;
  EXPECT_EQ(after_invalid_reload.value.parameters.at("width"), "11");

  auto clear = host->clear_graph(session);
  ASSERT_TRUE(clear.status.ok) << clear.status.message;

  auto ids = host->list_node_ids(session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  EXPECT_TRUE(ids.value.empty());
}

/**
 * @brief Distinguishes graph-session absence from destination IO failures.
 *
 * @return Nothing; GoogleTest assertions report status-category mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup cannot
 *         allocate or create its deterministic graph inputs.
 * @note Missing and already-closed sessions are lifecycle misses, while an
 *       existing session still reports an unwritable destination as IO.
 */
TEST(EmbeddedHostAdapter,
     SaveGraphDistinguishesMissingSessionsFromDestinationIo) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_save_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto missing =
      host->save_graph(GraphSessionId{"missing_save_graph"},
                       (temp.root() / "missing_save_graph.yaml").string());
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "save_status_graph");
  const auto unwritable = host->save_graph(
      session, (temp.root() / "missing_parent" / "graph.yaml").string());
  EXPECT_FALSE(unwritable.status.ok);
  EXPECT_EQ(checked_graph_error_code(unwritable.status), GraphErrc::Io);

  const VoidResult closed = host->close_graph(session);
  ASSERT_TRUE(closed.status.ok) << closed.status.message;
  const auto after_close = host->save_graph(
      session, (temp.root() / "closed_save_graph.yaml").string());
  EXPECT_FALSE(after_close.status.ok);
  EXPECT_EQ(checked_graph_error_code(after_close.status), GraphErrc::NotFound);
}

/**
 * @brief Distinguishes absent node-YAML targets from malformed replacements.
 *
 * @return Nothing; GoogleTest assertions report status-category mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup cannot
 *         allocate or create its deterministic graph inputs.
 * @note Session and node lookup are part of the requested mutation; only YAML
 *       parsing or graph validation for an existing target is InvalidYaml.
 */
TEST(EmbeddedHostAdapter,
     SetNodeYamlDistinguishesAbsentTargetsFromInvalidYaml) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_set_yaml_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const std::string valid_yaml =
      replacement_node_yaml("valid_replacement", 8, 3);
  const auto missing_session = host->set_node_yaml(
      GraphSessionId{"missing_set_yaml_graph"}, NodeId{1}, valid_yaml);
  EXPECT_FALSE(missing_session.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session.status),
            GraphErrc::NotFound);
  const auto missing_session_invalid_yaml = host->set_node_yaml(
      GraphSessionId{"missing_set_yaml_graph"}, NodeId{1}, "[");
  EXPECT_FALSE(missing_session_invalid_yaml.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_invalid_yaml.status),
            GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "set_yaml_status_graph");
  const auto missing_node =
      host->set_node_yaml(session, NodeId{99}, valid_yaml);
  EXPECT_FALSE(missing_node.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node.status), GraphErrc::NotFound);
  const auto missing_node_invalid_yaml =
      host->set_node_yaml(session, NodeId{99}, "[");
  EXPECT_FALSE(missing_node_invalid_yaml.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_invalid_yaml.status),
            GraphErrc::NotFound);

  const auto invalid_yaml = host->set_node_yaml(session, NodeId{1}, "[");
  EXPECT_FALSE(invalid_yaml.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_yaml.status),
            GraphErrc::InvalidYaml);

  const std::string invalid_topology =
      "id: 1\n"
      "name: invalid_topology\n"
      "type: host_adapter_test\n"
      "subtype: source\n"
      "image_inputs:\n"
      "  - from_node_id: 99\n";
  const auto invalid_topology_yaml =
      host->set_node_yaml(session, NodeId{1}, invalid_topology);
  EXPECT_FALSE(invalid_topology_yaml.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_topology_yaml.status),
            GraphErrc::InvalidYaml);

  const VoidResult closed = host->close_graph(session);
  ASSERT_TRUE(closed.status.ok) << closed.status.message;
  const auto after_close = host->set_node_yaml(session, NodeId{1}, valid_yaml);
  EXPECT_FALSE(after_close.status.ok);
  EXPECT_EQ(checked_graph_error_code(after_close.status), GraphErrc::NotFound);
}

/**
 * @brief Projects public rectangles while preserving target/parameter error
 *        categories in both directions.
 *
 * @return Nothing; GoogleTest assertions report ROI or status mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup cannot
 *         allocate or create its deterministic graph inputs.
 * @note Missing sessions/nodes are NotFound; empty ROIs and unreachable paths
 *       for existing nodes remain InvalidParameter, and closed sessions follow
 *       lifecycle absence.
 */
TEST(EmbeddedHostAdapter, RoiProjectionUsesPublicPixelRectValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_roi_projection_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "roi_graph.yaml";
  write_host_adapter_roi_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"roi_projection"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();

  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const PixelRect roi{1, 2, 3, 2};
  auto projected =
      host->project_roi(request.session, NodeId{1}, roi, NodeId{2});
  ASSERT_TRUE(projected.status.ok) << projected.status.message;
  EXPECT_EQ(projected.value.x, roi.x);
  EXPECT_EQ(projected.value.y, roi.y);
  EXPECT_EQ(projected.value.width, roi.width);
  EXPECT_EQ(projected.value.height, roi.height);

  auto back_projected =
      host->project_roi_backward(request.session, NodeId{2}, roi, NodeId{1});
  ASSERT_TRUE(back_projected.status.ok) << back_projected.status.message;
  EXPECT_EQ(back_projected.value.x, roi.x);
  EXPECT_EQ(back_projected.value.y, roi.y);
  EXPECT_EQ(back_projected.value.width, roi.width);
  EXPECT_EQ(back_projected.value.height, roi.height);

  auto missing_target =
      host->project_roi(request.session, NodeId{1}, roi, NodeId{99});
  EXPECT_FALSE(missing_target.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_target.status),
            GraphErrc::NotFound);

  auto missing_source =
      host->project_roi_backward(request.session, NodeId{2}, roi, NodeId{99});
  EXPECT_FALSE(missing_source.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_source.status),
            GraphErrc::NotFound);

  const PixelRect empty_roi{1, 2, 0, 2};
  auto invalid_forward =
      host->project_roi(request.session, NodeId{1}, empty_roi, NodeId{2});
  EXPECT_FALSE(invalid_forward.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_forward.status),
            GraphErrc::InvalidParameter);
  auto invalid_backward = host->project_roi_backward(request.session, NodeId{2},
                                                     empty_roi, NodeId{1});
  EXPECT_FALSE(invalid_backward.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_backward.status),
            GraphErrc::InvalidParameter);

  auto unreachable_forward =
      host->project_roi(request.session, NodeId{2}, roi, NodeId{1});
  EXPECT_FALSE(unreachable_forward.status.ok);
  EXPECT_EQ(checked_graph_error_code(unreachable_forward.status),
            GraphErrc::InvalidParameter);
  auto unreachable_backward =
      host->project_roi_backward(request.session, NodeId{1}, roi, NodeId{2});
  EXPECT_FALSE(unreachable_backward.status.ok);
  EXPECT_EQ(checked_graph_error_code(unreachable_backward.status),
            GraphErrc::InvalidParameter);

  const GraphSessionId missing_session{"missing_roi_session"};
  auto missing_session_forward =
      host->project_roi(missing_session, NodeId{1}, roi, NodeId{2});
  EXPECT_FALSE(missing_session_forward.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_forward.status),
            GraphErrc::NotFound);
  auto missing_session_backward =
      host->project_roi_backward(missing_session, NodeId{2}, roi, NodeId{1});
  EXPECT_FALSE(missing_session_backward.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_backward.status),
            GraphErrc::NotFound);

  const VoidResult closed = host->close_graph(request.session);
  ASSERT_TRUE(closed.status.ok) << closed.status.message;
  auto after_close_forward =
      host->project_roi(request.session, NodeId{1}, roi, NodeId{2});
  EXPECT_FALSE(after_close_forward.status.ok);
  EXPECT_EQ(checked_graph_error_code(after_close_forward.status),
            GraphErrc::NotFound);
  auto after_close_backward =
      host->project_roi_backward(request.session, NodeId{2}, roi, NodeId{1});
  EXPECT_FALSE(after_close_backward.status.ok);
  EXPECT_EQ(checked_graph_error_code(after_close_backward.status),
            GraphErrc::NotFound);
}

#if defined(PHOTOSPIDER_INTERNAL_REQUIRED_TARGET_TESTING)
/**
 * @brief Prevents clear from entering between node lookup and replacement.
 *
 * @return Nothing; GoogleTest assertions report graph-state ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The target gate blocks only after the existing node is resolved while
 *       its GraphStateExecutor callback remains active. Clear must stay
 * pending, then run second.
 */
TEST(EmbeddedHostAdapter, SetNodeYamlLookupAndMutationExcludeConcurrentClear) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_set_yaml_clear_race_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "set_yaml_clear_race_graph");

  const std::string replacement =
      replacement_node_yaml("replacement_before_clear", 10, 6);
  const auto race = run_required_target_race(
      testing::RequiredTargetTestEvent::SetNodeDocumentTargetResolved,
      "set_node_yaml", "clear_graph",
      [&] { return host->set_node_yaml(session, NodeId{1}, replacement); },
      [&] { return host->clear_graph(session); });

  EXPECT_TRUE(race.target.status.ok) << race.target.status.message;
  EXPECT_TRUE(race.competitor.status.ok) << race.competitor.status.message;
  const auto ids = host->list_node_ids(session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  EXPECT_TRUE(ids.value.empty());
  const VoidResult close = host->close_graph(session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

/**
 * @brief Prevents reload from entering between node lookup and replacement.
 *
 * @return Nothing; GoogleTest assertions report graph-state ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Replacement completes first under the required-target gate; reload
 * then replaces the graph, so the final public snapshot must be reload data.
 */
TEST(EmbeddedHostAdapter, SetNodeYamlLookupAndMutationExcludeConcurrentReload) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_set_yaml_reload_race_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "set_yaml_reload_race_graph");
  const auto reload_path = temp.root() / "source" / "reload_after_set.yaml";
  {
    std::ofstream reload_yaml(reload_path);
    reload_yaml << "- id: 1\n"
                << "  name: reload_after_set\n"
                << "  type: host_adapter_test\n"
                << "  subtype: source\n"
                << "  parameters:\n"
                << "    width: 12\n"
                << "    height: 7\n";
  }

  const std::string replacement =
      replacement_node_yaml("replacement_before_reload", 10, 6);
  const auto race = run_required_target_race(
      testing::RequiredTargetTestEvent::SetNodeDocumentTargetResolved,
      "set_node_yaml", "reload_graph",
      [&] { return host->set_node_yaml(session, NodeId{1}, replacement); },
      [&] { return host->reload_graph(session, reload_path.string()); });

  EXPECT_TRUE(race.target.status.ok) << race.target.status.message;
  EXPECT_TRUE(race.competitor.status.ok) << race.competitor.status.message;
  const auto node = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node.status.ok) << node.status.message;
  EXPECT_EQ(node.value.name, "reload_after_set");
  EXPECT_EQ(node.value.parameters.at("width"), "12");
  const VoidResult close = host->close_graph(session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

/**
 * @brief Prevents clear from entering after forward ROI endpoint resolution.
 *
 * @return Nothing; GoogleTest assertions report graph-state ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The forward projection remains inside one graph-state work item from
 * endpoint lookup through propagation; clear must execute only afterward.
 */
TEST(EmbeddedHostAdapter, ForwardRoiLookupAndProjectionExcludeConcurrentClear) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_forward_roi_clear_race_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  GraphLoadRequest load;
  load.session = GraphSessionId{"forward_roi_clear_race_graph"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = (temp.root() / "source" / "roi_graph.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_roi_graph(load.yaml_path);
  const auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const PixelRect expected{1, 1, 3, 2};
  const auto race = run_required_target_race(
      testing::RequiredTargetTestEvent::ForwardRoiEndpointsResolved,
      "project_roi", "clear_graph",
      [&] {
        return host->project_roi(load.session, NodeId{1}, expected, NodeId{2});
      },
      [&] { return host->clear_graph(load.session); });

  EXPECT_TRUE(race.target.status.ok) << race.target.status.message;
  EXPECT_EQ(race.target.value.x, expected.x);
  EXPECT_EQ(race.target.value.y, expected.y);
  EXPECT_EQ(race.target.value.width, expected.width);
  EXPECT_EQ(race.target.value.height, expected.height);
  EXPECT_TRUE(race.competitor.status.ok) << race.competitor.status.message;
  const auto ids = host->list_node_ids(load.session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  EXPECT_TRUE(ids.value.empty());
  const VoidResult close = host->close_graph(load.session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

/**
 * @brief Prevents reload from entering after forward ROI endpoint resolution.
 *
 * @return Nothing; GoogleTest assertions report graph-state ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Projection must finish on the original two-node graph before reload
 * publishes its single-node replacement as the final public state.
 */
TEST(EmbeddedHostAdapter,
     ForwardRoiLookupAndProjectionExcludeConcurrentReload) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_forward_roi_reload_race_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  GraphLoadRequest load;
  load.session = GraphSessionId{"forward_roi_reload_race_graph"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = (temp.root() / "source" / "roi_graph.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_roi_graph(load.yaml_path);
  const auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  const auto reload_path = temp.root() / "source" / "roi_reload.yaml";
  {
    std::ofstream reload_yaml(reload_path);
    reload_yaml << "- id: 1\n"
                << "  name: forward_reload_final\n"
                << "  type: host_adapter_test\n"
                << "  subtype: source\n";
  }

  const PixelRect expected{1, 1, 3, 2};
  const auto race = run_required_target_race(
      testing::RequiredTargetTestEvent::ForwardRoiEndpointsResolved,
      "project_roi", "reload_graph",
      [&] {
        return host->project_roi(load.session, NodeId{1}, expected, NodeId{2});
      },
      [&] { return host->reload_graph(load.session, reload_path.string()); });

  EXPECT_TRUE(race.target.status.ok) << race.target.status.message;
  EXPECT_EQ(race.target.value.x, expected.x);
  EXPECT_EQ(race.target.value.y, expected.y);
  EXPECT_EQ(race.target.value.width, expected.width);
  EXPECT_EQ(race.target.value.height, expected.height);
  EXPECT_TRUE(race.competitor.status.ok) << race.competitor.status.message;
  const auto ids = host->list_node_ids(load.session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  ASSERT_EQ(ids.value.size(), 1u);
  EXPECT_EQ(ids.value.front().value, 1);
  const auto node = host->inspect_node(load.session, NodeId{1});
  ASSERT_TRUE(node.status.ok) << node.status.message;
  EXPECT_EQ(node.value.name, "forward_reload_final");
  const VoidResult close = host->close_graph(load.session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

/**
 * @brief Prevents clear from entering after backward ROI endpoint resolution.
 *
 * @return Nothing; GoogleTest assertions report graph-state ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Backward propagation must retain the resolved graph until its public
 * result is produced; clear runs second and leaves the loaded session empty.
 */
TEST(EmbeddedHostAdapter,
     BackwardRoiLookupAndProjectionExcludeConcurrentClear) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_backward_roi_clear_race_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  GraphLoadRequest load;
  load.session = GraphSessionId{"backward_roi_clear_race_graph"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = (temp.root() / "source" / "roi_graph.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_roi_graph(load.yaml_path);
  const auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const PixelRect expected{1, 1, 3, 2};
  const auto race = run_required_target_race(
      testing::RequiredTargetTestEvent::BackwardRoiEndpointsResolved,
      "project_roi_backward", "clear_graph",
      [&] {
        return host->project_roi_backward(load.session, NodeId{2}, expected,
                                          NodeId{1});
      },
      [&] { return host->clear_graph(load.session); });

  EXPECT_TRUE(race.target.status.ok) << race.target.status.message;
  EXPECT_EQ(race.target.value.x, expected.x);
  EXPECT_EQ(race.target.value.y, expected.y);
  EXPECT_EQ(race.target.value.width, expected.width);
  EXPECT_EQ(race.target.value.height, expected.height);
  EXPECT_TRUE(race.competitor.status.ok) << race.competitor.status.message;
  const auto ids = host->list_node_ids(load.session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  EXPECT_TRUE(ids.value.empty());
  const VoidResult close = host->close_graph(load.session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

/**
 * @brief Prevents reload from entering after backward ROI endpoint resolution.
 *
 * @return Nothing; GoogleTest assertions report graph-state ordering failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Backward projection observes the original topology, then reload
 * publishes its single-node graph as the final externally visible state.
 */
TEST(EmbeddedHostAdapter,
     BackwardRoiLookupAndProjectionExcludeConcurrentReload) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_backward_roi_reload_race_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  GraphLoadRequest load;
  load.session = GraphSessionId{"backward_roi_reload_race_graph"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = (temp.root() / "source" / "roi_graph.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_roi_graph(load.yaml_path);
  const auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  const auto reload_path = temp.root() / "source" / "roi_reload.yaml";
  {
    std::ofstream reload_yaml(reload_path);
    reload_yaml << "- id: 1\n"
                << "  name: backward_reload_final\n"
                << "  type: host_adapter_test\n"
                << "  subtype: source\n";
  }

  const PixelRect expected{1, 1, 3, 2};
  const auto race = run_required_target_race(
      testing::RequiredTargetTestEvent::BackwardRoiEndpointsResolved,
      "project_roi_backward", "reload_graph",
      [&] {
        return host->project_roi_backward(load.session, NodeId{2}, expected,
                                          NodeId{1});
      },
      [&] { return host->reload_graph(load.session, reload_path.string()); });

  EXPECT_TRUE(race.target.status.ok) << race.target.status.message;
  EXPECT_EQ(race.target.value.x, expected.x);
  EXPECT_EQ(race.target.value.y, expected.y);
  EXPECT_EQ(race.target.value.width, expected.width);
  EXPECT_EQ(race.target.value.height, expected.height);
  EXPECT_TRUE(race.competitor.status.ok) << race.competitor.status.message;
  const auto ids = host->list_node_ids(load.session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  ASSERT_EQ(ids.value.size(), 1u);
  EXPECT_EQ(ids.value.front().value, 1);
  const auto node = host->inspect_node(load.session, NodeId{1});
  ASSERT_TRUE(node.status.ok) << node.status.message;
  EXPECT_EQ(node.value.name, "backward_reload_final");
  const VoidResult close = host->close_graph(load.session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}
#endif

/**
 * @brief Verifies the public Host dirty path through planning and execution.
 *
 * @return Nothing; GoogleTest assertions report ROI, plan, execution, commit,
 *         and trace failures.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The test submits PixelRect at the Host boundary, observes the same
 *       kernel-native geometry in dirty/planning snapshots, requires a real
 *       NodeExecutor tiled callback, and distinguishes the committed dirty HP
 *       output from the pre-request authoritative cache.
 */
TEST(EmbeddedHostAdapter,
     DirtyComputeCarriesNativeRoiThroughPlanningAndExecution) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_snapshot_details_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "dirty_roi_graph.yaml";
  write_host_adapter_offset_roi_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"dirty_snapshot_details"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();

  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest full_request;
  full_request.session = request.session;
  full_request.node = NodeId{2};
  full_request.cache.precision = "fp32";
  g_offset_tiled_output_value.store(3, std::memory_order_relaxed);
  g_offset_tiled_invocation_count.store(0, std::memory_order_relaxed);
  auto initial_compute = host->compute_and_get_image(full_request);
  ASSERT_TRUE(initial_compute.status.ok) << initial_compute.status.message;
  ASSERT_NE(initial_compute.value.data, nullptr);
  EXPECT_GT(g_offset_tiled_invocation_count.load(std::memory_order_relaxed), 0);
  const cv::Mat initial_image = toCvMat(initial_compute.value);
  ASSERT_EQ(initial_image.type(), CV_32FC1);
  EXPECT_FLOAT_EQ(initial_image.at<float>(10, 70), 3.0f);
  auto initial_events =
      host->drain_compute_events(request.session, kComputeEventDrainMaxLimit);
  ASSERT_TRUE(initial_events.status.ok) << initial_events.status.message;

  HostComputeRequest dirty_request = full_request;
  dirty_request.intent = ComputeIntent::GlobalHighPrecision;
  dirty_request.dirty_roi = PixelRect{70, 10, 20, 20};
  dirty_request.execution.parallel = true;
  g_offset_tiled_output_value.store(11, std::memory_order_relaxed);
  g_offset_tiled_invocation_count.store(0, std::memory_order_relaxed);
  auto dirty_compute = host->compute_and_get_image(dirty_request);
  ASSERT_TRUE(dirty_compute.status.ok) << dirty_compute.status.message;
  ASSERT_NE(dirty_compute.value.data, nullptr);
  EXPECT_EQ(g_offset_tiled_invocation_count.load(std::memory_order_relaxed),
            16);
  const cv::Mat committed_image = toCvMat(dirty_compute.value);
  ASSERT_EQ(committed_image.type(), CV_32FC1);
  EXPECT_FLOAT_EQ(committed_image.at<float>(10, 70), 11.0f);
  EXPECT_FLOAT_EQ(committed_image.at<float>(10, 10), 3.0f);

  auto planning = host->compute_planning_snapshot(request.session);
  ASSERT_TRUE(planning.status.ok) << planning.status.message;
  ASSERT_TRUE(planning.value.has_value());
  EXPECT_EQ(planning.value->intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(planning.value->target_node.value, 2);
  EXPECT_EQ(planning.value->planned_node_count, 2u);
  EXPECT_EQ(planning.value->tile_task_count, 128u);
  EXPECT_EQ(planning.value->monolithic_task_count, 1u);
  EXPECT_EQ(planning.value->active_task_count, 17u);
  EXPECT_EQ(planning.value->dirty_source_task_count, 1u);
  EXPECT_EQ(planning.value->downstream_task_count, 16u);
  const auto planned_tiled_downstream = std::find_if(
      planning.value->task_sample.begin(), planning.value->task_sample.end(),
      [](const ComputePlanningTaskSnapshot& task) {
        return task.node.value == 2 && task.kind == "tile" &&
               task.output_roi.x == 64 && task.output_roi.y == 0;
      });
  ASSERT_NE(planned_tiled_downstream, planning.value->task_sample.end());
  EXPECT_EQ(planned_tiled_downstream->tile_size, 16);
  EXPECT_EQ(planned_tiled_downstream->output_roi.x, 64);
  EXPECT_EQ(planned_tiled_downstream->output_roi.y, 0);
  EXPECT_EQ(planned_tiled_downstream->output_roi.width, 16);
  EXPECT_EQ(planned_tiled_downstream->output_roi.height, 16);

  auto snapshot = host->dirty_region_snapshot(request.session);
  ASSERT_TRUE(snapshot.status.ok) << snapshot.status.message;
  EXPECT_FALSE(snapshot.value.dirty_monolithic_nodes.empty());
  EXPECT_FALSE(snapshot.value.dirty_tiles.empty());
  EXPECT_FALSE(snapshot.value.edge_mappings.empty());

  const auto downstream_tile = std::find_if(
      snapshot.value.dirty_tiles.begin(), snapshot.value.dirty_tiles.end(),
      [](const DirtyTileSnapshot& tile) {
        return tile.node.value == 2 &&
               tile.domain == DirtyDomain::HighPrecision;
      });
  ASSERT_NE(downstream_tile, snapshot.value.dirty_tiles.end());
  EXPECT_EQ(downstream_tile->tile_x, 1);
  EXPECT_EQ(downstream_tile->tile_y, 0);
  EXPECT_EQ(downstream_tile->tile_size, 64);
  EXPECT_EQ(downstream_tile->pixel_roi.x, 64);
  EXPECT_EQ(downstream_tile->pixel_roi.y, 0);
  EXPECT_EQ(downstream_tile->pixel_roi.width, 64);
  EXPECT_EQ(downstream_tile->pixel_roi.height, 64);
  EXPECT_EQ(std::count_if(snapshot.value.dirty_monolithic_nodes.begin(),
                          snapshot.value.dirty_monolithic_nodes.end(),
                          [](const DirtyMonolithicRegionSnapshot& region) {
                            return region.node.value == 2 &&
                                   region.domain == DirtyDomain::HighPrecision;
                          }),
            0);

  const auto edge_mapping = std::find_if(
      snapshot.value.edge_mappings.begin(), snapshot.value.edge_mappings.end(),
      [](const DirtyEdgeMappingSnapshot& mapping) {
        return mapping.from_node.value == 1 && mapping.to_node.value == 2 &&
               mapping.domain == DirtyDomain::HighPrecision;
      });
  ASSERT_NE(edge_mapping, snapshot.value.edge_mappings.end());
  EXPECT_EQ(edge_mapping->direction, DirtyEdgeDirection::BackwardDemand);
  EXPECT_EQ(edge_mapping->from_roi.x, 64);
  EXPECT_EQ(edge_mapping->from_roi.y, 0);
  EXPECT_EQ(edge_mapping->from_roi.width, 128);
  EXPECT_EQ(edge_mapping->from_roi.height, 64);
  EXPECT_EQ(edge_mapping->to_roi.x, 64);
  EXPECT_EQ(edge_mapping->to_roi.y, 0);
  EXPECT_EQ(edge_mapping->to_roi.width, 64);
  EXPECT_EQ(edge_mapping->to_roi.height, 64);

  auto trace =
      host->scheduler_trace(request.session, 0, kSchedulerTraceMaxLimit);
  ASSERT_TRUE(trace.status.ok) << trace.status.message;
  const bool executed_dirty_source = std::any_of(
      trace.value.events.begin(), trace.value.events.end(),
      [](const SchedulerTraceEventSnapshot& event) {
        return event.node.value == 1 &&
               event.action == HostSchedulerTraceAction::ExecuteDirtySource;
      });
  const bool executed_dirty_downstream = std::any_of(
      trace.value.events.begin(), trace.value.events.end(),
      [](const SchedulerTraceEventSnapshot& event) {
        return event.node.value == 2 &&
               event.action ==
                   HostSchedulerTraceAction::ExecuteDirtyDownstreamNode;
      });
  const bool executed_dirty_downstream_tile = std::any_of(
      trace.value.events.begin(), trace.value.events.end(),
      [](const SchedulerTraceEventSnapshot& event) {
        return event.node.value == 2 &&
               event.action ==
                   HostSchedulerTraceAction::ExecuteDirtyDownstreamTile;
      });
  EXPECT_TRUE(executed_dirty_source);
  EXPECT_TRUE(executed_dirty_downstream);
  EXPECT_TRUE(executed_dirty_downstream_tile);

  auto dirty_events =
      host->drain_compute_events(request.session, kComputeEventDrainMaxLimit);
  ASSERT_TRUE(dirty_events.status.ok) << dirty_events.status.message;
  EXPECT_TRUE(std::any_of(
      dirty_events.value.events.begin(), dirty_events.value.events.end(),
      [](const ComputeEventSnapshot& event) {
        return event.node.value == 2 && event.source == "hp_update";
      }));
}

TEST(EmbeddedHostAdapter, DirtySourceAndCacheControlsExposeFrontendStatus) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_cache_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "dirty_cache");

  HostComputeRequest request = make_compute_request(session);
  auto compute = host->compute(request);
  ASSERT_TRUE(compute.status.ok) << compute.status.message;

  const PixelRect roi{1, 1, 2, 2};
  auto begin = host->begin_dirty_source(session, NodeId{1},
                                        DirtyDomain::HighPrecision, roi);
  ASSERT_TRUE(begin.status.ok) << begin.status.message;
  ASSERT_FALSE(begin.value.sources.empty());
  EXPECT_EQ(begin.value.sources.front().node.value, 1);
  EXPECT_EQ(begin.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Updating);

  auto update = host->update_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, PixelRect{2, 2, 1, 1});
  ASSERT_TRUE(update.status.ok) << update.status.message;

  auto end =
      host->end_dirty_source(session, NodeId{1}, DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.status.ok) << end.status.message;
  ASSERT_FALSE(end.value.sources.empty());
  EXPECT_EQ(end.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Settled);

  auto cache_all = host->cache_all_nodes(session, "fp32");
  EXPECT_TRUE(cache_all.status.ok) << cache_all.status.message;

  auto sync = host->synchronize_disk_cache(session, "fp32");
  EXPECT_TRUE(sync.status.ok) << sync.status.message;

  auto clear_memory = host->clear_memory_cache(session);
  EXPECT_TRUE(clear_memory.status.ok) << clear_memory.status.message;

  auto clear_drive = host->clear_drive_cache(session);
  EXPECT_TRUE(clear_drive.status.ok) << clear_drive.status.message;

  auto clear_all = host->clear_cache(session);
  EXPECT_TRUE(clear_all.status.ok) << clear_all.status.message;

  auto free_memory = host->free_transient_memory(session);
  EXPECT_TRUE(free_memory.status.ok) << free_memory.status.message;
}

TEST(EmbeddedHostAdapter, DirtySourceFailuresPreserveStatusCodes) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "dirty_status");
  const PixelRect roi{1, 1, 2, 2};
  const PixelRect empty_roi{1, 1, 0, 2};

  auto missing_session_begin =
      host->begin_dirty_source(GraphSessionId{"missing_dirty_session"},
                               NodeId{1}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_session_begin.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_begin.status),
            GraphErrc::NotFound);

  auto missing_session_update =
      host->update_dirty_source(GraphSessionId{"missing_dirty_session"},
                                NodeId{1}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_session_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_update.status),
            GraphErrc::NotFound);

  auto missing_session_end =
      host->end_dirty_source(GraphSessionId{"missing_dirty_session"}, NodeId{1},
                             DirtyDomain::HighPrecision);
  EXPECT_FALSE(missing_session_end.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_end.status),
            GraphErrc::NotFound);

  auto missing_node_begin = host->begin_dirty_source(
      session, NodeId{99}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_node_begin.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_begin.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_begin.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_begin_error = host->last_error(session);
  EXPECT_FALSE(missing_node_begin_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_begin_error),
            GraphErrc::NotFound);

  auto missing_node_update = host->update_dirty_source(
      session, NodeId{99}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_node_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_update.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_update.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_update_error = host->last_error(session);
  EXPECT_FALSE(missing_node_update_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_update_error),
            GraphErrc::NotFound);

  auto missing_node_end =
      host->end_dirty_source(session, NodeId{99}, DirtyDomain::HighPrecision);
  EXPECT_FALSE(missing_node_end.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_end.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_end.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_end_error = host->last_error(session);
  EXPECT_FALSE(missing_node_end_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_end_error),
            GraphErrc::NotFound);

  auto invalid_begin = host->begin_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, empty_roi);
  EXPECT_FALSE(invalid_begin.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_begin.status),
            GraphErrc::InvalidParameter);
  EXPECT_NE(invalid_begin.status.message.find("Dirty source ROI is empty"),
            std::string::npos);
  auto invalid_begin_error = host->last_error(session);
  EXPECT_FALSE(invalid_begin_error.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_begin_error),
            GraphErrc::InvalidParameter);

  auto invalid_update = host->update_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, empty_roi);
  EXPECT_FALSE(invalid_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_update.status),
            GraphErrc::InvalidParameter);
  EXPECT_NE(invalid_update.status.message.find("Dirty source ROI is empty"),
            std::string::npos);
  auto invalid_update_error = host->last_error(session);
  EXPECT_FALSE(invalid_update_error.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_update_error),
            GraphErrc::InvalidParameter);
}

TEST(EmbeddedHostAdapter, SchedulerScanLoadAndPluginUnloadUseStatusValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_scheduler_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto scheduler_dir = temp.root() / "schedulers";
  std::filesystem::create_directories(scheduler_dir);

  auto scan = host->scheduler_scan({scheduler_dir.string()});
  ASSERT_TRUE(scan.status.ok) << scan.status.message;

  auto bad_load =
      host->scheduler_load((temp.root() / "missing_scheduler.so").string());
  EXPECT_FALSE(bad_load.status.ok);
  EXPECT_EQ(checked_graph_error_code(bad_load.status), GraphErrc::Io);

  auto loaded = host->scheduler_loaded_plugins();
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const auto plugin_dir = lifecycle_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(plugin_dir))
      << "lifecycle op plugin directory was not built: " << plugin_dir;

  auto plugin_report = host->plugins_load_report({plugin_dir.string()});
  ASSERT_TRUE(plugin_report.status.ok) << plugin_report.status.message;
  EXPECT_EQ(plugin_report.value.loaded, 1);
  EXPECT_TRUE(plugin_report.value.errors.empty());
  EXPECT_TRUE(
      contains_string(plugin_report.value.new_op_keys, "plugin_lifecycle:op"));

  auto plugin_sources = host->ops_combined_sources();
  ASSERT_TRUE(plugin_sources.status.ok) << plugin_sources.status.message;
  ASSERT_NE(plugin_sources.value.find("plugin_lifecycle:op"),
            plugin_sources.value.end());

  auto unload_ops = host->plugins_unload_all();
  ASSERT_TRUE(unload_ops.status.ok) << unload_ops.status.message;
  EXPECT_GE(unload_ops.value, 1);

  auto plugin_sources_after_unload = host->ops_combined_sources();
  ASSERT_TRUE(plugin_sources_after_unload.status.ok)
      << plugin_sources_after_unload.status.message;
  EXPECT_EQ(plugin_sources_after_unload.value.count("plugin_lifecycle:op"), 0u);

  auto status_only_load = host->plugins_load({plugin_dir.string()});
  ASSERT_TRUE(status_only_load.status.ok) << status_only_load.status.message;

  auto unload_status_only = host->plugins_unload_all();
  ASSERT_TRUE(unload_status_only.status.ok)
      << unload_status_only.status.message;
  EXPECT_GE(unload_status_only.value, 1);

  auto empty_scan_plugins =
      host->plugins_load({(temp.root() / "missing_plugins").string()});
  EXPECT_TRUE(empty_scan_plugins.status.ok)
      << empty_scan_plugins.status.message;
}

/**
 * @brief Exercises external operation and scheduler DSOs through embedded Host
 * compute.
 *
 * @return Nothing; GoogleTest records assertion failures.
 * @throws Nothing when plugin discovery, graph ownership, scheduler selection,
 *         and production compute behavior match their public status contracts;
 *         GoogleTest records any mismatch.
 * @note The lifecycle operation and destroy-count scheduler are both loaded
 *       through public Host methods. Parallel HP compute must therefore cross
 *       the real v2 operation adapter and inherited scheduler task runtime.
 */
TEST(EmbeddedHostAdapter,
     ExternalOperationAndSchedulerPluginsDriveParallelCompute) {
  ScopedTempDir temp("photospider_host_external_plugin_compute_test");
  ScopedSchedulerPluginCleanup scheduler_cleanup;
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  ScopedHostPluginCleanup operation_cleanup(*host);

  const std::filesystem::path operation_dir = lifecycle_plugin_dir();
  const std::filesystem::path scheduler_path =
      destroy_count_scheduler_plugin_path();
  ASSERT_TRUE(std::filesystem::is_directory(operation_dir));
  ASSERT_TRUE(std::filesystem::is_regular_file(scheduler_path));

  const auto operation_report =
      host->plugins_load_report({operation_dir.string()});
  ASSERT_TRUE(operation_report.status.ok) << operation_report.status.message;
  ASSERT_TRUE(contains_string(operation_report.value.new_op_keys,
                              "plugin_lifecycle:op"));
  const VoidResult scheduler_load =
      host->scheduler_load(scheduler_path.string());
  ASSERT_TRUE(scheduler_load.status.ok) << scheduler_load.status.message;

  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = kDestroyCountSchedulerType;
  scheduler_config.rt_type = "serial_debug";
  scheduler_config.worker_count = 1;
  const VoidResult configured =
      host->configure_scheduler_defaults(scheduler_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  GraphLoadRequest load;
  load.session = GraphSessionId{"external_plugin_compute"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = (temp.root() / "source" / "external.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_lifecycle_plugin_graph(load.yaml_path);
  const auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const auto scheduler =
      host->scheduler_info(load.session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(scheduler.status.ok) << scheduler.status.message;
  EXPECT_EQ(scheduler.value.scheduler_name, kDestroyCountSchedulerType);

  HostComputeRequest request = make_compute_request(load.session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  const VoidResult computed = host->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;

  const auto node = host->inspect_node(load.session, NodeId{1});
  ASSERT_TRUE(node.status.ok) << node.status.message;
  ASSERT_TRUE(node.value.space.has_value());
  EXPECT_EQ(node.value.space->absolute_roi.width, 11);
  EXPECT_EQ(node.value.space->absolute_roi.height, 7);

  const VoidResult closed = host->close_graph(load.session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
}

/**
 * @brief Proves every embedded Host shares one process plugin owner.
 *
 * @throws Nothing when public Host status mapping and fixture IO succeed.
 * @note One Host loads P1, another loads P2 and executes it, both loading Hosts
 *       are destroyed, and a third Host performs the global unload. The
 *       surviving and newly created Hosts must observe the same state.
 */
TEST(EmbeddedHostAdapter,
     OperationPluginsAreProcessGlobalAcrossHostDestructionAndUnload) {
  ScopedTempDir temp("photospider_host_process_plugin_owner_test");
  auto observer = create_embedded_host();
  ASSERT_NE(observer, nullptr);
  ScopedHostPluginCleanup cleanup(*observer);

  const auto original_dir = lifecycle_plugin_dir();
  const auto replacement_dir = override_lifecycle_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(original_dir));
  ASSERT_TRUE(std::filesystem::exists(replacement_dir));

  auto original_loader = create_embedded_host();
  ASSERT_NE(original_loader, nullptr);
  const auto original_report =
      original_loader->plugins_load_report({original_dir.string()});
  ASSERT_TRUE(original_report.status.ok) << original_report.status.message;
  ASSERT_EQ(original_report.value.loaded, 1);

  auto observer_sources = observer->ops_sources();
  ASSERT_TRUE(observer_sources.status.ok) << observer_sources.status.message;
  ASSERT_EQ(observer_sources.value.count("plugin_lifecycle:op"), 1u);

  GraphLoadRequest load_request;
  load_request.session = GraphSessionId{"process_plugin_graph"};
  load_request.root_dir = (temp.root() / "sessions").string();
  load_request.yaml_path = (temp.root() / "source" / "plugin.yaml").string();
  load_request.cache_root_dir = (temp.root() / "cache").string();
  write_lifecycle_plugin_graph(load_request.yaml_path);
  const auto loaded = observer->load_graph(load_request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest request = make_compute_request(load_request.session);
  request.cache.force_recache = true;
  auto computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto original_view = observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(original_view.status.ok) << original_view.status.message;
  ASSERT_TRUE(original_view.value.space.has_value());
  EXPECT_EQ(original_view.value.space->absolute_roi.width, 11);
  EXPECT_EQ(original_view.value.space->absolute_roi.height, 7);

  auto replacement_loader = create_embedded_host();
  ASSERT_NE(replacement_loader, nullptr);
  const auto replacement_report =
      replacement_loader->plugins_load_report({replacement_dir.string()});
  ASSERT_TRUE(replacement_report.status.ok)
      << replacement_report.status.message;
  ASSERT_EQ(replacement_report.value.loaded, 1);

  const auto repeated_seed = observer->seed_builtin_ops();
  ASSERT_TRUE(repeated_seed.status.ok) << repeated_seed.status.message;
  computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto replacement_view =
      observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(replacement_view.status.ok) << replacement_view.status.message;
  ASSERT_TRUE(replacement_view.value.space.has_value());
  EXPECT_EQ(replacement_view.value.space->absolute_roi.width, 22);
  EXPECT_EQ(replacement_view.value.space->absolute_roi.height, 9);

  original_loader.reset();
  replacement_loader.reset();
  computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto after_loader_destruction =
      observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(after_loader_destruction.status.ok)
      << after_loader_destruction.status.message;
  ASSERT_TRUE(after_loader_destruction.value.space.has_value());
  EXPECT_EQ(after_loader_destruction.value.space->absolute_roi.width, 22);
  EXPECT_EQ(after_loader_destruction.value.space->absolute_roi.height, 9);

  auto unloading_host = create_embedded_host();
  ASSERT_NE(unloading_host, nullptr);
  const auto unloaded = unloading_host->plugins_unload_all();
  ASSERT_TRUE(unloaded.status.ok) << unloaded.status.message;
  EXPECT_EQ(unloaded.value, 2);

  observer_sources = observer->ops_sources();
  ASSERT_TRUE(observer_sources.status.ok) << observer_sources.status.message;
  EXPECT_EQ(observer_sources.value.count("plugin_lifecycle:op"), 0u);
  const auto unloading_sources = unloading_host->ops_sources();
  ASSERT_TRUE(unloading_sources.status.ok) << unloading_sources.status.message;
  EXPECT_EQ(unloading_sources.value.count("plugin_lifecycle:op"), 0u);

  auto fresh_host = create_embedded_host();
  ASSERT_NE(fresh_host, nullptr);
  const auto fresh_sources = fresh_host->ops_sources();
  ASSERT_TRUE(fresh_sources.status.ok) << fresh_sources.status.message;
  EXPECT_EQ(fresh_sources.value.count("plugin_lifecycle:op"), 0u);
}

}  // namespace
}  // namespace ps
