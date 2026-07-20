#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "compute/dirty_region_snapshot.hpp"
#include "compute/execution_service.hpp"
#include "core/parameter_value_text.hpp"
#include "host/embedded_host_dependencies.hpp"
#include "photospider/host/host.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "providers/configured_persistence_adapters.hpp"
#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief BUILD_TESTING lifecycle-coordination events exposed to Host tests.
 *
 * @throws Nothing.
 * @note Values are published synchronously while the embedded Host lifecycle
 *       mutex remains held. They exist only in the test-enabled build and do
 *       not enter the public Host ABI or cross a process boundary.
 */
enum class EmbeddedLifecycleTestEvent {
  /** @brief One caller has claimed the session close marker. */
  MarkerClaimed,
  /** @brief A duplicate caller is about to wait for the marker to clear. */
  DuplicateAboutToWait,
  /** @brief One synchronous session operation has entered admission. */
  SessionOperationAdmitted,
};

/**
 * @brief Non-allocating callback installed by deterministic close tests.
 *
 * @throws Nothing for value construction and scalar access.
 * @note The hook borrows `context`; the installing test serializes hook
 *       replacement and keeps that context alive until the hook is removed.
 *       Notification runs synchronously under the Host lifecycle mutex, so the
 *       callback must not re-enter the same Host lifecycle path.
 */
struct EmbeddedLifecycleTestHook {
  /** @brief Borrowed test context that outlives the installed hook. */
  void* context = nullptr;
  /**
   * @brief Publishes one lifecycle event while lifecycle_mutex_ remains held.
   * @param context Borrowed context supplied by the test.
   * @param event Coordination point that has just been reached.
   * @return Nothing.
   * @throws Nothing; throwing from a test callback terminates the process.
   */
  void (*notify)(void* context,
                 EmbeddedLifecycleTestEvent event) noexcept = nullptr;
};

/**
 * @brief Borrowed lifecycle-hook pointer stored by the atomic test seam.
 * @throws Nothing for alias use.
 */
using EmbeddedLifecycleHookPtr = const EmbeddedLifecycleTestHook*;

/**
 * @brief Process-local borrowed lifecycle hook used by serialized tests.
 * @throws Nothing.
 * @note Atomic publication does not transfer ownership. Tests remove the hook
 *       before destroying the referenced callback value or context.
 */
std::atomic<EmbeddedLifecycleHookPtr> g_embedded_lifecycle_test_hook{nullptr};

/**
 * @brief Publishes one lifecycle event to the installed test hook.
 * @param event Coordination point reached by the Host lifecycle gate.
 * @return Nothing.
 * @throws Nothing.
 */
void notify_embedded_lifecycle_test_hook(
    EmbeddedLifecycleTestEvent event) noexcept {
  const EmbeddedLifecycleTestHook* hook =
      g_embedded_lifecycle_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->notify != nullptr) {
    hook->notify(hook->context, event);
  }
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
/**
 * @brief Synchronous Host operations covered by the admission-lifetime gate.
 * @throws Nothing for value construction and comparison.
 * @note Production builds do not compile this private test contract.
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
 * @brief Admission-lifetime checkpoints around Kernel and public translation.
 * @throws Nothing for value construction and comparison.
 */
enum class EmbeddedOperationTestPhase {
  /** @brief Admission exists and the operation has not entered Kernel. */
  BeforeKernelReady,
  /** @brief Blocker has ended; exact session admission is sampled pre-Kernel.
   */
  BeforeKernelAdmissionSnapshot,
  /** @brief Public result exists; exact admission is sampled before return. */
  AfterTranslationAdmissionSnapshot,
};

/**
 * @brief Borrowed blocking callback for deterministic admission tests.
 * @throws Nothing for aggregate construction.
 * @note The callback runs without the lifecycle or graph-state mutex. It may
 *       wait on test-owned synchronization but must not re-enter Host.
 */
struct EmbeddedOperationTestHook {
  /** @brief Borrowed test context that outlives every callback. */
  void* context = nullptr;

  /**
   * @brief Observes and optionally blocks one Host operation checkpoint.
   * @param context Borrowed context supplied by the installing test.
   * @param event Operation reaching the checkpoint.
   * @param phase Admission-lifetime phase being observed.
   * @param admission_active Whether this session has an active admission when
   *        the snapshot phase is published; ignored for BeforeKernelReady.
   * @return Nothing.
   * @throws Nothing; implementations must contain every failure.
   */
  void (*wait)(void* context, EmbeddedOperationTestEvent event,
               EmbeddedOperationTestPhase phase,
               bool admission_active) noexcept = nullptr;
};

/**
 * @brief Borrowed operation hook stored only by test-enabled builds.
 * @throws Nothing for alias use.
 */
using EmbeddedOperationHookPtr = const EmbeddedOperationTestHook*;

/**
 * @brief Process-local operation hook used by serialized admission tests.
 * @throws Nothing for atomic initialization and pointer publication.
 * @note Tests clear the hook after every affected future has completed.
 */
std::atomic<EmbeddedOperationHookPtr> g_embedded_operation_test_hook{nullptr};

/**
 * @brief Publishes one operation-lifetime phase to the installed hook.
 * @param event Operation reaching the checkpoint.
 * @param phase Admission-lifetime phase being observed.
 * @param admission_active Exact session-admission snapshot for snapshot phases.
 * @return Nothing.
 * @throws Nothing.
 */
void notify_embedded_operation_test_hook(EmbeddedOperationTestEvent event,
                                         EmbeddedOperationTestPhase phase,
                                         bool admission_active) noexcept {
  const EmbeddedOperationTestHook* hook =
      g_embedded_operation_test_hook.load(std::memory_order_acquire);
  if (hook != nullptr && hook->wait != nullptr) {
    hook->wait(hook->context, event, phase, admission_active);
  }
}
#endif

namespace {

/**
 * @brief Owns the backend objects used by one embedded Host adapter.
 *
 * @throws std::bad_alloc if Kernel or InteractionService dependencies allocate
 *         during construction.
 * @note The state owns every joined async status worker. Destroying the Host
 *       waits those workers before Kernel teardown, so a returned future can
 *       outlive the Host without leaving a worker with dangling backend state.
 */
struct EmbeddedHostState {
  /**
   * @brief Tracked async compute submitted through the Host adapter.
   *
   * @throws Nothing for destruction.
   * @note The shared future is copied into this table so graph close can wait
   *       for backend work before releasing the graph runtime.
   */
  struct TrackedAsyncCompute {
    /** @brief Adapter-local tracking id. */
    uint64_t id = 0;

    /** @brief Session whose runtime is used by the async compute. */
    GraphSessionId session;

    /**
     * @brief Shared backend completion future for the compute request.
     *
     * @note The entry stays in outstanding_async_ until the Host wrapper has
     *       converted the backend-owned exact result into OperationStatus. A
     *       ready backend future alone is not enough to close the session
     *       because status publication is part of the accepted operation.
     */
    std::shared_future<Kernel::AsyncComputeResult> future;

    /**
     * @brief Joined worker that publishes the public OperationStatus future.
     *
     * @note The state owns this worker after attachment. Close or state
     * destruction waits it after `status_published` becomes true; no worker is
     * detached and the worker borrows this state only for that joined lifetime.
     */
    std::future<void> status_worker;

    /** @brief Whether status_worker has been attached to this placeholder. */
    bool status_worker_attached = false;

    /** @brief Whether the caller-visible status future has been made ready. */
    bool status_published = false;
  };

  /**
   * @brief One admitted synchronous session-scoped Host operation.
   *
   * @throws Nothing for destruction.
   * @note The entry contains no borrowed scheduler or graph pointer; it only
   *       keeps close from destroying the session until the complete admitted
   *       Host call, including Kernel execution and public status/value
   *       translation, has finished.
   */
  struct ActiveSessionAdmission {
    /** @brief Adapter-local admission id. */
    uint64_t id = 0;

    /** @brief Session protected by this admission. */
    GraphSessionId session;
  };

  /**
   * @brief RAII token for one synchronous session admission.
   *
   * @throws Nothing for destruction.
   * @note Tokens are movable and release exactly one table entry. The token is
   *       held through the complete admitted Host call but never holds the
   *       lifecycle mutex while runtime, graph-state, or scheduler work is
   *       coordinated.
   */
  class SessionAdmissionToken {
   public:
    /** @brief Creates an empty non-admitted token. @throws Nothing. */
    SessionAdmissionToken() noexcept = default;

    /**
     * @brief Creates a token for one pre-registered admission id.
     * @param state Adapter state that owns the admission table.
     * @param id Registered admission id.
     * @throws Nothing.
     */
    SessionAdmissionToken(EmbeddedHostState* state, uint64_t id) noexcept
        : state_(state), id_(id) {}

    /**
     * @brief Transfers one admission without releasing it.
     * @param other Token whose ownership is transferred.
     * @throws Nothing.
     */
    SessionAdmissionToken(SessionAdmissionToken&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)),
          id_(std::exchange(other.id_, 0)) {}

    /**
     * @brief Transfers admission ownership after releasing any current entry.
     * @param other Token whose ownership is transferred.
     * @return This token.
     * @throws Nothing.
     */
    SessionAdmissionToken& operator=(SessionAdmissionToken&& other) noexcept {
      if (this != &other) {
        release();
        state_ = std::exchange(other.state_, nullptr);
        id_ = std::exchange(other.id_, 0);
      }
      return *this;
    }

    /** @brief Releases the active admission. @throws Nothing. */
    ~SessionAdmissionToken() { release(); }

    /**
     * @brief Copying would double-release one admission and is disabled.
     * @throws Nothing because this operation is unavailable.
     */
    SessionAdmissionToken(const SessionAdmissionToken&) = delete;

    /**
     * @brief Copy assignment would double-release and is disabled.
     * @return No value because this operation is unavailable.
     * @throws Nothing because this operation is unavailable.
     */
    SessionAdmissionToken& operator=(const SessionAdmissionToken&) = delete;

   private:
    /**
     * @brief Releases the owned admission when present.
     * @return Nothing.
     * @throws Nothing.
     */
    void release() noexcept {
      if (state_ != nullptr) {
        state_->release_session_admission(id_);
        state_ = nullptr;
        id_ = 0;
      }
    }

    /** @brief Non-owning state pointer valid for the Host method duration. */
    EmbeddedHostState* state_ = nullptr;

    /** @brief Registered admission id, or zero for an empty token. */
    uint64_t id_ = 0;
  };

  /**
   * @brief Result of scheduling backend async compute under Host tracking lock.
   *
   * @throws Nothing for destruction.
   * @note `scheduled=false` means no backend task was queued, either because
   *       the backend rejected the request or because the session is closing.
   */
  struct AsyncComputeRegistration {
    /** @brief True when backend work was scheduled and tracked atomically. */
    bool scheduled = false;

    /** @brief Adapter-local tracking id for the scheduled backend future. */
    uint64_t tracking_id = 0;

    /** @brief Shared exact backend result consumed by the Host wrapper. */
    std::shared_future<Kernel::AsyncComputeResult> future;
  };

  /**
   * @brief Process-domain CPU execution owner shared with Kernel.
   *
   * @note Declaration order makes this service outlive Kernel and every
   * request-local ComputeService that borrows it.
   */
  std::shared_ptr<compute::ExecutionService> execution_service;

  /** @brief Backend Kernel instance owned by the embedded adapter. */
  Kernel kernel;

  /** @brief Internal interaction facade used only by this Host adapter. */
  InteractionService interaction;

  /**
   * @brief Creates backend state from explicit retained dependencies.
   *
   * @param image_codec Shared artifact codec owner.
   * @param metadata_codec Shared metadata codec owner.
   * @param document_reader Shared graph/node document reader owner.
   * @param document_writer Shared graph/node document writer owner.
   * @throws std::invalid_argument when any required owner is empty.
   * @throws std::bad_alloc if backend ownership allocation fails.
   * @note ExecutionService is composed first with explicit product resource
   * limits, Kernel retains the same owner, and InteractionService borrows only
   * the completed Kernel.
   */
  EmbeddedHostState(std::shared_ptr<const ImageArtifactCodec> image_codec,
                    std::shared_ptr<const CacheMetadataCodec> metadata_codec,
                    std::shared_ptr<const GraphDocumentReader> document_reader,
                    std::shared_ptr<const GraphDocumentWriter> document_writer)
      : execution_service(std::make_shared<compute::ExecutionService>(
            compute::ExecutionService::default_resource_limits())),
        kernel(std::move(image_codec), std::move(metadata_codec),
               std::move(document_reader), std::move(document_writer),
               execution_service),
        interaction(kernel) {}

  /**
   * @brief Waits for all tracked async computes before destroying the backend.
   *
   * @throws Nothing.
   * @note Waiting here preserves runtime lifetime if a Host is destroyed while
   *       callers still hold adapter-created futures.
   */
  ~EmbeddedHostState() { wait_for_all_async_compute(); }

  /**
   * @brief Joins and removes completed asynchronous status workers.
   *
   * @return Nothing.
   * @throws Nothing; an unexpected future wait failure is contained during
   * cleanup.
   * @note Publication is recorded only after the caller-visible promise is
   * ready. Reaping before a new submission keeps completed tracking state
   * bounded without joining an active worker while holding lifecycle_mutex_.
   */
  void reap_published_async_compute() noexcept {
    for (;;) {
      std::future<void> worker;
      {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        const auto completed = std::find_if(
            outstanding_async_.begin(), outstanding_async_.end(),
            [](const TrackedAsyncCompute& tracked) {
              return tracked.status_published && tracked.status_worker_attached;
            });
        if (completed == outstanding_async_.end()) {
          return;
        }
        worker = std::move(completed->status_worker);
        outstanding_async_.erase(completed);
      }
      try {
        if (worker.valid()) {
          worker.wait();
        }
      } catch (...) {
      }
    }
  }

  /**
   * @brief Attempts one synchronous session admission under the close gate.
   *
   * @param session Session whose runtime will be used by the Host method.
   * @return Owned token, or nullopt after close has marked the session closing.
   * @throws std::bad_alloc if adding the admission entry allocates.
   * @note The lifecycle mutex is released before return. The caller keeps the
   *       token alive through the complete Kernel call and exact public
   *       status/value translation. The token is a logical lifetime admission;
   *       runtime startup and graph-state or scheduler coordination occur only
   *       after the lifecycle mutex has been released. BUILD_TESTING publishes
   *       a non-blocking admission event only after the entry is installed and
   *       while the lifecycle mutex still establishes total event order.
   */
  std::optional<SessionAdmissionToken> try_admit_session_operation(
      const GraphSessionId& session) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (session_close_in_progress_locked(session)) {
      return std::nullopt;
    }
    const uint64_t id = next_admission_id_++;
    active_admissions_.push_back(ActiveSessionAdmission{id, session});
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    notify_embedded_lifecycle_test_hook(
        EmbeddedLifecycleTestEvent::SessionOperationAdmitted);
#endif
    return SessionAdmissionToken(this, id);
  }

#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
  /**
   * @brief Publishes one lock-free operation checkpoint with exact admission.
   *
   * @param session Session whose current admission state is sampled.
   * @param event Host operation reaching the checkpoint.
   * @param phase Phase before Kernel entry or after public translation.
   * @return Nothing.
   * @throws Nothing except implementation-defined mutex system errors.
   * @note BeforeKernelReady deliberately skips the snapshot so its callback can
   *       hold the operation while the test joins the blocking compute. Every
   *       callback runs after lifecycle_mutex_ is released and outside GSE.
   */
  void wait_at_operation_test_phase(const GraphSessionId& session,
                                    EmbeddedOperationTestEvent event,
                                    EmbeddedOperationTestPhase phase) {
    bool admission_active = false;
    if (phase != EmbeddedOperationTestPhase::BeforeKernelReady) {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      admission_active = has_active_session_admission_locked(session);
    }
    notify_embedded_operation_test_hook(event, phase, admission_active);
  }
#endif

  /**
   * @brief Pre-registers and schedules backend async compute for close safety.
   *
   * @tparam Scheduler Callable returning an optional future with the exact
   *         backend async result.
   * @param session Session whose runtime will be captured by backend work.
   * @param scheduler Backend scheduling callable executed without the Host
   *        lifecycle mutex.
   * @return Registration containing a tracked shared future, or
   * `scheduled=false`.
   * @throws std::bad_alloc if placeholder, task, or future-state allocation
   *         fails.
   * @throws std::system_error if Host or graph-state synchronization fails.
   * @throws Any non-close backend submission exception unchanged after the
   *         placeholder is removed. A lane `std::runtime_error` caused by this
   *         session's published close marker becomes `scheduled=false`.
   * @note Phase one reserves a placeholder under `lifecycle_mutex_`; phase two
   *       releases that mutex before entering the bounded graph-state lane.
   *       Close therefore observes every in-flight scheduler, can publish its
   *       marker, and can stop lane admission to wake a full-queue producer.
   *       The placeholder remains incomplete until scheduling either publishes
   *       the shared future or removes the entry. No post-submit Host
   * allocation is required to establish runtime ownership.
   */
  template <typename Scheduler>
  AsyncComputeRegistration schedule_and_track_async_compute(
      const GraphSessionId& session, Scheduler&& scheduler) {
    static_assert(
        noexcept(
            std::declval<std::future<Kernel::AsyncComputeResult>&>().share()),
        "future::share must not allocate after backend scheduling");

    reap_published_async_compute();
    const uint64_t id = next_async_id_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      if (session_close_in_progress_locked(session)) {
        return AsyncComputeRegistration{};
      }
      TrackedAsyncCompute placeholder;
      placeholder.id = id;
      placeholder.session = session;
      outstanding_async_.push_back(std::move(placeholder));
    }

    std::shared_future<Kernel::AsyncComputeResult> shared_future;
    try {
      auto future = scheduler();
      if (!future) {
        remove_async_compute_tracking(id);
        return AsyncComputeRegistration{};
      }
      shared_future = future->share();
    } catch (const std::system_error&) {
      remove_async_compute_tracking(id);
      throw;
    } catch (const std::runtime_error&) {
      bool close_in_progress = false;
      {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        close_in_progress = session_close_in_progress_locked(session);
        outstanding_async_.erase(
            std::remove_if(outstanding_async_.begin(), outstanding_async_.end(),
                           [id](const TrackedAsyncCompute& tracked) {
                             return tracked.id == id;
                           }),
            outstanding_async_.end());
      }
      lifecycle_cv_.notify_all();
      if (close_in_progress) {
        return AsyncComputeRegistration{};
      }
      throw;
    } catch (...) {
      remove_async_compute_tracking(id);
      throw;
    }

    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      auto tracked = std::find_if(
          outstanding_async_.begin(), outstanding_async_.end(),
          [id](const TrackedAsyncCompute& entry) { return entry.id == id; });
      if (tracked != outstanding_async_.end()) {
        tracked->future = shared_future;
      }
    }
    lifecycle_cv_.notify_all();

    AsyncComputeRegistration registration;
    registration.scheduled = true;
    registration.tracking_id = id;
    registration.future = std::move(shared_future);
    return registration;
  }

  /**
   * @brief Attaches the joined public-status worker to a tracked compute.
   *
   * @param id Tracking id returned by schedule_and_track_async_compute().
   * @param worker Joinable worker that owns the status promise.
   * @return Nothing.
   * @throws Nothing.
   * @note Attachment uses the preallocated placeholder and cannot allocate.
   * Close treats an unattached worker as unfinished, covering the interval
   * between backend acceptance and wrapper-worker publication.
   */
  void attach_async_status_worker(uint64_t id,
                                  std::future<void> worker) noexcept {
    bool attached = false;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      const auto tracked = std::find_if(
          outstanding_async_.begin(), outstanding_async_.end(),
          [id](const TrackedAsyncCompute& entry) { return entry.id == id; });
      if (tracked != outstanding_async_.end()) {
        tracked->status_worker = std::move(worker);
        tracked->status_worker_attached = true;
        attached = true;
      }
    }
    lifecycle_cv_.notify_all();
    if (!attached) {
      try {
        if (worker.valid()) {
          worker.wait();
        }
      } catch (...) {
      }
    }
  }

  /**
   * @brief Records that one caller-visible asynchronous status is ready.
   *
   * @param id Tracking id whose public promise was fulfilled or broken.
   * @return Nothing.
   * @throws Nothing.
   * @note The status worker calls this only after set_value(), set_exception(),
   * or explicit promise destruction has made the public future observable.
   */
  void mark_async_status_published(uint64_t id) noexcept {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      const auto tracked = std::find_if(
          outstanding_async_.begin(), outstanding_async_.end(),
          [id](const TrackedAsyncCompute& entry) { return entry.id == id; });
      if (tracked != outstanding_async_.end()) {
        tracked->status_published = true;
      }
    }
    lifecycle_cv_.notify_all();
  }

  /**
   * @brief Removes one async placeholder or completed tracking entry.
   *
   * @param id Tracking id whose backend/runtime ownership is no longer pending.
   * @return Nothing.
   * @throws Nothing.
   * @note Before backend acceptance this rolls back the pre-registration. After
   *       acceptance, callers must first wait the backend future so removal
   *       cannot let close destroy a runtime still captured by untracked work.
   *       Every removal notifies close waiters.
   */
  void remove_async_compute_tracking(uint64_t id) noexcept {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      outstanding_async_.erase(
          std::remove_if(outstanding_async_.begin(), outstanding_async_.end(),
                         [id](const TrackedAsyncCompute& tracked) {
                           return tracked.id == id;
                         }),
          outstanding_async_.end());
    }
    lifecycle_cv_.notify_all();
  }

  /**
   * @brief Claims the exclusive Host close marker for one session.
   *
   * @param session Session about to be closed.
   * @return Nothing after this caller exclusively owns the close marker.
   * @throws std::bad_alloc if recording the closing marker allocates.
   * @throws std::system_error if lifecycle synchronization fails.
   * @note A caller arriving during another close waits until that attempt
   *       clears its marker, then claims a fresh marker and performs its own
   *       backend existence/close attempt. This phase deliberately does not
   *       wait for admitted users. The caller next drains pre-marker
   *       synchronous admissions, then stops Kernel lane admission so a
   *       full-queue async producer can leave its placeholder.
   */
  void begin_session_close(const GraphSessionId& session) {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    while (session_close_in_progress_locked(session)) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
      notify_embedded_lifecycle_test_hook(
          EmbeddedLifecycleTestEvent::DuplicateAboutToWait);
#endif
      lifecycle_cv_.wait(
          lock, [&] { return !session_close_in_progress_locked(session); });
    }
    mark_session_closing_locked(session);
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    notify_embedded_lifecycle_test_hook(
        EmbeddedLifecycleTestEvent::MarkerClaimed);
#endif
  }

  /**
   * @brief Waits until every pre-close synchronous Host admission finishes.
   * @param session Session whose marker is already owned by this close caller.
   * @return Nothing after no synchronous admission token protects the runtime.
   * @throws std::system_error if lifecycle synchronization fails.
   * @note The graph-state lane remains accepting during this phase so a save,
   *       reload, node replacement, ROI projection, or other already-admitted
   *       call can enter Kernel and preserve its result contract. The close
   *       marker prevents any new synchronous admission. After this method
   *       returns, close stops lane admission before waiting on async
   *       placeholders.
   */
  void wait_for_session_admissions(const GraphSessionId& session) {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    lifecycle_cv_.wait(
        lock, [&] { return !has_active_session_admission_locked(session); });
  }

  /**
   * @brief Waits for pre-registered async scheduling and public status workers.
   * @param session Session whose marker and stopped Kernel lane are owned by
   *        this close caller.
   * @return Nothing after every async placeholder has been rejected or every
   *         accepted backend result has reached its caller-visible promise.
   * @throws std::system_error if lifecycle synchronization fails.
   * @note Kernel lane admission must already be stopped. Rejected in-flight
   *       schedulers remove their placeholders and wake this waiter;
   *       backend-accepted work remains tracked through status publication.
   *       Completed status workers are joined without holding
   *       `lifecycle_mutex_`.
   */
  void wait_for_session_async_compute(const GraphSessionId& session) {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    lifecycle_cv_.wait(
        lock, [&] { return !has_outstanding_async_compute_locked(session); });
    for (;;) {
      const auto completed =
          std::find_if(outstanding_async_.begin(), outstanding_async_.end(),
                       [&session](const TrackedAsyncCompute& tracked) {
                         return tracked.session.value == session.value;
                       });
      if (completed == outstanding_async_.end()) {
        return;
      }
      std::future<void> worker = std::move(completed->status_worker);
      outstanding_async_.erase(completed);
      lock.unlock();
      try {
        if (worker.valid()) {
          worker.wait();
        }
      } catch (...) {
      }
      lock.lock();
    }
  }

  /**
   * @brief Ends a Host close lifecycle for one session.
   *
   * @param session Session whose backend close attempt has returned.
   * @return Nothing.
   * @throws Nothing.
   * @note This method must be called after `begin_session_close()` even when a
   *       later admission-stop or backend close reports NotFound, so load
   *       attempts using the same label are not rejected as still closing.
   */
  void finish_session_close(const GraphSessionId& session) {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      closing_sessions_.erase(
          std::remove(closing_sessions_.begin(), closing_sessions_.end(),
                      session.value),
          closing_sessions_.end());
    }
    lifecycle_cv_.notify_all();
  }

  /**
   * @brief Waits for every tracked async compute wrapper to finish.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note Used during adapter-state destruction before Kernel member teardown.
   *       Entries are removed only after Host OperationStatus mapping finishes.
   */
  void wait_for_all_async_compute() {
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    lifecycle_cv_.wait(lock, [&] {
      return std::all_of(outstanding_async_.begin(), outstanding_async_.end(),
                         [](const TrackedAsyncCompute& tracked) {
                           return tracked.status_published &&
                                  tracked.status_worker_attached;
                         });
    });
    while (!outstanding_async_.empty()) {
      std::future<void> worker =
          std::move(outstanding_async_.back().status_worker);
      outstanding_async_.pop_back();
      lock.unlock();
      try {
        if (worker.valid()) {
          worker.wait();
        }
      } catch (...) {
      }
      lock.lock();
    }
  }

 private:
  /**
   * @brief Releases one admitted synchronous session operation.
   *
   * @param id Adapter-local admission id returned in a token.
   * @return Nothing.
   * @throws Nothing.
   * @note Removal and waiter notification happen after the Kernel call and its
   *       public status/value translation have completed. Unknown ids are
   *       ignored so a moved-from or already-released token is harmless.
   */
  void release_session_admission(uint64_t id) noexcept {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      active_admissions_.erase(
          std::remove_if(active_admissions_.begin(), active_admissions_.end(),
                         [id](const ActiveSessionAdmission& admission) {
                           return admission.id == id;
                         }),
          active_admissions_.end());
    }
    lifecycle_cv_.notify_all();
  }

  /**
   * @brief Returns whether one session still has Host async status work.
   *
   * @param session Session label to search.
   * @return True while the caller-visible status is not ready or its joined
   * worker has not yet been attached.
   * @throws Nothing.
   * @note Caller must hold lifecycle_mutex_. Published entries remain owned
   * until close, the next submission, or adapter destruction joins the worker.
   */
  bool has_outstanding_async_compute_locked(
      const GraphSessionId& session) const {
    return std::any_of(
        outstanding_async_.begin(), outstanding_async_.end(),
        [&session](const TrackedAsyncCompute& tracked) {
          return tracked.session.value == session.value &&
                 (!tracked.status_published || !tracked.status_worker_attached);
        });
  }

  /**
   * @brief Returns whether a synchronous operation still uses one session.
   *
   * @param session Session label to search.
   * @return True while an admitted compute, image compute, scheduler-info,
   *         scheduler-replacement, reload, required-save, node-YAML
   *         replacement, ROI projection, timing, or all-cache-clear call has
   *         not finished public translation.
   * @throws Nothing.
   * @note Caller must hold lifecycle_mutex_. Close waits for these admissions
   *       before entering Kernel::close_graph(), so the runtime map entry and
   *       scheduler owner remain alive for the complete admitted call.
   */
  bool has_active_session_admission_locked(
      const GraphSessionId& session) const noexcept {
    return std::any_of(active_admissions_.begin(), active_admissions_.end(),
                       [&session](const ActiveSessionAdmission& admission) {
                         return admission.session.value == session.value;
                       });
  }

  /**
   * @brief Returns whether one session is already in close lifecycle.
   *
   * @param session Session label to search.
   * @return True when close_graph has marked the session closing.
   * @throws Nothing.
   * @note Caller must hold lifecycle_mutex_.
   */
  bool session_close_in_progress_locked(const GraphSessionId& session) const {
    return std::find(closing_sessions_.begin(), closing_sessions_.end(),
                     session.value) != closing_sessions_.end();
  }

  /**
   * @brief Records that close_graph has started for one session.
   *
   * @param session Session label being closed.
   * @return Nothing.
   * @throws std::bad_alloc if inserting the label allocates.
   * @note Caller must hold lifecycle_mutex_ and must already have rejected a
   *       duplicate marker. The separate wait path prevents two callers from
   *       entering Kernel::close_graph() concurrently for one session.
   */
  void mark_session_closing_locked(const GraphSessionId& session) {
    if (!session_close_in_progress_locked(session)) {
      closing_sessions_.push_back(session.value);
    }
  }

  /**
   * @brief Protects closing markers plus async and synchronous admissions.
   */
  std::mutex lifecycle_mutex_;

  /**
   * @brief Notifies close waiters when admitted Host status mapping completes.
   */
  std::condition_variable lifecycle_cv_;

  /** @brief Adapter-local async compute tracking table. */
  std::vector<TrackedAsyncCompute> outstanding_async_;

  /** @brief Synchronous session operations admitted before close began. */
  std::vector<ActiveSessionAdmission> active_admissions_;

  /** @brief Sessions currently being closed by the Host adapter. */
  std::vector<std::string> closing_sessions_;

  /** @brief Monotonic id source for async tracking entries. */
  std::atomic<uint64_t> next_async_id_{1};

  /** @brief Monotonic id source for synchronous admission entries. */
  std::atomic<uint64_t> next_admission_id_{1};
};

/**
 * @brief Returns a successful OperationStatus.
 *
 * @return Status with ok=true and no diagnostic text.
 * @throws Nothing.
 * @note Success uses the canonical None/zero/empty representation shared by
 *       embedded and IPC products.
 */
OperationStatus success_status() {
  return OperationStatus{};
}

/**
 * @brief Builds a failed OperationStatus.
 *
 * @param code Stable error code for the failure.
 * @param message Human-readable diagnostic text.
 * @return Graph-domain status with ok=false, the exact numeric code, and its
 *         stable lowercase name.
 * @throws std::bad_alloc if copying message allocates and fails.
 * @note The message is diagnostic and should not be parsed for control flow.
 */
OperationStatus failure_status(GraphErrc code, std::string message) {
  OperationStatus status;
  status.ok = false;
  status.domain = OperationErrorDomain::Graph;
  status.code = static_cast<std::int32_t>(code);
  status.name = graph_error_stable_name(code);
  status.message = std::move(message);
  return status;
}

/**
 * @brief Converts one backend-owned asynchronous outcome to public status.
 *
 * @param outcome Exact result captured by the asynchronous compute work item.
 * @param session Session used only to construct an invariant-failure message.
 * @return Canonical success, the captured graph failure, or ComputeError when
 *         a malformed failed outcome contains no error value.
 * @throws std::bad_alloc if public diagnostic construction allocates.
 * @note This helper never reads Kernel::last_error(). Concurrent operations may
 *       replace that best-effort diagnostic without changing this outcome.
 */
OperationStatus status_from_async_compute_outcome(
    const Kernel::AsyncComputeResult& outcome, const GraphSessionId& session) {
  if (outcome.ok) {
    return success_status();
  }
  if (outcome.error) {
    return failure_status(outcome.error->code, outcome.error->message);
  }
  return failure_status(
      GraphErrc::ComputeError,
      "async compute returned no failure for graph session: " + session.value);
}

/**
 * @brief Wraps a value in a successful Result.
 *
 * @tparam Value Result payload type.
 * @param value Value to move into the result.
 * @return Successful Result carrying value.
 * @throws Whatever moving Value may throw.
 * @note The helper keeps status construction consistent across adapter
 *       methods.
 */
template <typename Value>
Result<Value> success_result(Value value) {
  Result<Value> result;
  result.status = success_status();
  result.value = std::move(value);
  return result;
}

/**
 * @brief Builds a failed Result.
 *
 * @tparam Value Result payload type.
 * @param code Stable error code for the failure.
 * @param message Human-readable diagnostic text.
 * @return Failed Result with a default payload.
 * @throws std::bad_alloc if copying message allocates and fails.
 * @note Callers must ignore value when status.ok is false.
 */
template <typename Value>
Result<Value> failure_result(GraphErrc code, std::string message) {
  Result<Value> result;
  result.status = failure_status(code, std::move(message));
  return result;
}

/**
 * @brief Builds a successful VoidResult.
 *
 * @return Success status with no payload.
 * @throws Nothing.
 */
VoidResult success_void() {
  return VoidResult{success_status()};
}

/**
 * @brief Builds a failed VoidResult.
 *
 * @param code Stable error code for the failure.
 * @param message Human-readable diagnostic text.
 * @return Failure status with no payload.
 * @throws std::bad_alloc if copying message allocates and fails.
 */
VoidResult failure_void(GraphErrc code, std::string message) {
  return VoidResult{failure_status(code, std::move(message))};
}

/**
 * @brief Maps a caught GraphError into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param error Backend graph error.
 * @return Failed status preserving the backend error code.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 * @note Public Host methods use this path to avoid leaking exceptions through
 *       the frontend seam for recoverable backend errors.
 */
OperationStatus status_from_exception(const char* operation,
                                      const GraphError& error) {
  return failure_status(error.code(),
                        std::string(operation) + " failed: " + error.what());
}

/**
 * @brief Maps a filesystem exception into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param error Filesystem exception from backend IO or testable path handling.
 * @return Io failure status with diagnostic text.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 */
OperationStatus status_from_exception(
    const char* operation, const std::filesystem::filesystem_error& error) {
  return failure_status(GraphErrc::Io,
                        std::string(operation) + " IO failed: " + error.what());
}

/**
 * @brief Maps a standard exception into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param fallback_code Error code used when no more specific mapping exists.
 * @param error Standard exception raised by a recoverable backend operation.
 * @return Failure status with copied diagnostic text.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 * @note std::bad_alloc is intentionally handled by callers before this helper
 *       so allocation failure semantics remain the language default.
 */
OperationStatus status_from_exception(const char* operation,
                                      GraphErrc fallback_code,
                                      const std::exception& error) {
  return failure_status(fallback_code,
                        std::string(operation) + " failed: " + error.what());
}

/**
 * @brief Maps an unknown exception into a Host operation status.
 *
 * @param operation Frontend operation name used in the diagnostic prefix.
 * @param fallback_code Error code used for the unknown failure.
 * @return Failure status with stable generic diagnostic text.
 * @throws std::bad_alloc if diagnostic string allocation fails.
 */
OperationStatus status_from_unknown_exception(const char* operation,
                                              GraphErrc fallback_code) {
  return failure_status(
      fallback_code, std::string(operation) + " failed with unknown exception");
}

/**
 * @brief Waits for one backend outcome and maps every recoverable exception.
 *
 * @param backend_future Shared exact backend future owned by tracking and the
 * status worker.
 * @param session Session used for exact outcome and fallback diagnostics.
 * @return Public status captured from the backend work item or a mapped
 * recoverable wrapper exception.
 * @throws std::bad_alloc when backend execution or status construction
 * exhausts memory.
 * @note This helper never reads shared LastError state. Unknown and ordinary
 * wrapper exceptions retain the established embedded Host mapping.
 */
OperationStatus await_async_compute_status(
    const std::shared_future<Kernel::AsyncComputeResult>& backend_future,
    const GraphSessionId& session) {
  try {
    const Kernel::AsyncComputeResult& outcome = backend_future.get();
    return status_from_async_compute_outcome(outcome, session);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    return status_from_exception("compute_async", error);
  } catch (const std::exception& error) {
    return status_from_exception("compute_async", GraphErrc::ComputeError,
                                 error);
  } catch (...) {
    return status_from_unknown_exception("compute_async",
                                         GraphErrc::ComputeError);
  }
}

/**
 * @brief Executes a value-returning Host method with exception-to-status
 * translation.
 *
 * @tparam Value Public Result payload type.
 * @tparam Fn Callable returning Result<Value>.
 * @param operation Frontend operation name used in diagnostics.
 * @param fallback_code Error code used for generic std::exception failures.
 * @param fn Host method body to execute.
 * @return Result from fn, or a failed Result when a recoverable exception is
 *         caught.
 * @throws std::bad_alloc when allocation failure prevents reliable status
 *         construction.
 * @note Concrete adapters translate provider exceptions before this public
 * seam; the guard handles only dependency-neutral failures.
 */
template <typename Value, typename Fn>
Result<Value> guarded_result(const char* operation, GraphErrc fallback_code,
                             Fn&& fn) {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    Result<Value> result;
    result.status = status_from_exception(operation, error);
    return result;
  } catch (const std::filesystem::filesystem_error& error) {
    Result<Value> result;
    result.status = status_from_exception(operation, error);
    return result;
  } catch (const std::exception& error) {
    Result<Value> result;
    result.status = status_from_exception(operation, fallback_code, error);
    return result;
  } catch (...) {
    Result<Value> result;
    result.status = status_from_unknown_exception(operation, fallback_code);
    return result;
  }
}

/**
 * @brief Executes a void Host method with exception-to-status translation.
 *
 * @tparam Fn Callable returning VoidResult.
 * @param operation Frontend operation name used in diagnostics.
 * @param fallback_code Error code used for generic std::exception failures.
 * @param fn Host method body to execute.
 * @return VoidResult from fn, or a failed status when a recoverable exception
 *         is caught.
 * @throws std::bad_alloc when allocation failure prevents reliable status
 *         construction.
 * @note Backend exceptions are normalized at the adapter boundary.
 */
template <typename Fn>
VoidResult guarded_void(const char* operation, GraphErrc fallback_code,
                        Fn&& fn) {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    return VoidResult{status_from_exception(operation, error)};
  } catch (const std::filesystem::filesystem_error& error) {
    return VoidResult{status_from_exception(operation, error)};
  } catch (const std::exception& error) {
    return VoidResult{status_from_exception(operation, fallback_code, error)};
  } catch (...) {
    return VoidResult{status_from_unknown_exception(operation, fallback_code)};
  }
}

/**
 * @brief Executes graph close while reserving NotFound for an absent session.
 *
 * @tparam Fn Callable returning VoidResult and explicitly producing NotFound
 *         only when Kernel reports that no graph map entry exists.
 * @param fn Close body to execute after Host admission coordination.
 * @return Callable result, the exact recoverable Graph/filesystem category, or
 *         Graph Unknown for scheduler NotFound and otherwise
 *         unclassified exceptions raised while stopping an existing runtime.
 * @throws std::bad_alloc when backend execution or status construction exhausts
 *         memory.
 * @note Unlike generic guards, this boundary deliberately remaps even a caught
 *       GraphError::NotFound because scheduler shutdown cannot prove absence.
 */
template <typename Fn>
VoidResult guarded_graph_close(Fn&& fn) {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    if (error.code() == GraphErrc::NotFound) {
      return VoidResult{
          status_from_exception("close_graph", GraphErrc::Unknown, error)};
    }
    return VoidResult{status_from_exception("close_graph", error)};
  } catch (const std::filesystem::filesystem_error& error) {
    return VoidResult{status_from_exception("close_graph", error)};
  } catch (const std::exception& error) {
    return VoidResult{
        status_from_exception("close_graph", GraphErrc::Unknown, error)};
  } catch (...) {
    return VoidResult{
        status_from_unknown_exception("close_graph", GraphErrc::Unknown)};
  }
}

/**
 * @brief Executes a status-returning Host method with exception translation.
 *
 * @tparam Fn Callable returning OperationStatus.
 * @param operation Frontend operation name used in diagnostics.
 * @param fallback_code Error code used for generic std::exception failures.
 * @param fn Host method body to execute.
 * @return OperationStatus from fn, or a failed status on recoverable exception.
 * @throws std::bad_alloc when allocation failure prevents reliable status
 *         construction.
 */
template <typename Fn>
OperationStatus guarded_status(const char* operation, GraphErrc fallback_code,
                               Fn&& fn) {
  try {
    return std::forward<Fn>(fn)();
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const GraphError& error) {
    return status_from_exception(operation, error);
  } catch (const std::filesystem::filesystem_error& error) {
    return status_from_exception(operation, error);
  } catch (const std::exception& error) {
    return status_from_exception(operation, fallback_code, error);
  } catch (...) {
    return status_from_unknown_exception(operation, fallback_code);
  }
}

/**
 * @brief Checks whether a graph session is currently loaded.
 *
 * @param state Embedded Host backend state.
 * @param session Session id to find.
 * @return True when the backend lists the session.
 * @throws std::bad_alloc if graph listing allocation fails.
 * @note This helper is used only to distinguish missing-graph failures from
 *       optional empty inspection state.
 */
bool session_exists(const EmbeddedHostState& state,
                    const GraphSessionId& session) {
  const auto names = state.interaction.cmd_list_graphs();
  return std::find(names.begin(), names.end(), session.value) != names.end();
}

/**
 * @brief Converts the backend LastError for a graph into a failure status.
 *
 * @param state Embedded Host backend state.
 * @param session Session whose LastError should be inspected.
 * @param fallback_code Error code used when no LastError is available.
 * @param fallback_message Diagnostic used when no LastError is available.
 * @return Failed status with backend diagnostic or fallback text.
 * @throws std::bad_alloc if diagnostic text allocation fails.
 * @note Kernel LastError is best-effort; not every quiet facade writes it.
 */
OperationStatus failure_from_last_error(const EmbeddedHostState& state,
                                        const GraphSessionId& session,
                                        GraphErrc fallback_code,
                                        const std::string& fallback_message) {
  const auto error = state.interaction.cmd_last_error(session.value);
  if (error) {
    return failure_status(error->code, error->message);
  }
  return failure_status(fallback_code, fallback_message);
}

/**
 * @brief Converts a public dirty domain into the compute-service domain enum.
 *
 * @param domain Public dirty domain.
 * @return Internal dirty domain.
 * @throws Nothing.
 */
compute::DirtyDomain to_compute_dirty_domain(DirtyDomain domain) {
  switch (domain) {
    case DirtyDomain::RealTime:
      return compute::DirtyDomain::RealTime;
    case DirtyDomain::HighPrecision:
    default:
      return compute::DirtyDomain::HighPrecision;
  }
}

/**
 * @brief Converts an internal dirty domain into the public dirty domain enum.
 *
 * @param domain Internal dirty domain.
 * @return Public dirty domain.
 * @throws Nothing.
 */
DirtyDomain to_public_dirty_domain(compute::DirtyDomain domain) {
  switch (domain) {
    case compute::DirtyDomain::RealTime:
      return DirtyDomain::RealTime;
    case compute::DirtyDomain::HighPrecision:
    default:
      return DirtyDomain::HighPrecision;
  }
}

/**
 * @brief Converts an internal dirty-source lifecycle value.
 *
 * @param state Internal lifecycle state.
 * @return Public lifecycle state.
 * @throws Nothing.
 */
DirtySourceLifecycleState to_public_dirty_lifecycle(
    compute::DirtySourceLifecycleState state) {
  switch (state) {
    case compute::DirtySourceLifecycleState::Updating:
      return DirtySourceLifecycleState::Updating;
    case compute::DirtySourceLifecycleState::Settled:
      return DirtySourceLifecycleState::Settled;
    case compute::DirtySourceLifecycleState::Idle:
    default:
      return DirtySourceLifecycleState::Idle;
  }
}

/**
 * @brief Converts a format-neutral parameter map into public display strings.
 *
 * @param parameters Backend ParameterValue parameters.
 * @return Map from parameter name to display/serialization text.
 * @throws std::bad_alloc if recursive display conversion allocates and fails.
 * @throws std::logic_error if a value reports an unknown parameter kind.
 * @note Scalar strings retain their exact text. Arrays and objects use the
 * dependency-neutral deterministic inspection grammar.
 */
std::map<std::string, std::string> parameter_strings_from_values(
    const plugin::ParameterMap& parameters) {
  std::map<std::string, std::string> out;
  for (const auto& [key, value] : parameters) {
    out[key] = core::format_parameter_value_for_inspection(value);
  }
  return out;
}

/**
 * @brief Converts debug metadata into a public snapshot.
 *
 * @param debug Backend debug metadata.
 * @return Public debug snapshot.
 * @throws std::bad_alloc if copying strings allocates and fails.
 */
DebugMetadataSnapshot to_public_debug(const DebugMeta& debug) {
  DebugMetadataSnapshot snapshot;
  snapshot.computed_by_worker_id = debug.computed_by_worker_id;
  snapshot.timestamp_us = debug.timestamp_us;
  snapshot.execution_time_ms = debug.execution_time_ms;
  snapshot.min_val = debug.min_val;
  snapshot.max_val = debug.max_val;
  snapshot.has_nan = debug.has_nan;
  snapshot.compute_device = debug.compute_device;
  return snapshot;
}

/**
 * @brief Converts spatial metadata into a public snapshot.
 *
 * @param space Backend spatial context.
 * @param output_width Cached output width in local pixels.
 * @param output_height Cached output height in local pixels.
 * @return Public spatial snapshot.
 * @throws Nothing.
 * @note The local output extent can differ from absolute ROI when an operation
 *       resizes, crops, or scales pixels while preserving graph-space coverage.
 */
SpatialSnapshot to_public_space(const SpatialContext& space, int output_width,
                                int output_height) {
  SpatialSnapshot snapshot;
  snapshot.absolute_roi = space.absolute_roi;
  snapshot.extent = PixelSize{output_width, output_height};
  snapshot.global_scale_x = space.global_scale_x;
  snapshot.global_scale_y = space.global_scale_y;
  std::copy(space.transform_matrix.begin(), space.transform_matrix.end(),
            snapshot.transform_matrix);
  std::copy(space.inverse_matrix.begin(), space.inverse_matrix.end(),
            snapshot.inverse_matrix);
  std::copy(space.local_inverse_matrix.begin(),
            space.local_inverse_matrix.end(), snapshot.local_inverse_matrix);
  return snapshot;
}

/**
 * @brief Converts backend node inspection into a public value snapshot.
 *
 * @param info Backend node inspection result.
 * @return Public node inspection view.
 * @throws std::bad_alloc if recursive parameter display conversion allocates
 * and fails.
 * @throws std::logic_error if a parameter reports an unknown kind.
 * @note Backend Node values are copied into public value fields.
 */
NodeInspectionView to_public_node(const GraphNodeInspectInfo& info) {
  NodeInspectionView view;
  view.id = NodeId{info.id};
  view.name = info.name;
  view.type = info.type;
  view.subtype = info.subtype;
  view.parameters = parameter_strings_from_values(info.parameters);
  if (info.metadata) {
    view.has_cached_output = info.metadata->has_cached_output;
    if (!info.metadata->source_label.empty()) {
      view.source_label = info.metadata->source_label;
    }
    if (info.metadata->has_cached_output) {
      view.debug = to_public_debug(info.metadata->debug);
      view.space =
          to_public_space(info.metadata->space, info.metadata->output_width,
                          info.metadata->output_height);
    }
  }
  return view;
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Injects resource exhaustion inside the graph-view adapter loop.
 *
 * @param info Backend inspection value already produced by
 * GraphInspectService traversal.
 * @return Nothing.
 * @throws std::bad_alloc when info carries the private test probe name.
 * @note This translation-unit-local BUILD_TESTING probe has no mutable state,
 * is safe across concurrent inspections, and adds no installed/public Host
 * seam. Production builds compile out both the check and sentinel behavior.
 */
void throw_if_graph_adapter_bad_alloc_probe(const GraphNodeInspectInfo& info) {
  if (info.name == "__photospider_test_bad_alloc_inspect_adapter__") {
    throw std::bad_alloc{};
  }
}
#endif

/**
 * @brief Converts backend graph inspection into a public graph view.
 *
 * @param session Session that was inspected.
 * @param snapshot Backend graph inspection snapshot.
 * @return Public graph inspection view.
 * @throws std::bad_alloc when public node, parameter, or result storage
 * exhausts memory.
 * @throws std::logic_error if a recursive parameter reports an unknown kind.
 * @note BUILD_TESTING may compile an immutable-name failpoint inside the real
 * adapter loop. Production builds compile out the probe and expose no callable
 * test seam.
 */
GraphInspectionView to_public_graph_view(
    const GraphSessionId& session, const GraphInspectionSnapshot& snapshot) {
  GraphInspectionView view;
  view.session = session;
  view.nodes.reserve(snapshot.nodes.size());
  for (const auto& node : snapshot.nodes) {
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
    throw_if_graph_adapter_bad_alloc_probe(node);
#endif
    view.nodes.push_back(to_public_node(node));
  }
  return view;
}

/**
 * @brief Converts an internal topology edge kind into a public edge kind.
 *
 * @param kind Backend topology edge kind.
 * @return Public edge kind.
 * @throws Nothing.
 */
HostGraphEdgeKind to_public_edge_kind(GraphTopologyEdgeKind kind) {
  switch (kind) {
    case GraphTopologyEdgeKind::ParameterInput:
      return HostGraphEdgeKind::ParameterInput;
    case GraphTopologyEdgeKind::ImageInput:
    default:
      return HostGraphEdgeKind::ImageInput;
  }
}

/**
 * @brief Converts backend topology edge metadata into a public snapshot.
 *
 * @param edge Backend topology edge.
 * @return Public edge snapshot.
 * @throws std::bad_alloc if strings allocate and fail.
 */
HostGraphEdgeSnapshot to_public_edge(const GraphTopologyEdge& edge) {
  HostGraphEdgeSnapshot snapshot;
  snapshot.from_node = NodeId{edge.from_node_id};
  snapshot.to_node = NodeId{edge.to_node_id};
  snapshot.kind = to_public_edge_kind(edge.kind);
  snapshot.from_output_name = edge.from_output_name;
  snapshot.to_input_name = edge.to_input_name;
  snapshot.input_index = edge.input_index;
  return snapshot;
}

/**
 * @brief Converts backend dependency tree scope into a public scope.
 *
 * @param scope Backend dependency tree scope.
 * @return Public dependency tree scope.
 * @throws Nothing.
 */
HostDependencyTreeScope to_public_tree_scope(DependencyTree::Scope scope) {
  switch (scope) {
    case DependencyTree::Scope::StartNode:
      return HostDependencyTreeScope::StartNode;
    case DependencyTree::Scope::EndingNodes:
    default:
      return HostDependencyTreeScope::EndingNodes;
  }
}

/**
 * @brief Converts backend dependency tree into a public snapshot.
 *
 * @param tree Backend dependency tree.
 * @return Public dependency tree snapshot.
 * @throws std::logic_error or std::bad_alloc from node conversion.
 */
HostDependencyTreeSnapshot to_public_dependency_tree(
    const DependencyTree& tree) {
  HostDependencyTreeSnapshot snapshot;
  snapshot.scope = to_public_tree_scope(tree.scope);
  if (tree.start_node_id) {
    snapshot.start_node = NodeId{*tree.start_node_id};
  }
  snapshot.graph_empty = tree.graph_empty;
  snapshot.start_node_found = tree.start_node_found;
  snapshot.no_ending_nodes = tree.no_ending_nodes;
  snapshot.root_nodes.reserve(tree.root_node_ids.size());
  for (int root : tree.root_node_ids) {
    snapshot.root_nodes.push_back(NodeId{root});
  }
  snapshot.entries.reserve(tree.entries.size());
  for (const auto& entry : tree.entries) {
    HostDependencyTreeEntry public_entry;
    public_entry.depth = entry.depth;
    if (entry.incoming_edge) {
      public_entry.incoming_edge = to_public_edge(*entry.incoming_edge);
    }
    public_entry.node = to_public_node(entry.node);
    public_entry.cycle = entry.cycle;
    snapshot.entries.push_back(std::move(public_entry));
  }
  return snapshot;
}

/**
 * @brief Converts traversal node metadata into a public snapshot.
 *
 * @param info Backend traversal node info.
 * @return Public traversal node snapshot.
 * @throws std::bad_alloc if copying the name allocates and fails.
 */
HostTraversalNodeSnapshot to_public_traversal_node(
    const Kernel::TraversalNodeInfo& info) {
  HostTraversalNodeSnapshot snapshot;
  snapshot.node = NodeId{info.id};
  snapshot.name = info.name;
  snapshot.has_memory_cache = info.has_memory_cache;
  snapshot.has_disk_cache = info.has_disk_cache;
  return snapshot;
}

/**
 * @brief Converts backend traversal orders into public node id vectors.
 *
 * @param orders Backend traversal order map.
 * @return Public traversal order map.
 * @throws std::bad_alloc if container allocation fails.
 */
std::map<int, std::vector<NodeId>> to_public_traversal_orders(
    const std::map<int, std::vector<int>>& orders) {
  std::map<int, std::vector<NodeId>> out;
  for (const auto& [end_node, nodes] : orders) {
    auto& converted = out[end_node];
    converted.reserve(nodes.size());
    for (int node : nodes) {
      converted.push_back(NodeId{node});
    }
  }
  return out;
}

/**
 * @brief Converts backend traversal details into public snapshots.
 *
 * @param details Backend traversal details keyed by ending node.
 * @return Public traversal details.
 * @throws std::bad_alloc if container/string allocation fails.
 */
std::map<int, std::vector<HostTraversalNodeSnapshot>>
to_public_traversal_details(
    const std::map<int, std::vector<Kernel::TraversalNodeInfo>>& details) {
  std::map<int, std::vector<HostTraversalNodeSnapshot>> out;
  for (const auto& [end_node, nodes] : details) {
    auto& converted = out[end_node];
    converted.reserve(nodes.size());
    for (const auto& node : nodes) {
      converted.push_back(to_public_traversal_node(node));
    }
  }
  return out;
}

/**
 * @brief Converts backend timing collector into a public timing snapshot.
 *
 * @param timing Backend timing collector.
 * @return Public timing snapshot.
 * @throws std::bad_alloc if container/string allocation fails.
 */
TimingSnapshot to_public_timing(const TimingCollector& timing) {
  TimingSnapshot snapshot;
  snapshot.total_ms = timing.total_ms;
  snapshot.node_timings.reserve(timing.node_timings.size());
  for (const auto& row : timing.node_timings) {
    NodeTimingSnapshot public_row;
    public_row.node = NodeId{row.id};
    public_row.name = row.name;
    public_row.elapsed_ms = row.elapsed_ms;
    public_row.source = row.source;
    snapshot.node_timings.push_back(std::move(public_row));
  }
  return snapshot;
}

/**
 * @brief Converts backend scheduler action into a public action label.
 *
 * @param action Backend scheduler action.
 * @return Public scheduler trace action.
 * @throws Nothing.
 */
HostSchedulerTraceAction to_public_scheduler_action(
    GraphRuntime::SchedulerEvent::Action action) {
  switch (action) {
    case GraphRuntime::SchedulerEvent::ASSIGN_INITIAL:
      return HostSchedulerTraceAction::AssignInitial;
    case GraphRuntime::SchedulerEvent::EXECUTE:
      return HostSchedulerTraceAction::Execute;
    case GraphRuntime::SchedulerEvent::EXECUTE_TILE:
      return HostSchedulerTraceAction::ExecuteTile;
    case GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_SOURCE:
      return HostSchedulerTraceAction::ExecuteDirtySource;
    case GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE:
      return HostSchedulerTraceAction::ExecuteDirtyDownstreamNode;
    case GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE:
      return HostSchedulerTraceAction::ExecuteDirtyDownstreamTile;
    case GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION:
      return HostSchedulerTraceAction::SkipStaleGeneration;
    case GraphRuntime::SchedulerEvent::RETHROW_EXCEPTION:
      return HostSchedulerTraceAction::RethrowException;
  }
  return HostSchedulerTraceAction::Unknown;
}

/**
 * @brief Converts one bounded backend scheduler page into public snapshots.
 *
 * @param backend_page Backend scheduler events and locked metadata.
 * @return Public scheduler trace page preserving sequence metadata.
 * @throws std::bad_alloc if vector allocation fails.
 * @note The conversion is non-destructive and cannot exceed the already
 *       validated backend page bound.
 */
SchedulerTracePage to_public_scheduler_trace_page(
    const GraphRuntime::SchedulerEventPage& backend_page) {
  SchedulerTracePage page;
  page.events.reserve(backend_page.events.size());
  for (const auto& event : backend_page.events) {
    SchedulerTraceEventSnapshot snapshot;
    snapshot.sequence = event.sequence;
    snapshot.epoch = event.epoch;
    snapshot.node = NodeId{event.node_id};
    snapshot.worker_id = event.worker_id;
    snapshot.action = to_public_scheduler_action(event.action);
    snapshot.timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            event.timestamp.time_since_epoch())
            .count());
    page.events.push_back(snapshot);
  }
  page.next_sequence = backend_page.next_sequence;
  page.has_more = backend_page.has_more;
  page.dropped_count = backend_page.dropped_count;
  return page;
}

/**
 * @brief Converts backend planned task shape into a public label.
 *
 * @param kind Backend task shape.
 * @return Stable lowercase display label.
 * @throws Nothing.
 */
std::string to_public_planning_task_kind(compute::PlannedTaskKind kind) {
  switch (kind) {
    case compute::PlannedTaskKind::Node:
      return "node";
    case compute::PlannedTaskKind::Tile:
      return "tile";
    case compute::PlannedTaskKind::Monolithic:
      return "monolithic";
  }
  return "unknown";
}

/**
 * @brief Converts one backend planned task sample into a public snapshot.
 *
 * @param task Backend planned task sample.
 * @return Public task sample snapshot.
 * @throws std::bad_alloc if dependency vector copying allocates.
 * @note The result is diagnostic value data and carries no runtime task
 *       closure, queue entry, or mutable graph object.
 */
ComputePlanningTaskSnapshot to_public_planning_task(
    const compute::PlannedTask& task) {
  ComputePlanningTaskSnapshot snapshot;
  snapshot.task_id = task.task_id;
  snapshot.node = NodeId{task.node_id};
  snapshot.kind = to_public_planning_task_kind(task.kind);
  snapshot.domain = to_public_dirty_domain(task.domain);
  snapshot.output_roi = task.output_roi;
  snapshot.tile_x = task.tile_x;
  snapshot.tile_y = task.tile_y;
  snapshot.tile_size = task.tile_size;
  snapshot.whole_output = task.whole_output;
  snapshot.dirty_selected = task.dirty_selected;
  snapshot.dirty_generation = task.dirty_generation;
  snapshot.dependency_task_ids = task.dependency_task_ids;
  return snapshot;
}

/**
 * @brief Converts one backend planning summary into a public snapshot.
 *
 * @param summary Backend planning summary copied from graph state.
 * @return Public planning inspection snapshot.
 * @throws std::bad_alloc if string/vector allocation fails.
 * @note Deep backend plan references are intentionally ignored; the Host seam
 *       exposes bounded value summaries only.
 */
ComputePlanningInspectionSnapshot to_public_planning_snapshot(
    const compute::ComputePlanSummary& summary) {
  ComputePlanningInspectionSnapshot snapshot;
  snapshot.intent = summary.intent;
  snapshot.target_node = NodeId{summary.target_node_id};
  snapshot.parallel = summary.parallel;
  snapshot.topology_generation = summary.topology_generation;
  snapshot.expansion_cache_key = summary.full_graph_cache_key;
  snapshot.planned_node_count = summary.planned_node_count;
  snapshot.task_count = summary.task_count;
  snapshot.tile_task_count = summary.tile_task_count;
  snapshot.monolithic_task_count = summary.monolithic_task_count;
  snapshot.node_task_count = summary.node_task_count;
  snapshot.dependency_count = summary.dependency_count;
  snapshot.initial_task_count = summary.initial_task_count;
  snapshot.active_task_count = summary.active_task_count;
  snapshot.dirty_source_task_count = summary.dirty_source_task_count;
  snapshot.downstream_task_count = summary.downstream_task_count;
  snapshot.initial_downstream_task_count =
      summary.initial_downstream_task_count;
  snapshot.planned_node_sample.reserve(summary.planned_node_sample.size());
  for (int node_id : summary.planned_node_sample) {
    snapshot.planned_node_sample.push_back(NodeId{node_id});
  }
  snapshot.task_sample.reserve(summary.task_sample.size());
  for (const auto& task : summary.task_sample) {
    snapshot.task_sample.push_back(to_public_planning_task(task));
  }
  return snapshot;
}

/**
 * @brief Converts backend planning summary history into public snapshots.
 *
 * @param summaries Backend bounded summary history.
 * @return Public planning snapshot history.
 * @throws std::bad_alloc if vector/string allocation fails.
 */
std::vector<ComputePlanningInspectionSnapshot> to_public_planning_snapshots(
    const std::vector<compute::ComputePlanSummary>& summaries) {
  std::vector<ComputePlanningInspectionSnapshot> out;
  out.reserve(summaries.size());
  for (const auto& summary : summaries) {
    out.push_back(to_public_planning_snapshot(summary));
  }
  return out;
}

/**
 * @brief Converts backend dirty source state into a public snapshot.
 *
 * @param state Backend dirty source state.
 * @return Public dirty source snapshot.
 * @throws std::bad_alloc if ROI vector allocation fails.
 */
DirtySourceSnapshot to_public_dirty_source(
    const compute::DirtySourceNodeState& state) {
  DirtySourceSnapshot snapshot;
  snapshot.node = NodeId{state.node_id};
  snapshot.domain = to_public_dirty_domain(state.domain);
  snapshot.lifecycle = to_public_dirty_lifecycle(state.lifecycle);
  snapshot.generation = state.generation;
  snapshot.source_rois.reserve(state.source_rois.size());
  for (const auto& roi : state.source_rois) {
    snapshot.source_rois.push_back(roi);
  }
  return snapshot;
}

/**
 * @brief Converts backend dirty tile state into a public snapshot.
 *
 * @param tile Backend dirty tile key.
 * @return Public dirty tile snapshot.
 * @throws Nothing.
 */
DirtyTileSnapshot to_public_dirty_tile(const compute::DirtyTileKey& tile) {
  DirtyTileSnapshot snapshot;
  snapshot.node = NodeId{tile.node_id};
  snapshot.domain = to_public_dirty_domain(tile.domain);
  snapshot.tile_x = tile.tile_x;
  snapshot.tile_y = tile.tile_y;
  snapshot.tile_size = tile.tile_size;
  snapshot.pixel_roi = tile.pixel_roi;
  return snapshot;
}

/**
 * @brief Converts backend dirty edge direction into a public value.
 *
 * @param direction Backend dirty propagation direction.
 * @return Public dirty propagation direction.
 * @throws Nothing.
 */
DirtyEdgeDirection to_public_dirty_edge_direction(
    compute::DirtyEdgeDirection direction) {
  switch (direction) {
    case compute::DirtyEdgeDirection::ForwardAffected:
      return DirtyEdgeDirection::ForwardAffected;
    case compute::DirtyEdgeDirection::BackwardDemand:
      return DirtyEdgeDirection::BackwardDemand;
  }
  return DirtyEdgeDirection::BackwardDemand;
}

/**
 * @brief Converts backend monolithic dirty work into a public snapshot.
 *
 * @param region Backend monolithic dirty record.
 * @return Public monolithic dirty record snapshot.
 * @throws Nothing.
 */
DirtyMonolithicRegionSnapshot to_public_dirty_monolithic_region(
    const compute::DirtyMonolithicRegion& region) {
  DirtyMonolithicRegionSnapshot snapshot;
  snapshot.node = NodeId{region.node_id};
  snapshot.domain = to_public_dirty_domain(region.domain);
  snapshot.pixel_roi = region.pixel_roi;
  snapshot.whole_output = region.whole_output;
  return snapshot;
}

/**
 * @brief Converts backend dirty edge provenance into a public snapshot.
 *
 * @param mapping Backend dirty edge mapping.
 * @return Public dirty edge mapping snapshot.
 * @throws Nothing.
 */
DirtyEdgeMappingSnapshot to_public_dirty_edge_mapping(
    const compute::DirtyEdgeMapping& mapping) {
  DirtyEdgeMappingSnapshot snapshot;
  snapshot.from_node = NodeId{mapping.from_node_id};
  snapshot.to_node = NodeId{mapping.to_node_id};
  snapshot.domain = to_public_dirty_domain(mapping.domain);
  snapshot.from_roi = mapping.from_roi;
  snapshot.to_roi = mapping.to_roi;
  snapshot.direction = to_public_dirty_edge_direction(mapping.direction);
  return snapshot;
}

/**
 * @brief Converts backend dirty-region state into a public snapshot.
 *
 * @param snapshot Backend dirty-region snapshot.
 * @return Public dirty-region inspection snapshot.
 * @throws std::bad_alloc if container allocation fails.
 * @note Only frontend-facing source, tile, monolithic work, actual ROI, and
 *       propagation provenance values are copied.
 */
DirtyRegionInspectionSnapshot to_public_dirty_snapshot(
    const compute::DirtyRegionSnapshot& snapshot) {
  DirtyRegionInspectionSnapshot out;
  out.graph_generation = snapshot.graph_generation;

  std::vector<int> source_ids;
  source_ids.reserve(snapshot.dirty_source_state.size());
  for (const auto& [node_id, _] : snapshot.dirty_source_state) {
    source_ids.push_back(node_id);
  }
  std::sort(source_ids.begin(), source_ids.end());
  out.sources.reserve(source_ids.size());
  for (int node_id : source_ids) {
    out.sources.push_back(
        to_public_dirty_source(snapshot.dirty_source_state.at(node_id)));
  }

  out.dirty_tiles.reserve(snapshot.dirty_tiles.size());
  for (const auto& tile : snapshot.dirty_tiles) {
    out.dirty_tiles.push_back(to_public_dirty_tile(tile));
  }

  out.dirty_monolithic_nodes.reserve(snapshot.dirty_monolithic_nodes.size());
  for (const auto& region : snapshot.dirty_monolithic_nodes) {
    out.dirty_monolithic_nodes.push_back(
        to_public_dirty_monolithic_region(region));
  }

  for (const auto& [node_id, rois] : snapshot.actual_dirty_rois) {
    auto& converted = out.actual_dirty_rois[node_id];
    converted.reserve(rois.size());
    for (const auto& roi : rois) {
      converted.push_back(roi);
    }
  }

  out.edge_mappings.reserve(snapshot.edge_mappings.size());
  for (const auto& mapping : snapshot.edge_mappings) {
    out.edge_mappings.push_back(to_public_dirty_edge_mapping(mapping));
  }
  return out;
}

/**
 * @brief Converts public compute request values into the Kernel request.
 *
 * @param request Public Host compute request.
 * @return Kernel request with kernel-native dirty ROI.
 * @throws std::bad_alloc if copying strings allocates and fails.
 */
Kernel::ComputeRequest to_kernel_compute_request(
    const HostComputeRequest& request) {
  Kernel::ComputeRequest kernel_request;
  kernel_request.name = request.session.value;
  kernel_request.node_id = request.node.value;
  kernel_request.cache.precision = request.cache.precision;
  kernel_request.cache.force_recache = request.cache.force_recache;
  kernel_request.cache.disable_disk_cache = request.cache.disable_disk_cache;
  kernel_request.cache.nosave = request.cache.nosave;
  kernel_request.execution.parallel = request.execution.parallel;
  kernel_request.execution.quiet = request.execution.quiet;
  kernel_request.telemetry.enable_timing = request.telemetry.enable_timing;
  kernel_request.intent = request.intent;
  if (request.dirty_roi) {
    kernel_request.dirty_roi = *request.dirty_roi;
  }
  return kernel_request;
}

/**
 * @brief Converts backend plugin load report into a public report.
 *
 * @param report Backend plugin load report.
 * @return Public plugin load report.
 * @throws std::bad_alloc if container/string allocation fails.
 */
HostPluginLoadReport to_public_plugin_report(const PluginLoadResult& report) {
  HostPluginLoadReport out;
  out.attempted = report.attempted;
  out.loaded = report.loaded;
  out.new_op_keys = report.new_op_keys;
  out.errors.reserve(report.errors.size());
  for (const auto& error : report.errors) {
    HostPluginLoadError public_error;
    public_error.path = error.path;
    public_error.code = error.code;
    public_error.message = error.message;
    out.errors.push_back(std::move(public_error));
  }
  return out;
}

/**
 * @brief Embedded in-process Host implementation.
 *
 * @throws std::bad_alloc if adapter state allocation fails.
 * @note Public methods translate values to InteractionService and Kernel calls
 *       while keeping all implementation-only objects inside this translation
 *       unit. Per-adapter graph state is independent, while operation plugin
 *       state comes from the one process owner shared by every adapter.
 */
class EmbeddedHost final : public Host {
 public:
  /**
   * @brief Creates a Host with a fresh explicitly composed backend state.
   *
   * @param image_codec Shared artifact codec owner.
   * @param metadata_codec Shared metadata codec owner.
   * @param document_reader Shared graph/node document reader owner.
   * @param document_writer Shared graph/node document writer owner.
   * @throws std::invalid_argument when any required owner is empty.
   * @throws std::bad_alloc if backend state allocation fails.
   * @note The state owns per-Host implementation objects and outlives adapter
   *       futures captured by compute_async(). It does not own or unload the
   *       process operation plugin manager.
   */
  EmbeddedHost(std::shared_ptr<const ImageArtifactCodec> image_codec,
               std::shared_ptr<const CacheMetadataCodec> metadata_codec,
               std::shared_ptr<const GraphDocumentReader> document_reader,
               std::shared_ptr<const GraphDocumentWriter> document_writer)
      : state_(std::make_shared<EmbeddedHostState>(
            std::move(image_codec), std::move(metadata_codec),
            std::move(document_reader), std::move(document_writer))) {}

  /**
   * @brief Loads one graph through the embedded backend.
   *
   * @param request Public graph load request.
   * @return Loaded session id, duplicate/scheduler InvalidParameter,
   *         resource-ledger ComputeError, explicit-source/session-path Io,
   *         syntax/schema InvalidYaml, topology MissingDependency/Cycle, or
   *         Unknown for an unexpected internal failure.
   * @throws std::bad_alloc on allocation failure.
   * @note Empty yaml_path selects session-local-or-empty semantics; a nonempty
   *       path is explicit and never falls back. Recoverable backend and
   *       filesystem failures are converted to exact OperationStatus
   *       categories. Backend pair planning/admission and complete document
   *       validation happen before Graph-map insertion. The backend
   *       preallocates its return label before publication and this adapter
   *       moves that label into the result, so every failure leaves no newly
   *       published session or scheduler reservation.
   */
  Result<GraphSessionId> load_graph(const GraphLoadRequest& request) override {
    return guarded_result<GraphSessionId>(
        "load_graph", GraphErrc::Unknown, [&] {
          auto loaded = state_->interaction.cmd_load_graph(
              request.session.value, request.root_dir, request.yaml_path,
              request.config_path, request.cache_root_dir);
          if (!loaded) {
            return failure_result<GraphSessionId>(
                GraphErrc::InvalidParameter,
                "failed to load graph session '" + request.session.value + "'");
          }
          return success_result(GraphSessionId{std::move(*loaded)});
        });
  }

  /**
   * @brief Closes a graph after adapter-submitted async status mapping
   * completes.
   *
   * @param session Session to close.
   * @return Success, NotFound only when the graph session does not exist, or a
   *         non-NotFound failure when runtime shutdown fails before removal.
   * @throws std::bad_alloc on diagnostic allocation failure.
   * @note The adapter first marks the session closing and waits only the
   *       synchronous operations admitted before that marker, leaving the lane
   *       open for those accepted calls. It then asks Kernel to stop lane
   *       admission before waiting for pre-registered async placeholders. This
   *       ordering wakes a producer blocked by the full FIFO and prevents
   *       close-marker starvation without rejecting accepted synchronous work.
   *       Every backend-accepted async promise is made caller-visible and its
   *       status worker is joined before backend close drains/joins the lane
   *       and stops schedulers. A shutdown failure recreates one lane worker
   *       before the adapter clears the marker, so the session may be retried.
   */
  VoidResult close_graph(const GraphSessionId& session) override {
    return guarded_graph_close([&] {
      state_->begin_session_close(session);
      bool closed = false;
      try {
        state_->wait_for_session_admissions(session);
        const bool runtime_exists =
            state_->interaction.cmd_stop_graph_admission(session.value);
        state_->wait_for_session_async_compute(session);
        if (runtime_exists) {
          closed = state_->interaction.cmd_close_graph(session.value);
        }
      } catch (...) {
        state_->finish_session_close(session);
        throw;
      }
      state_->finish_session_close(session);
      if (!closed) {
        return failure_void(GraphErrc::NotFound,
                            "graph session not found: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Lists currently loaded graph sessions.
   *
   * @return Copied session labels.
   * @throws std::bad_alloc on allocation failure.
   * @note Backend exceptions are converted to a generic status failure.
   */
  Result<std::vector<GraphSessionId>> list_graphs() const override {
    return guarded_result<std::vector<GraphSessionId>>(
        "list_graphs", GraphErrc::Unknown, [&] {
          std::vector<GraphSessionId> sessions;
          const auto names = state_->interaction.cmd_list_graphs();
          sessions.reserve(names.size());
          for (const auto& name : names) {
            sessions.push_back(GraphSessionId{name});
          }
          return success_result(std::move(sessions));
        });
  }

  /**
   * @brief Reloads a graph session from YAML.
   *
   * @param session Session to reload.
   * @param yaml_path Source YAML path.
   * @return Success, NotFound for missing or closing sessions,
   *         InvalidParameter for an empty path on an existing session, Io for
   *         unreadable input, InvalidYaml for syntax/schema rejection,
   *         MissingDependency/Cycle for topology rejection, or Unknown for
   *         unexpected failures.
   * @throws std::bad_alloc if admission, graph-state submission, reload,
   *         status translation, or result construction exhausts memory.
   * @note Host admission precedes session existence testing and remains owned
   *       through the backend call and public LastError translation. Close
   *       therefore cannot erase the runtime or its diagnostic state after an
   *       accepted reload begins. Failed reload and propagated std::bad_alloc
   *       retain the published nodes, topology generation, runtime graph
   *       state, and session identity.
   */
  VoidResult reload_graph(const GraphSessionId& session,
                          const std::string& yaml_path) override {
    std::optional<EmbeddedHostState::SessionAdmissionToken> admission;
    VoidResult result = guarded_void("reload_graph", GraphErrc::Unknown, [&] {
      admission = state_->try_admit_session_operation(session);
      if (!admission) {
        return failure_void(GraphErrc::NotFound,
                            "graph session is closing: " + session.value);
      }
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::ReloadGraph,
          EmbeddedOperationTestPhase::BeforeKernelReady);
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::ReloadGraph,
          EmbeddedOperationTestPhase::BeforeKernelAdmissionSnapshot);
#endif
      if (!session_exists(*state_, session)) {
        return failure_void(GraphErrc::NotFound,
                            "graph session not found: " + session.value);
      }
      if (!state_->interaction.cmd_reload_graph_document(session.value,
                                                         yaml_path)) {
        return VoidResult{failure_from_last_error(
            *state_, session, GraphErrc::InvalidYaml,
            "failed to reload graph session: " + session.value)};
      }
      return success_void();
    });
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
    if (admission) {
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::ReloadGraph,
          EmbeddedOperationTestPhase::AfterTranslationAdmissionSnapshot);
    }
#endif
    return result;
  }

  /**
   * @brief Saves a graph session to YAML.
   *
   * @param session Session to save.
   * @param yaml_path Destination YAML path.
   * @return Success, NotFound for a missing or closing session, or Io for
   *         recoverable node serialization, YAML emission, or destination
   *         preparation/open/write/flush/close failure.
   * @throws std::bad_alloc if graph-state submission, node/YAML serialization,
   *         path handling, status translation, or result construction
   *         exhausts memory.
   * @note A lifecycle admission protects required-session resolution and the
   *       serialized save from concurrent close. Recoverable serialization and
   *       IO failures return OperationStatus. Success, returned failure, and
   *       propagated resource exhaustion all preserve graph topology, runtime
   *       state, and session ownership. The destination is not replaced
   *       atomically: failure before open preserves existing bytes, while a
   *       post-open failure may leave a created, truncated, or partially
   *       written file.
   */
  VoidResult save_graph(const GraphSessionId& session,
                        const std::string& yaml_path) override {
    std::optional<EmbeddedHostState::SessionAdmissionToken> admission;
    VoidResult result = guarded_void("save_graph", GraphErrc::Io, [&] {
      admission = state_->try_admit_session_operation(session);
      if (!admission) {
        return failure_void(GraphErrc::NotFound,
                            "graph session is closing: " + session.value);
      }
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::SaveGraph,
          EmbeddedOperationTestPhase::BeforeKernelReady);
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::SaveGraph,
          EmbeddedOperationTestPhase::BeforeKernelAdmissionSnapshot);
#endif
      state_->interaction.cmd_save_graph_document(session.value, yaml_path);
      return success_void();
    });
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
    if (admission) {
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::SaveGraph,
          EmbeddedOperationTestPhase::AfterTranslationAdmissionSnapshot);
    }
#endif
    return result;
  }

  /**
   * @brief Clears graph model state for a loaded session.
   *
   * @param session Session to clear.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note The clear request is serialized by the backend graph-state boundary.
   */
  VoidResult clear_graph(const GraphSessionId& session) override {
    return guarded_void("clear_graph", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_clear_graph(session.value)) {
        return failure_void(GraphErrc::NotFound,
                            "failed to clear graph session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Runs synchronous compute through the embedded backend.
   *
   * @param request Public compute request.
   * @return Success, NotFound for a missing or closed session, or compute
   *         failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note A lifecycle admission protects session lookup, Kernel execution, and
   *       status mapping against close. Backend LastError is used only when the
   *       compute facade reports failure for that admitted existing session.
   */
  VoidResult compute(const HostComputeRequest& request) override {
    return guarded_void("compute", GraphErrc::ComputeError, [&] {
      auto admission = state_->try_admit_session_operation(request.session);
      if (!admission) {
        return failure_void(GraphErrc::NotFound, "graph session is closing: " +
                                                     request.session.value);
      }
      if (!session_exists(*state_, request.session)) {
        return failure_void(GraphErrc::NotFound, "graph session not found: " +
                                                     request.session.value);
      }
      const auto kernel_request = to_kernel_compute_request(request);
      if (!state_->interaction.cmd_compute(kernel_request)) {
        return VoidResult{failure_from_last_error(
            *state_, request.session, GraphErrc::ComputeError,
            "compute failed for graph session: " + request.session.value)};
      }
      return success_void();
    });
  }

  /**
   * @brief Schedules async compute and tracks runtime and diagnostic lifetime.
   *
   * @param request Public compute request captured by value.
   * @return Future resolving to OperationStatus, or scheduling failure.
   * @throws std::bad_alloc on allocation failure.
   * @note Host tracking is pre-registered under the lifecycle lock, which is
   *       released before potentially blocking backend lane submission. A
   *       joined worker maps only the backend-owned exact result, fulfills the
   *       caller-visible promise, and then notifies close; it never
   *       reconstructs failure from shared LastError state.
   */
  Result<std::future<OperationStatus>> compute_async(
      HostComputeRequest request) override {
    return guarded_result<std::future<OperationStatus>>(
        "compute_async", GraphErrc::ComputeError, [&] {
          auto kernel_request = to_kernel_compute_request(request);
          GraphSessionId session = request.session;
          auto state = state_;
          auto registration = state->schedule_and_track_async_compute(
              request.session,
              [state, kernel_request = std::move(kernel_request)]() mutable {
                return state->interaction.cmd_compute_async(
                    std::move(kernel_request));
              });
          if (!registration.scheduled) {
            return failure_result<std::future<OperationStatus>>(
                GraphErrc::NotFound,
                "failed to schedule compute for graph session: " +
                    request.session.value);
          }

          std::shared_future<Kernel::AsyncComputeResult> shared_future =
              std::move(registration.future);
          const uint64_t tracking_id = registration.tracking_id;
          std::future<OperationStatus> wrapped;
          try {
            // After backend acceptance, every allocating setup stage is covered
            // by the catch path until the joined worker is attached.
            std::optional<std::promise<OperationStatus>> publication(
                std::in_place);
            wrapped = publication->get_future();
            EmbeddedHostState* const state_ptr = state.get();
            std::future<void> status_worker = std::async(
                std::launch::async,
                [state_ptr, session, tracking_id, shared_future,
                 publication = std::move(publication)]() mutable {
                  // set_value/set_exception (or reset as the defensive
                  // fallback) makes the caller-visible future ready before
                  // close is notified.
                  try {
                    publication->set_value(
                        await_async_compute_status(shared_future, session));
                  } catch (...) {
                    const std::exception_ptr failure = std::current_exception();
                    try {
                      publication->set_exception(failure);
                    } catch (...) {
                      publication.reset();
                    }
                  }
                  state_ptr->mark_async_status_published(tracking_id);
                });
            state->attach_async_status_worker(tracking_id,
                                              std::move(status_worker));
          } catch (...) {
            try {
              if (shared_future.valid()) {
                shared_future.wait();
              }
            } catch (...) {
            }
            state->remove_async_compute_tracking(tracking_id);
            throw;
          }
          return success_result(std::move(wrapped));
        });
  }

  /**
   * @brief Computes a node and returns a copied image descriptor.
   *
   * @param request Public compute request.
   * @return ImageBuffer value, a successful empty ImageBuffer when compute
   *         completes without image output, NotFound for a missing or closed
   *         session, or a compute failure status for existing sessions.
   * @throws std::bad_alloc on allocation failure.
   * @note One lifecycle admission protects session lookup, compute, empty/error
   *       classification, and public image construction against close. Backend
   *       LastError distinguishes handled failure from successful no-image
   *       output, and backend image memory is cloned before public conversion.
   */
  Result<ImageBuffer> compute_and_get_image(
      const HostComputeRequest& request) override {
    return guarded_result<ImageBuffer>(
        "compute_and_get_image", GraphErrc::ComputeError, [&] {
          auto admission = state_->try_admit_session_operation(request.session);
          if (!admission) {
            return failure_result<ImageBuffer>(
                GraphErrc::NotFound,
                "graph session is closing: " + request.session.value);
          }
          if (!session_exists(*state_, request.session)) {
            return failure_result<ImageBuffer>(
                GraphErrc::NotFound,
                "graph session not found: " + request.session.value);
          }
          const auto kernel_request = to_kernel_compute_request(request);
          auto image =
              state_->interaction.cmd_compute_and_get_image(kernel_request);
          if (!image) {
            const auto error =
                state_->interaction.cmd_last_error(request.session.value);
            if (!error) {
              return success_result(ImageBuffer{});
            }
            Result<ImageBuffer> result;
            result.status = failure_status(error->code, error->message);
            return result;
          }
          return success_result(std::move(*image));
        });
  }

  /**
   * @brief Reads timing rows for a graph session.
   *
   * @param session Session to inspect.
   * @return Timing snapshot, NotFound for missing/closing sessions, or another
   * failed status from public translation.
   * @throws std::bad_alloc on allocation failure.
   * @note One lifecycle admission keeps the session alive across Kernel access,
   * timing copy, and public result translation. Missing timing data is reported
   * as NotFound.
   */
  Result<TimingSnapshot> timing(const GraphSessionId& session) override {
    std::optional<EmbeddedHostState::SessionAdmissionToken> admission;
    Result<TimingSnapshot> result =
        guarded_result<TimingSnapshot>("timing", GraphErrc::NotFound, [&] {
          admission = state_->try_admit_session_operation(session);
          if (!admission) {
            return failure_result<TimingSnapshot>(
                GraphErrc::NotFound,
                "graph session is closing: " + session.value);
          }
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
          state_->wait_at_operation_test_phase(
              session, EmbeddedOperationTestEvent::Timing,
              EmbeddedOperationTestPhase::BeforeKernelReady);
          state_->wait_at_operation_test_phase(
              session, EmbeddedOperationTestEvent::Timing,
              EmbeddedOperationTestPhase::BeforeKernelAdmissionSnapshot);
#endif
          auto timing_result = state_->interaction.cmd_timing(session.value);
          if (!timing_result) {
            return failure_result<TimingSnapshot>(
                GraphErrc::NotFound,
                "timing not available for graph session: " + session.value);
          }
          return success_result(to_public_timing(*timing_result));
        });
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
    if (admission) {
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::Timing,
          EmbeddedOperationTestPhase::AfterTranslationAdmissionSnapshot);
    }
#endif
    return result;
  }

  /**
   * @brief Reads the backend IO-time accumulator for a graph session.
   *
   * @param session Session to inspect.
   * @return Latest IO duration in milliseconds, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note The value is copied from Kernel diagnostic state and may be zero for
   *       compute paths that performed no measured disk IO.
   */
  Result<double> last_io_time(const GraphSessionId& session) const override {
    return guarded_result<double>("last_io_time", GraphErrc::NotFound, [&] {
      auto io_time = state_->interaction.cmd_get_last_io_time(session.value);
      if (!io_time) {
        return failure_result<double>(
            GraphErrc::NotFound,
            "IO timing not available for graph session: " + session.value);
      }
      return success_result(*io_time);
    });
  }

  /**
   * @brief Reads the backend last-error snapshot.
   *
   * @param session Session to inspect.
   * @return Last failure status, or ok when no failure was recorded.
   * @throws std::bad_alloc on allocation failure.
   * @note Exceptions while reading diagnostic state become status failures.
   */
  OperationStatus last_error(const GraphSessionId& session) const override {
    return guarded_status("last_error", GraphErrc::Unknown, [&] {
      auto error = state_->interaction.cmd_last_error(session.value);
      if (!error) {
        return success_status();
      }
      return failure_status(error->code, error->message);
    });
  }

  /**
   * @brief Lists node ids for a graph session.
   *
   * @param session Session to inspect.
   * @return Copied node ids, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note The result is a point-in-time value snapshot.
   */
  Result<std::vector<NodeId>> list_node_ids(
      const GraphSessionId& session) override {
    return guarded_result<std::vector<NodeId>>(
        "list_node_ids", GraphErrc::NotFound, [&] {
          auto ids = state_->interaction.cmd_list_node_ids(session.value);
          if (!ids) {
            return failure_result<std::vector<NodeId>>(
                GraphErrc::NotFound,
                "node ids not available for graph session: " + session.value);
          }
          std::vector<NodeId> out;
          out.reserve(ids->size());
          for (int id : *ids) {
            out.push_back(NodeId{id});
          }
          return success_result(std::move(out));
        });
  }

  /**
   * @brief Lists ending node ids for a graph session.
   *
   * @param session Session to inspect.
   * @return Copied ending node ids, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Topology errors are returned as OperationStatus failures.
   */
  Result<std::vector<NodeId>> ending_nodes(
      const GraphSessionId& session) override {
    return guarded_result<std::vector<NodeId>>(
        "ending_nodes", GraphErrc::NotFound, [&] {
          auto ids = state_->interaction.cmd_ending_nodes(session.value);
          if (!ids) {
            return failure_result<std::vector<NodeId>>(
                GraphErrc::NotFound,
                "ending nodes not available for graph session: " +
                    session.value);
          }
          std::vector<NodeId> out;
          out.reserve(ids->size());
          for (int id : *ids) {
            out.push_back(NodeId{id});
          }
          return success_result(std::move(out));
        });
  }

  /**
   * @brief Serializes a node to YAML text.
   *
   * @param session Session containing the node.
   * @param node Node to serialize.
   * @return YAML text, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Serialization exceptions are converted to Host status values.
   */
  Result<std::string> get_node_yaml(const GraphSessionId& session,
                                    NodeId node) override {
    return guarded_result<std::string>(
        "get_node_yaml", GraphErrc::NotFound, [&] {
          auto yaml = state_->interaction.cmd_get_node_document(session.value,
                                                                node.value);
          if (!yaml) {
            return failure_result<std::string>(
                GraphErrc::NotFound, "node YAML not available for node " +
                                         std::to_string(node.value));
          }
          return success_result(*yaml);
        });
  }

  /**
   * @brief Replaces a node from YAML text.
   *
   * @param session Session containing the node.
   * @param node Node id to preserve.
   * @param yaml_text Replacement node YAML.
   * @return Success, NotFound for a missing/closing session or missing node,
   *         or InvalidYaml for parsing or candidate-topology validation
   *         failure.
   * @throws std::bad_alloc on allocation failure.
   * @note A lifecycle admission protects the complete Kernel call from close;
   *       Kernel performs node lookup, parsing, validation, and replacement in
   *       one graph-state work item.
   */
  VoidResult set_node_yaml(const GraphSessionId& session, NodeId node,
                           const std::string& yaml_text) override {
    std::optional<EmbeddedHostState::SessionAdmissionToken> admission;
    VoidResult result =
        guarded_void("set_node_yaml", GraphErrc::InvalidYaml, [&] {
          admission = state_->try_admit_session_operation(session);
          if (!admission) {
            return failure_void(GraphErrc::NotFound,
                                "graph session is closing: " + session.value);
          }
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
          state_->wait_at_operation_test_phase(
              session, EmbeddedOperationTestEvent::SetNodeYaml,
              EmbeddedOperationTestPhase::BeforeKernelReady);
          state_->wait_at_operation_test_phase(
              session, EmbeddedOperationTestEvent::SetNodeYaml,
              EmbeddedOperationTestPhase::BeforeKernelAdmissionSnapshot);
#endif
          state_->interaction.cmd_set_node_document(session.value, node.value,
                                                    yaml_text);
          return success_void();
        });
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
    if (admission) {
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::SetNodeYaml,
          EmbeddedOperationTestPhase::AfterTranslationAdmissionSnapshot);
    }
#endif
    return result;
  }

  /**
   * @brief Inspects one graph node.
   *
   * @param session Session containing the node.
   * @param node Node to inspect.
   * @return Public node snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Recursive parameter display-format exceptions are converted to
   * status.
   */
  Result<NodeInspectionView> inspect_node(const GraphSessionId& session,
                                          NodeId node) override {
    return guarded_result<NodeInspectionView>(
        "inspect_node", GraphErrc::NotFound, [&] {
          auto info =
              state_->interaction.cmd_inspect_node(session.value, node.value);
          if (!info) {
            return failure_result<NodeInspectionView>(
                GraphErrc::NotFound, "node inspection not available for node " +
                                         std::to_string(node.value));
          }
          return success_result(to_public_node(*info));
        });
  }

  /**
   * @brief Inspects all nodes in one graph session.
   *
   * @param session Session to inspect.
   * @return Public graph snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Serialization failures in node parameters do not escape the Host
   *       boundary.
   */
  Result<GraphInspectionView> inspect_graph(
      const GraphSessionId& session) override {
    return guarded_result<GraphInspectionView>(
        "inspect_graph", GraphErrc::NotFound, [&] {
          auto snapshot = state_->interaction.cmd_inspect_graph(session.value);
          if (!snapshot) {
            return failure_result<GraphInspectionView>(
                GraphErrc::NotFound,
                "graph inspection not available for session: " + session.value);
          }
          return success_result(to_public_graph_view(session, *snapshot));
        });
  }

  /**
   * @brief Builds a dependency-tree snapshot.
   *
   * @param session Session to inspect.
   * @param node Optional start node.
   * @param include_metadata Whether metadata should be copied.
   * @return Public dependency tree, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Backend topology and metadata objects are copied into public values.
   */
  Result<HostDependencyTreeSnapshot> dependency_tree(
      const GraphSessionId& session, std::optional<NodeId> node,
      bool include_metadata) override {
    return guarded_result<HostDependencyTreeSnapshot>(
        "dependency_tree", GraphErrc::NotFound, [&] {
          std::optional<int> node_id;
          if (node) {
            node_id = node->value;
          }
          auto tree = state_->interaction.cmd_dependency_tree(
              session.value, node_id, include_metadata);
          if (!tree) {
            return failure_result<HostDependencyTreeSnapshot>(
                GraphErrc::NotFound,
                "dependency tree not available for session: " + session.value);
          }
          return success_result(to_public_dependency_tree(*tree));
        });
  }

  /**
   * @brief Returns traversal orders keyed by ending node.
   *
   * @param session Session to inspect.
   * @return Public traversal order map, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Node ids are copied values and do not expose mutable topology state.
   */
  Result<std::map<int, std::vector<NodeId>>> traversal_orders(
      const GraphSessionId& session) override {
    return guarded_result<std::map<int, std::vector<NodeId>>>(
        "traversal_orders", GraphErrc::NotFound, [&] {
          auto orders = state_->interaction.cmd_traversal_orders(session.value);
          if (!orders) {
            return failure_result<std::map<int, std::vector<NodeId>>>(
                GraphErrc::NotFound,
                "traversal orders not available for session: " + session.value);
          }
          return success_result(to_public_traversal_orders(*orders));
        });
  }

  /**
   * @brief Returns traversal details keyed by ending node.
   *
   * @param session Session to inspect.
   * @return Public traversal metadata map, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Cache flags are observational and may become stale after mutation.
   */
  Result<std::map<int, std::vector<HostTraversalNodeSnapshot>>>
  traversal_details(const GraphSessionId& session) override {
    return guarded_result<
        std::map<int, std::vector<HostTraversalNodeSnapshot>>>(
        "traversal_details", GraphErrc::NotFound, [&] {
          auto details =
              state_->interaction.cmd_traversal_details(session.value);
          if (!details) {
            return failure_result<
                std::map<int, std::vector<HostTraversalNodeSnapshot>>>(
                GraphErrc::NotFound,
                "traversal details not available for session: " +
                    session.value);
          }
          return success_result(to_public_traversal_details(*details));
        });
  }

  /**
   * @brief Finds ending-node trees that contain a node.
   *
   * @param session Session to inspect.
   * @param node Node to search for.
   * @return Ending node ids, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note The query is converted to copied public node ids.
   */
  Result<std::vector<NodeId>> trees_containing_node(
      const GraphSessionId& session, NodeId node) override {
    return guarded_result<std::vector<NodeId>>(
        "trees_containing_node", GraphErrc::NotFound, [&] {
          auto roots = state_->interaction.cmd_trees_containing_node(
              session.value, node.value);
          if (!roots) {
            return failure_result<std::vector<NodeId>>(
                GraphErrc::NotFound,
                "trees not available for node " + std::to_string(node.value));
          }
          std::vector<NodeId> out;
          out.reserve(roots->size());
          for (int id : *roots) {
            out.push_back(NodeId{id});
          }
          return success_result(std::move(out));
        });
  }

  /**
   * @brief Projects a source ROI forward to a target node.
   *
   * @param session Session containing the graph.
   * @param start_node Source node.
   * @param start_roi Source ROI in public coordinates.
   * @param target_node Target node.
   * @return Projected ROI, NotFound for a missing/closing session or endpoint,
   *         or InvalidParameter when existing endpoints produce no projection.
   * @throws std::bad_alloc on allocation failure.
   * @note OpenCV rectangles remain implementation-local. A lifecycle admission
   *       protects the complete required endpoint lookup and projection from
   *       concurrent close.
   */
  Result<PixelRect> project_roi(const GraphSessionId& session,
                                NodeId start_node, const PixelRect& start_roi,
                                NodeId target_node) override {
    std::optional<EmbeddedHostState::SessionAdmissionToken> admission;
    Result<PixelRect> result = guarded_result<PixelRect>(
        "project_roi", GraphErrc::InvalidParameter, [&] {
          admission = state_->try_admit_session_operation(session);
          if (!admission) {
            return failure_result<PixelRect>(
                GraphErrc::NotFound,
                "graph session is closing: " + session.value);
          }
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
          state_->wait_at_operation_test_phase(
              session, EmbeddedOperationTestEvent::ForwardRoiProjection,
              EmbeddedOperationTestPhase::BeforeKernelReady);
          state_->wait_at_operation_test_phase(
              session, EmbeddedOperationTestEvent::ForwardRoiProjection,
              EmbeddedOperationTestPhase::BeforeKernelAdmissionSnapshot);
#endif
          auto roi = state_->interaction.cmd_project_roi(
              session.value, start_node.value, start_roi, target_node.value);
          if (!roi) {
            return failure_result<PixelRect>(
                GraphErrc::InvalidParameter,
                "failed to project ROI for session: " + session.value);
          }
          return success_result(*roi);
        });
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
    if (admission) {
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::ForwardRoiProjection,
          EmbeddedOperationTestPhase::AfterTranslationAdmissionSnapshot);
    }
#endif
    return result;
  }

  /**
   * @brief Projects a target ROI backward to a source node.
   *
   * @param session Session containing the graph.
   * @param target_node Target node.
   * @param target_roi Target ROI in public coordinates.
   * @param source_node Source node.
   * @return Projected source ROI, NotFound for a missing/closing session or
   *         endpoint, or InvalidParameter when existing endpoints produce no
   *         projection.
   * @throws std::bad_alloc on allocation failure.
   * @note A lifecycle admission protects the complete required endpoint lookup
   *       and projection from concurrent close. Ordinary no-projection results
   *       remain InvalidParameter.
   */
  Result<PixelRect> project_roi_backward(const GraphSessionId& session,
                                         NodeId target_node,
                                         const PixelRect& target_roi,
                                         NodeId source_node) override {
    std::optional<EmbeddedHostState::SessionAdmissionToken> admission;
    Result<PixelRect> result = guarded_result<PixelRect>(
        "project_roi_backward", GraphErrc::InvalidParameter, [&] {
          admission = state_->try_admit_session_operation(session);
          if (!admission) {
            return failure_result<PixelRect>(
                GraphErrc::NotFound,
                "graph session is closing: " + session.value);
          }
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
          state_->wait_at_operation_test_phase(
              session, EmbeddedOperationTestEvent::BackwardRoiProjection,
              EmbeddedOperationTestPhase::BeforeKernelReady);
          state_->wait_at_operation_test_phase(
              session, EmbeddedOperationTestEvent::BackwardRoiProjection,
              EmbeddedOperationTestPhase::BeforeKernelAdmissionSnapshot);
#endif
          auto roi = state_->interaction.cmd_project_roi_backward(
              session.value, target_node.value, target_roi, source_node.value);
          if (!roi) {
            return failure_result<PixelRect>(
                GraphErrc::InvalidParameter,
                "failed to project ROI backward for session: " + session.value);
          }
          return success_result(*roi);
        });
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
    if (admission) {
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::BackwardRoiProjection,
          EmbeddedOperationTestPhase::AfterTranslationAdmissionSnapshot);
    }
#endif
    return result;
  }

  /**
   * @brief Reads the latest dirty-region snapshot.
   *
   * @param session Session to inspect.
   * @return Public dirty-region snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Loaded sessions without dirty state return an empty successful
   *       snapshot.
   */
  Result<DirtyRegionInspectionSnapshot> dirty_region_snapshot(
      const GraphSessionId& session) override {
    return guarded_result<DirtyRegionInspectionSnapshot>(
        "dirty_region_snapshot", GraphErrc::NotFound, [&] {
          auto snapshot =
              state_->interaction.cmd_dirty_region_snapshot(session.value);
          if (!snapshot) {
            if (session_exists(*state_, session)) {
              return success_result(DirtyRegionInspectionSnapshot{});
            }
            return failure_result<DirtyRegionInspectionSnapshot>(
                GraphErrc::NotFound,
                "dirty-region snapshot not available for session: " +
                    session.value);
          }
          return success_result(to_public_dirty_snapshot(*snapshot));
        });
  }

  /**
   * @brief Reads the latest compute planning snapshot for a graph session.
   *
   * @param session Session to inspect.
   * @return Public optional planning snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing sessions fail with NotFound; loaded sessions without compute
   *       history return an empty optional with success status.
   */
  Result<std::optional<ComputePlanningInspectionSnapshot>>
  compute_planning_snapshot(const GraphSessionId& session) override {
    return guarded_result<std::optional<ComputePlanningInspectionSnapshot>>(
        "compute_planning_snapshot", GraphErrc::NotFound, [&] {
          auto snapshot =
              state_->interaction.cmd_compute_planning_snapshot(session.value);
          if (!snapshot) {
            if (session_exists(*state_, session)) {
              return success_result(
                  std::optional<ComputePlanningInspectionSnapshot>{});
            }
            return failure_result<
                std::optional<ComputePlanningInspectionSnapshot>>(
                GraphErrc::NotFound,
                "compute planning snapshot not available for session: " +
                    session.value);
          }
          return success_result(
              std::optional<ComputePlanningInspectionSnapshot>(
                  to_public_planning_snapshot(*snapshot)));
        });
  }

  /**
   * @brief Reads recent compute planning snapshots for a graph session.
   *
   * @param session Session to inspect.
   * @return Public bounded planning snapshot history, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Empty history is a successful loaded-session state before compute.
   */
  Result<std::vector<ComputePlanningInspectionSnapshot>>
  recent_compute_planning_snapshots(const GraphSessionId& session) override {
    return guarded_result<std::vector<ComputePlanningInspectionSnapshot>>(
        "recent_compute_planning_snapshots", GraphErrc::NotFound, [&] {
          auto snapshots =
              state_->interaction.cmd_recent_compute_planning_snapshots(
                  session.value);
          if (!snapshots) {
            return failure_result<
                std::vector<ComputePlanningInspectionSnapshot>>(
                GraphErrc::NotFound,
                "compute planning history not available for session: " +
                    session.value);
          }
          return success_result(to_public_planning_snapshots(*snapshots));
        });
  }

  /**
   * @brief Begins a dirty-source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Dirty source node.
   * @param domain Dirty domain.
   * @param source_roi Source-local ROI.
   * @return Updated dirty-region snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing sessions are reported as NotFound before entering the
   *       backend; missing nodes and invalid ROIs preserve backend LastError
   *       classifications.
   */
  Result<DirtyRegionInspectionSnapshot> begin_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi) override {
    return guarded_result<DirtyRegionInspectionSnapshot>(
        "begin_dirty_source", GraphErrc::InvalidParameter, [&] {
          if (!session_exists(*state_, session)) {
            return failure_result<DirtyRegionInspectionSnapshot>(
                GraphErrc::NotFound,
                "graph session not found: " + session.value);
          }
          auto snapshot = state_->interaction.cmd_begin_dirty_source(
              session.value, node.value, to_compute_dirty_domain(domain),
              source_roi);
          if (!snapshot) {
            Result<DirtyRegionInspectionSnapshot> result;
            result.status = failure_from_last_error(
                *state_, session, GraphErrc::InvalidParameter,
                "failed to begin dirty source for node " +
                    std::to_string(node.value));
            return result;
          }
          return success_result(to_public_dirty_snapshot(*snapshot));
        });
  }

  /**
   * @brief Updates a dirty-source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Dirty source node.
   * @param domain Dirty domain.
   * @param source_roi Additional source-local ROI.
   * @return Updated dirty-region snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing sessions are reported as NotFound before entering the
   *       backend; missing nodes and invalid ROIs preserve backend LastError
   *       classifications.
   */
  Result<DirtyRegionInspectionSnapshot> update_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi) override {
    return guarded_result<DirtyRegionInspectionSnapshot>(
        "update_dirty_source", GraphErrc::InvalidParameter, [&] {
          if (!session_exists(*state_, session)) {
            return failure_result<DirtyRegionInspectionSnapshot>(
                GraphErrc::NotFound,
                "graph session not found: " + session.value);
          }
          auto snapshot = state_->interaction.cmd_update_dirty_source(
              session.value, node.value, to_compute_dirty_domain(domain),
              source_roi);
          if (!snapshot) {
            Result<DirtyRegionInspectionSnapshot> result;
            result.status = failure_from_last_error(
                *state_, session, GraphErrc::InvalidParameter,
                "failed to update dirty source for node " +
                    std::to_string(node.value));
            return result;
          }
          return success_result(to_public_dirty_snapshot(*snapshot));
        });
  }

  /**
   * @brief Ends a dirty-source lifecycle event.
   *
   * @param session Session containing the graph.
   * @param node Dirty source node.
   * @param domain Dirty domain.
   * @return Updated dirty-region snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing sessions are reported as NotFound before entering the
   *       backend; missing nodes preserve backend LastError classifications.
   */
  Result<DirtyRegionInspectionSnapshot> end_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain) override {
    return guarded_result<DirtyRegionInspectionSnapshot>(
        "end_dirty_source", GraphErrc::InvalidParameter, [&] {
          if (!session_exists(*state_, session)) {
            return failure_result<DirtyRegionInspectionSnapshot>(
                GraphErrc::NotFound,
                "graph session not found: " + session.value);
          }
          auto snapshot = state_->interaction.cmd_end_dirty_source(
              session.value, node.value, to_compute_dirty_domain(domain));
          if (!snapshot) {
            Result<DirtyRegionInspectionSnapshot> result;
            result.status = failure_from_last_error(
                *state_, session, GraphErrc::InvalidParameter,
                "failed to end dirty source for node " +
                    std::to_string(node.value));
            return result;
          }
          return success_result(to_public_dirty_snapshot(*snapshot));
        });
  }

  /**
   * @brief Drains compute events for a graph session.
   *
   * @param session Session whose event buffer should be drained.
   * @param limit Maximum number of oldest retained events to remove.
   * @return Public bounded event batch, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Invalid limits fail before graph lookup or backend mutation. A
   *       successful call removes only the returned page and resets the shared
   *       drop count at the same locked observation point.
   */
  Result<ComputeEventBatch> drain_compute_events(const GraphSessionId& session,
                                                 std::size_t limit) override {
    return guarded_result<ComputeEventBatch>(
        "drain_compute_events", GraphErrc::InvalidParameter, [&] {
          if (limit < kComputeEventDrainMinLimit ||
              limit > kComputeEventDrainMaxLimit) {
            return failure_result<ComputeEventBatch>(
                GraphErrc::InvalidParameter,
                "compute-event drain limit must be between " +
                    std::to_string(kComputeEventDrainMinLimit) + " and " +
                    std::to_string(kComputeEventDrainMaxLimit));
          }
          auto batch = state_->interaction.cmd_drain_compute_events(
              session.value, limit);
          if (!batch) {
            return failure_result<ComputeEventBatch>(
                GraphErrc::NotFound,
                "compute events not available for session: " + session.value);
          }
          return success_result(std::move(*batch));
        });
  }

  /**
   * @brief Reads scheduler trace events for a graph session.
   *
   * @param session Session to inspect.
   * @param after_sequence Exclusive sequence cursor.
   * @param limit Maximum number of trace entries to copy.
   * @return Public bounded scheduler trace page, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Invalid bounds fail before graph lookup. Trace events are copied
   *       non-destructively and do not expose scheduler queues.
   */
  Result<SchedulerTracePage> scheduler_trace(const GraphSessionId& session,
                                             uint64_t after_sequence,
                                             std::size_t limit) override {
    return guarded_result<SchedulerTracePage>(
        "scheduler_trace", GraphErrc::InvalidParameter, [&] {
          if (limit < kSchedulerTraceMinLimit ||
              limit > kSchedulerTraceMaxLimit) {
            return failure_result<SchedulerTracePage>(
                GraphErrc::InvalidParameter,
                "scheduler-trace limit must be between " +
                    std::to_string(kSchedulerTraceMinLimit) + " and " +
                    std::to_string(kSchedulerTraceMaxLimit));
          }
          auto page = state_->interaction.cmd_scheduler_trace(
              session.value, after_sequence, limit);
          if (!page) {
            return failure_result<SchedulerTracePage>(
                GraphErrc::NotFound,
                "scheduler trace not available for session: " + session.value);
          }
          return success_result(to_public_scheduler_trace_page(*page));
        });
  }

  /**
   * @brief Clears all cache layers for a graph session.
   *
   * @param session Session whose caches should be cleared.
   * @return Success, or NotFound for a missing or closing session.
   * @throws std::bad_alloc on allocation failure.
   * @note One lifecycle admission keeps the session alive across Kernel cache
   * mutation and public status translation. Cache service exceptions are
   * converted to Host status values.
   */
  VoidResult clear_cache(const GraphSessionId& session) override {
    std::optional<EmbeddedHostState::SessionAdmissionToken> admission;
    VoidResult result = guarded_void("clear_cache", GraphErrc::NotFound, [&] {
      admission = state_->try_admit_session_operation(session);
      if (!admission) {
        return failure_void(GraphErrc::NotFound,
                            "graph session is closing: " + session.value);
      }
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::ClearCache,
          EmbeddedOperationTestPhase::BeforeKernelReady);
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::ClearCache,
          EmbeddedOperationTestPhase::BeforeKernelAdmissionSnapshot);
#endif
      if (!state_->interaction.cmd_clear_cache(session.value)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to clear cache for session: " + session.value);
      }
      return success_void();
    });
#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
    if (admission) {
      state_->wait_at_operation_test_phase(
          session, EmbeddedOperationTestEvent::ClearCache,
          EmbeddedOperationTestPhase::AfterTranslationAdmissionSnapshot);
    }
#endif
    return result;
  }

  /**
   * @brief Clears disk cache for a graph session.
   *
   * @param session Session whose disk cache should be cleared.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Detailed cache counts are not exposed by this Host slice.
   */
  VoidResult clear_drive_cache(const GraphSessionId& session) override {
    return guarded_void("clear_drive_cache", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_clear_drive_cache(session.value)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to clear drive cache for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Clears memory cache for a graph session.
   *
   * @param session Session whose memory cache should be cleared.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Persistent graph and disk cache state are left intact.
   */
  VoidResult clear_memory_cache(const GraphSessionId& session) override {
    return guarded_void("clear_memory_cache", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_clear_memory_cache(session.value)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to clear memory cache for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Saves all node caches for a graph session.
   *
   * @param session Session whose nodes should be cached.
   * @param precision Cache precision label.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Precision handling is delegated to the backend cache service.
   */
  VoidResult cache_all_nodes(const GraphSessionId& session,
                             const std::string& precision) override {
    return guarded_void("cache_all_nodes", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_cache_all_nodes(session.value, precision)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to cache nodes for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Releases transient memory cache for a graph session.
   *
   * @param session Session whose transient memory should be freed.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note The graph remains loaded after transient memory is released.
   */
  VoidResult free_transient_memory(const GraphSessionId& session) override {
    return guarded_void("free_transient_memory", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_free_transient_memory(session.value)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to free transient memory for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Synchronizes disk cache for a graph session.
   *
   * @param session Session whose cache should be synchronized.
   * @param precision Cache precision label.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Synchronization failures are mapped to status values.
   */
  VoidResult synchronize_disk_cache(const GraphSessionId& session,
                                    const std::string& precision) override {
    return guarded_void("synchronize_disk_cache", GraphErrc::NotFound, [&] {
      if (!state_->interaction.cmd_synchronize_disk_cache(session.value,
                                                          precision)) {
        return failure_void(
            GraphErrc::NotFound,
            "failed to synchronize disk cache for session: " + session.value);
      }
      return success_void();
    });
  }

  /**
   * @brief Loads operation plugins and returns a structured report.
   *
   * @param dirs Directories or glob-like inputs to scan.
   * @return Public plugin load report, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Successful handles stay in the process-global plugin owner and remain
   *       visible after this adapter is destroyed.
   */
  Result<HostPluginLoadReport> plugins_load_report(
      const std::vector<std::string>& dirs) override {
    return guarded_result<HostPluginLoadReport>(
        "plugins_load_report", GraphErrc::Io, [&] {
          return success_result(to_public_plugin_report(
              state_->interaction.cmd_plugins_load_report(dirs)));
        });
  }

  /**
   * @brief Loads operation plugins and returns status only.
   *
   * @param dirs Directories or glob-like inputs to scan.
   * @return Success or first plugin-load failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note This method preserves the status-only frontend contract while
   *       mutating the same process owner as every other Host.
   */
  VoidResult plugins_load(const std::vector<std::string>& dirs) override {
    return guarded_void("plugins_load", GraphErrc::Io, [&] {
      const auto report = state_->interaction.cmd_plugins_load_report(dirs);
      if (!report.errors.empty()) {
        return failure_void(report.errors.front().code,
                            report.errors.front().message);
      }
      return success_void();
    });
  }

  /**
   * @brief Unloads all process-global operation plugins.
   *
   * @return Number of active operation keys removed or restored, or a failed
   *         status.
   * @throws std::bad_alloc on allocation failure.
   * @note Every Host observes the registry/source mutation. Copied callbacks
   *       and returned values retain their library lease until destruction.
   */
  Result<int> plugins_unload_all() override {
    return guarded_result<int>("plugins_unload_all", GraphErrc::Io, [&] {
      return success_result(state_->interaction.cmd_plugins_unload_all());
    });
  }

  /**
   * @brief Seeds built-in operation source labels.
   *
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Built-in callback registration runs once process-wide; later calls
   *       reconcile source metadata without replaying over plugin overrides.
   */
  VoidResult seed_builtin_ops() override {
    return guarded_void("seed_builtin_ops", GraphErrc::Unknown, [&] {
      state_->interaction.cmd_seed_builtin_ops();
      return success_void();
    });
  }

  /**
   * @brief Lists operation source labels.
   *
   * @return Source labels keyed by operation key, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Returned labels are copied diagnostics, not plugin handles.
   */
  Result<std::map<std::string, std::string>> ops_sources() const override {
    return guarded_result<std::map<std::string, std::string>>(
        "ops_sources", GraphErrc::Unknown,
        [&] { return success_result(state_->interaction.cmd_ops_sources()); });
  }

  /**
   * @brief Lists combined operation keys.
   *
   * @return Combined operation keys, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Combined keys are suitable for frontend operation pickers.
   */
  Result<std::vector<std::string>> ops_combined_keys() const override {
    return guarded_result<std::vector<std::string>>(
        "ops_combined_keys", GraphErrc::Unknown, [&] {
          return success_result(state_->interaction.cmd_ops_combined_keys());
        });
  }

  /**
   * @brief Lists source labels for combined operation keys.
   *
   * @return Source labels keyed by combined operation key, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Missing source labels default through backend helper behavior.
   */
  Result<std::map<std::string, std::string>> ops_combined_sources()
      const override {
    return guarded_result<std::map<std::string, std::string>>(
        "ops_combined_sources", GraphErrc::Unknown, [&] {
          return success_result(state_->interaction.cmd_ops_combined_sources());
        });
  }

  /**
   * @brief Lists available scheduler type names.
   *
   * @return Scheduler type names, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Built-in and plugin-provided scheduler names are copied.
   */
  Result<std::vector<std::string>> scheduler_available_types() const override {
    return guarded_result<std::vector<std::string>>(
        "scheduler_available_types", GraphErrc::Unknown, [&] {
          return success_result(
              state_->interaction.cmd_scheduler_available_types());
        });
  }

  /**
   * @brief Reads a scheduler description.
   *
   * @param type_name Scheduler type name.
   * @return Description text, or NotFound when the scheduler type is
   * unavailable.
   * @throws std::bad_alloc on allocation failure.
   * @note The adapter checks the available type list before calling the backend
   *       description helper because that helper has a display fallback string.
   */
  Result<std::string> scheduler_description(
      const std::string& type_name) const override {
    return guarded_result<std::string>(
        "scheduler_description", GraphErrc::NotFound, [&] {
          const auto types =
              state_->interaction.cmd_scheduler_available_types();
          if (std::find(types.begin(), types.end(), type_name) == types.end()) {
            return failure_result<std::string>(
                GraphErrc::NotFound, "scheduler type not found: " + type_name);
          }
          return success_result(
              state_->interaction.cmd_scheduler_description(type_name));
        });
  }

  /**
   * @brief Scans directories for scheduler plugins.
   *
   * @param dirs Directories to scan.
   * @return Number of loaded scheduler types, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Plugin load exceptions are normalized to Host status values.
   */
  Result<size_t> scheduler_scan(const std::vector<std::string>& dirs) override {
    return guarded_result<size_t>("scheduler_scan", GraphErrc::Io, [&] {
      return success_result(state_->interaction.cmd_scheduler_scan(dirs));
    });
  }

  /**
   * @brief Loads one scheduler plugin.
   *
   * @param path Dynamic library path.
   * @return Success or failure status.
   * @throws std::bad_alloc on allocation failure.
   * @note Failed loads return Io status instead of throwing through Host.
   */
  VoidResult scheduler_load(const std::string& path) override {
    return guarded_void("scheduler_load", GraphErrc::Io, [&] {
      if (!state_->interaction.cmd_scheduler_load(path)) {
        return failure_void(GraphErrc::Io,
                            "failed to load scheduler plugin: " + path);
      }
      return success_void();
    });
  }

  /**
   * @brief Lists loaded scheduler plugin labels.
   *
   * @return Plugin labels, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note Labels are diagnostic strings copied from backend state.
   */
  Result<std::vector<std::string>> scheduler_loaded_plugins() const override {
    return guarded_result<std::vector<std::string>>(
        "scheduler_loaded_plugins", GraphErrc::Unknown, [&] {
          return success_result(
              state_->interaction.cmd_scheduler_loaded_plugins());
        });
  }

  /**
   * @brief Applies scheduler defaults for subsequently loaded graph sessions.
   *
   * @param config Public scheduler default values.
   * @return Success, or InvalidParameter when the worker request exceeds the
   *         public hard maximum.
   * @throws std::bad_alloc if scheduler type strings allocate while copied.
   * @note Validation precedes candidate construction and the single Kernel
   *       assignment. Existing runtime schedulers and previously accepted
   *       future-session defaults remain unchanged on rejection.
   */
  VoidResult configure_scheduler_defaults(
      const HostSchedulerConfig& config) override {
    return guarded_void(
        "configure_scheduler_defaults", GraphErrc::InvalidParameter, [&] {
          if (config.worker_count > kSchedulerWorkerRequestMax) {
            return failure_void(GraphErrc::InvalidParameter,
                                "scheduler worker count exceeds the maximum "
                                "of " +
                                    std::to_string(kSchedulerWorkerRequestMax));
          }
          Kernel::SchedulerConfig backend_config;
          backend_config.hp_type = config.hp_type;
          backend_config.rt_type = config.rt_type;
          backend_config.worker_count = config.worker_count;
          state_->kernel.set_scheduler_config(backend_config);
          return success_void();
        });
  }

  /**
   * @brief Reads scheduler information for a graph intent.
   *
   * @param session Session to inspect.
   * @param intent Compute intent served by the scheduler.
   * @return Scheduler info snapshot, or a failed status.
   * @throws std::bad_alloc on allocation failure.
   * @note One lifecycle admission protects the complete Kernel call against
   *       close. Kernel copies name/stats under the graph-state boundary shared
   *       with compute and replacement; no scheduler pointer escapes.
   */
  Result<SchedulerInfoSnapshot> scheduler_info(
      const GraphSessionId& session, ComputeIntent intent) const override {
    return guarded_result<SchedulerInfoSnapshot>(
        "scheduler_info", GraphErrc::NotFound, [&] {
          auto admission = state_->try_admit_session_operation(session);
          if (!admission) {
            return failure_result<SchedulerInfoSnapshot>(
                GraphErrc::NotFound,
                "graph session is closing: " + session.value);
          }
          const auto info =
              state_->kernel.get_scheduler_info(session.value, intent);
          if (!info) {
            return failure_result<SchedulerInfoSnapshot>(
                GraphErrc::NotFound,
                "scheduler info not available for session: " + session.value);
          }
          SchedulerInfoSnapshot snapshot;
          snapshot.intent = intent;
          snapshot.scheduler_name = info->first;
          snapshot.stats = info->second;
          return success_result(std::move(snapshot));
        });
  }

  /**
   * @brief Replaces the scheduler for one graph intent.
   *
   * @param session Session to update.
   * @param intent Compute intent whose scheduler is replaced.
   * @param type Scheduler type name.
   * @return Success, NotFound for a missing or closed session, or
   *         InvalidParameter for an unavailable/null scheduler type or handled
   *         candidate failure, or ComputeError when process capacity cannot
   *         reserve candidate headroom.
   * @throws std::bad_alloc on allocation failure.
   * @note One lifecycle admission covers session validation through status
   *       mapping. Kernel performs plan, single reservation, construction, and
   *       strong replacement under the graph-state boundary shared with
   *       compute/info/close. The Host session pre-check reserves NotFound for
   *       lifecycle absence, while guarded GraphError mapping preserves only
   *       resource exhaustion without releasing the prior owner; other
   *       candidate failures keep the established InvalidParameter result.
   */
  VoidResult replace_scheduler(const GraphSessionId& session,
                               ComputeIntent intent,
                               const std::string& type) override {
    return guarded_void("replace_scheduler", GraphErrc::InvalidParameter, [&] {
      auto admission = state_->try_admit_session_operation(session);
      if (!admission) {
        return failure_void(GraphErrc::NotFound,
                            "graph session is closing: " + session.value);
      }
      if (!session_exists(*state_, session)) {
        return failure_void(GraphErrc::NotFound,
                            "graph session not found: " + session.value);
      }
      if (!state_->kernel.replace_scheduler(session.value, intent, type)) {
        return failure_void(
            GraphErrc::InvalidParameter,
            "failed to replace scheduler for session: " + session.value);
      }
      return success_void();
    });
  }

 private:
  /** @brief Shared backend state owned by this Host and its joined workers. */
  std::shared_ptr<EmbeddedHostState> state_;
};

}  // namespace

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/**
 * @brief Installs or clears the deterministic lifecycle-coordination hook.
 * @param hook Hook that outlives all concurrent lifecycle callbacks, or
 *             nullptr.
 * @return Nothing.
 * @throws Nothing.
 * @note Tests must serialize installation and clear the hook before its context
 *       is destroyed. Production builds contain no hook storage or calls.
 */
void set_embedded_host_lifecycle_test_hook(
    const EmbeddedLifecycleTestHook* hook) noexcept {
  g_embedded_lifecycle_test_hook.store(hook, std::memory_order_release);
}
#endif

#if defined(PHOTOSPIDER_INTERNAL_HOST_OPERATION_TESTING)
/**
 * @brief Installs or clears the deterministic Host-operation test hook.
 * @param hook Hook that outlives all affected operation callbacks, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 * @note Tests serialize installation and clear the hook only after every
 *       operation future has completed. Production builds contain no symbol.
 */
void set_embedded_host_operation_test_hook(
    const EmbeddedOperationTestHook* hook) noexcept {
  g_embedded_operation_test_hook.store(hook, std::memory_order_release);
}
#endif

/** @copydoc ps::internal::create_embedded_host_with_dependencies */
std::unique_ptr<Host> internal::create_embedded_host_with_dependencies(
    std::shared_ptr<const ImageArtifactCodec> image_codec,
    std::shared_ptr<const CacheMetadataCodec> metadata_codec,
    std::shared_ptr<const GraphDocumentReader> document_reader,
    std::shared_ptr<const GraphDocumentWriter> document_writer) {
  return std::make_unique<EmbeddedHost>(
      std::move(image_codec), std::move(metadata_codec),
      std::move(document_reader), std::move(document_writer));
}

/** @copydoc ps::create_embedded_host */
std::unique_ptr<Host> create_embedded_host() {
  providers::ConfiguredPersistenceAdapters persistence =
      providers::make_configured_persistence_adapters();
  return internal::create_embedded_host_with_dependencies(
      providers::make_configured_image_artifact_codec(),
      std::move(persistence.metadata_codec),
      std::move(persistence.document_reader),
      std::move(persistence.document_writer));
}

}  // namespace ps
