/**
 * @file run_lifecycle_registry.hpp
 * @brief Declares process-owned Graph/Run lifecycle admission and settlement.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "compute/compute_run.hpp"
#include "compute/execution_lifecycle_telemetry.hpp"
#include "compute/run_group.hpp"
#include "graph/graph_revision.hpp"

namespace ps::testing {
class RunLifecycleRegistryTestAccess;
}  // namespace ps::testing

namespace ps::compute {

class DirtySiblingCommitGate;
class RunLifecycleRegistry;
struct RunLifecycleAdmissionCandidateControl;
struct RunLifecycleAdmissionHandleControl;

/**
 * @brief Preallocated one-generation Graph close completion coordinator.
 *
 * One complete GraphRuntime creates this record before registry publication.
 * The first close caller owns backend progression; concurrent callers join the
 * same monotonic success. No ordinary Run failure can reset the record.
 *
 * @throws std::system_error when synchronization primitives fail.
 * @note This record owns no Graph row, Run, lane, route, resource, callback, or
 * process shutdown authority.
 */
class GraphCloseCoordinator final {
 public:
  /**
   * @brief Distinguishes the one backend owner from live joiners.
   * @throws Nothing for value construction and comparison.
   */
  enum class Role {
    /** @brief Caller must perform the exact Kernel close progression. */
    Owner,
    /** @brief Caller waits for the already selected owner. */
    Joiner,
  };

  /**
   * @brief Preallocates an idle generation record.
   * @throws Nothing.
   */
  GraphCloseCoordinator() noexcept = default;

  /** @brief Prevents duplicating one close owner/result. */
  GraphCloseCoordinator(const GraphCloseCoordinator&) = delete;
  /** @brief Prevents assigning one close owner/result. */
  GraphCloseCoordinator& operator=(const GraphCloseCoordinator&) = delete;

  /**
   * @brief Selects the backend owner or joins the live generation.
   * @return Owner once, otherwise Joiner.
   * @throws std::system_error when record locking fails.
   * @note Selection is monotonic and allocates nothing.
   */
  Role begin();

  /**
   * @brief Publishes successful completion for every selected joiner.
   * @return Nothing.
   * @throws std::logic_error before owner selection or on duplicate completion.
   * @throws std::system_error when record locking fails.
   */
  void complete_success();

  /**
   * @brief Waits without a finite bound for the owner result.
   * @return Nothing after successful completion.
   * @throws std::logic_error before any owner was selected.
   * @throws std::system_error when record waiting fails.
   * @note A nonreturning entered callback honestly leaves this wait unbounded.
   */
  void wait_for_success();

  /**
   * @brief Reports whether backend progression has been selected.
   * @return True after the first begin().
   * @throws std::system_error when record locking fails.
   */
  bool started() const;

  /**
   * @brief Reports whether the generation published success.
   * @return True after complete_success().
   * @throws std::system_error when record locking fails.
   */
  bool completed() const;

 private:
  /** @brief Serializes selection and result publication. */
  mutable std::mutex mutex_;
  /** @brief Wakes every live generation joiner. */
  std::condition_variable cv_;
  /** @brief True after exactly one caller becomes Owner. */
  bool started_ = false;
  /** @brief True after the owner publishes monotonic success. */
  bool completed_ = false;
};

/**
 * @brief Stable lifetime root created with one complete GraphRuntime.
 *
 * Registry candidates retain leases to this anchor from their first admission
 * check through commit or rollback. The close coordinator remains alive after
 * registry-row removal until both Graph-owned lanes and runtime state retire.
 *
 * @throws std::bad_alloc when shared close-record ownership cannot allocate.
 * @note The anchor owns no GraphModel, Run, plan, staged output, resource
 * token, route, worker, plugin, or service state.
 */
class GraphLifetimeAnchor final {
 public:
  /**
   * @brief Creates one anchor for an exact live Graph identity.
   * @param graph_instance_id Nonzero Graph identity.
   * @throws std::bad_alloc when the preallocated close record cannot allocate.
   */
  explicit GraphLifetimeAnchor(GraphInstanceId graph_instance_id);

  /** @brief Prevents copying one exact Graph lifetime root. */
  GraphLifetimeAnchor(const GraphLifetimeAnchor&) = delete;
  /** @brief Prevents assigning one exact Graph lifetime root. */
  GraphLifetimeAnchor& operator=(const GraphLifetimeAnchor&) = delete;

  /**
   * @brief Returns the exact Graph identity retained by the anchor.
   * @return Stable nonzero identity.
   * @throws Nothing.
   */
  GraphInstanceId graph_instance_id() const noexcept {
    return graph_instance_id_;
  }

  /**
   * @brief Returns the preallocated monotonic close coordinator.
   * @return Shared record retained through lane/runtime retirement.
   * @throws Nothing.
   */
  std::shared_ptr<GraphCloseCoordinator> close_coordinator() const noexcept {
    return close_coordinator_;
  }

  /**
   * @brief Marks Graph lanes/state irreversibly retired.
   * @return Nothing.
   * @throws Nothing.
   * @note Repeated calls are harmless; this is observation, not destruction.
   */
  void mark_retired() noexcept {
    retired_.store(true, std::memory_order_release);
  }

  /**
   * @brief Reports whether Graph runtime ownership has retired.
   * @return True after mark_retired().
   * @throws Nothing.
   */
  bool retired() const noexcept {
    return retired_.load(std::memory_order_acquire);
  }

 private:
  /** @brief Exact non-reused Graph identity. */
  const GraphInstanceId graph_instance_id_;
  /** @brief One close record allocated before registry publication. */
  const std::shared_ptr<GraphCloseCoordinator> close_coordinator_;
  /** @brief Monotonic runtime/lane retirement observation. */
  std::atomic<bool> retired_{false};
};

/**
 * @brief Minimal strong lease protecting one Graph lifetime anchor.
 *
 * @throws Nothing for move, destruction, and scalar access.
 * @note The lease contains no GraphModel or lane pointer and grants no commit
 * or close authority.
 */
class GraphLifetimeLease final {
 public:
  /** @brief Creates an inactive lease used by moved-from values. */
  GraphLifetimeLease() noexcept = default;
  /** @brief Transfers one anchor lease. */
  GraphLifetimeLease(GraphLifetimeLease&&) noexcept = default;
  /** @brief Replaces this lease by transfer. */
  GraphLifetimeLease& operator=(GraphLifetimeLease&&) noexcept = default;
  /** @brief Releases the retained anchor passively. */
  ~GraphLifetimeLease() noexcept = default;

  /** @brief Prevents duplicating candidate lifetime ownership. */
  GraphLifetimeLease(const GraphLifetimeLease&) = delete;
  /** @brief Prevents assigning duplicate candidate ownership. */
  GraphLifetimeLease& operator=(const GraphLifetimeLease&) = delete;

  /**
   * @brief Returns the exact retained Graph identity.
   * @return Nonzero identity for an active lease.
   * @throws std::logic_error for an inactive moved-from lease.
   */
  GraphInstanceId graph_instance_id() const;

  /**
   * @brief Reports whether this value retains an anchor.
   * @return True for an active lease.
   * @throws Nothing.
   */
  bool active() const noexcept { return anchor_ != nullptr; }

 private:
  friend class RunLifecycleRegistry;

  /**
   * @brief Retains one registry-validated anchor.
   * @param anchor Exact Graph lifetime root.
   * @throws Nothing.
   */
  explicit GraphLifetimeLease(
      std::shared_ptr<GraphLifetimeAnchor> anchor) noexcept
      : anchor_(std::move(anchor)) {}

  /** @brief Strong anchor owner retained through candidate resolution. */
  std::shared_ptr<GraphLifetimeAnchor> anchor_;
};

/**
 * @brief Move-only pre-publication Graph admission candidate.
 *
 * Construction is the first lifecycle check and retains a Graph lifetime
 * lease. Destruction rolls back an unresolved candidate exactly once. Bundle
 * installation consumes the value and is the sole successful admission
 * linearization point.
 *
 * @throws std::system_error from registry synchronization.
 * @note Planning, policy calls, resource reservation, Run construction, and
 * staging occur while this value is live but outside the lifecycle fence.
 */
class RunLifecycleAdmissionCandidate final {
 public:
  /** @brief Creates an inactive moved-from candidate. */
  RunLifecycleAdmissionCandidate() noexcept = default;

  /**
   * @brief Transfers exactly-one candidate resolution ownership.
   * @param other Candidate made inactive.
   * @throws Nothing.
   */
  RunLifecycleAdmissionCandidate(
      RunLifecycleAdmissionCandidate&& other) noexcept;

  /**
   * @brief Rolls back the prior candidate, then transfers another.
   * @param other Candidate made inactive.
   * @return Reference to this value.
   * @throws Nothing; rollback synchronization failure terminates.
   */
  RunLifecycleAdmissionCandidate& operator=(
      RunLifecycleAdmissionCandidate&& other) noexcept;

  /**
   * @brief Rolls back an unresolved candidate and releases its Graph lease.
   * @throws Nothing; synchronization failure terminates.
   */
  ~RunLifecycleAdmissionCandidate() noexcept;

  /** @brief Prevents duplicating one candidate outcome. */
  RunLifecycleAdmissionCandidate(const RunLifecycleAdmissionCandidate&) =
      delete;
  /** @brief Prevents assigning duplicate candidate outcome ownership. */
  RunLifecycleAdmissionCandidate& operator=(
      const RunLifecycleAdmissionCandidate&) = delete;

  /**
   * @brief Returns the nonzero process-nonreused candidate identity.
   * @return Candidate scalar.
   * @throws std::logic_error for an inactive candidate.
   */
  std::uint64_t id() const;

  /**
   * @brief Returns the candidate's exact Graph identity.
   * @return Stable GraphInstanceId.
   * @throws std::logic_error for an inactive candidate.
   */
  GraphInstanceId graph_instance_id() const;

  /**
   * @brief Observes close/shutdown cancellation selected under the fence.
   * @return GraphClose, ProcessShutdown, or nullopt while still admissible.
   * @throws std::logic_error for an inactive candidate.
   * @note Observation never invokes a Run callback or takes the registry lock.
   */
  std::optional<ComputeRunCancellationReason> cancellation_reason() const;

  /**
   * @brief Reports whether this value still owns candidate resolution.
   * @return True until movement, commit, or rollback.
   * @throws Nothing.
   */
  bool active() const noexcept { return control_ != nullptr; }

 private:
  friend class RunLifecycleRegistry;

  /**
   * @brief Owns one registry-created candidate control.
   * @param control Shared control indexed by the Graph row.
   * @throws Nothing.
   */
  explicit RunLifecycleAdmissionCandidate(
      std::shared_ptr<RunLifecycleAdmissionCandidateControl> control) noexcept
      : control_(std::move(control)) {}

  /**
   * @brief Rolls back and clears this value when still active.
   * @return Nothing.
   * @throws Nothing; registry invariant failure terminates.
   */
  void reset() noexcept;

  /** @brief Candidate identity, lease, cancellation, and registry backlink. */
  std::shared_ptr<RunLifecycleAdmissionCandidateControl> control_;
};

/**
 * @brief Move-only installed standalone or realtime admission identity.
 *
 * @throws Nothing for movement and destruction; destroying an unresolved
 * finalization obligation terminates the process.
 * @note The value owns no Run lease itself. The registry record owns the
 * minimum leases until finalize_admission() retires every non-registry lease
 * and removes the complete bundle.
 */
class RunLifecycleAdmissionHandle final {
 public:
  /** @brief Creates an inactive handle for moved-from state. */
  RunLifecycleAdmissionHandle() noexcept = default;
  /**
   * @brief Transfers the sole finalization obligation.
   * @param other Active or inactive handle made inactive.
   * @throws Nothing.
   */
  RunLifecycleAdmissionHandle(RunLifecycleAdmissionHandle&& other) noexcept;
  /**
   * @brief Prevents overwriting an unresolved finalization obligation.
   * @param other Handle that cannot replace this authority.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  RunLifecycleAdmissionHandle& operator=(
      RunLifecycleAdmissionHandle&& other) noexcept = delete;
  /**
   * @brief Releases an inactive or already-finalized authority.
   * @throws Nothing; an unresolved installed bundle terminates the process.
   * @note Silent loss of the sole finalization obligation is forbidden.
   */
  ~RunLifecycleAdmissionHandle() noexcept;

  /** @brief Prevents duplicating one bundle finalization obligation. */
  RunLifecycleAdmissionHandle(const RunLifecycleAdmissionHandle&) = delete;
  /** @brief Prevents assigning duplicate finalization ownership. */
  RunLifecycleAdmissionHandle& operator=(const RunLifecycleAdmissionHandle&) =
      delete;

  /**
   * @brief Returns the nonzero installed bundle identity.
   * @return Registry-private bundle scalar.
   * @throws std::logic_error for an inactive handle.
   */
  std::uint64_t bundle_id() const;

  /**
   * @brief Reports whether the handle still names an installed bundle.
   * @return True while the persistent authority remains unresolved.
   * @throws Nothing.
   * @note A successful finalization leaves the shared control present but
   * marks it Finalized so concurrent/repeated observers are idempotent.
   */
  bool active() const noexcept;

 private:
  friend class RunLifecycleRegistry;

  /**
   * @brief Binds one installed bundle to its process registry.
   * @param control Preallocated persistent installed-bundle authority.
   * @throws Nothing.
   */
  explicit RunLifecycleAdmissionHandle(
      std::shared_ptr<RunLifecycleAdmissionHandleControl> control) noexcept
      : control_(std::move(control)) {}

  /** @brief Persistent synchronized finalization obligation. */
  std::shared_ptr<RunLifecycleAdmissionHandleControl> control_;
};

/**
 * @brief Single process execution-domain Graph/Run lifecycle authority.
 *
 * The registry owns monotonic service and Graph rows, pending candidates, and
 * installed bundle identity plus minimum Run leases. It never owns a Graph,
 * plan, staged output, dispatcher, resource token, callback, route, worker, or
 * plugin. Its fence protects only state/index/condition predicates and is
 * never held while waiting or invoking cancellation/plugin/ledger/lane work.
 *
 * @throws std::bad_alloc from pre-publication staging.
 * @throws std::overflow_error when a stable identity/generation is exhausted.
 * @throws std::system_error when registry synchronization fails.
 * @note ExecutionService constructs this owner before publication and destroys
 * it only after explicit Stopped transition.
 */
class RunLifecycleRegistry final {
 public:
  /**
   * @brief Constructs an empty Accepting registry over one telemetry owner.
   * @param telemetry Stable service-owned telemetry that outlives the registry.
   * @throws std::bad_alloc when private state cannot allocate.
   */
  explicit RunLifecycleRegistry(ExecutionLifecycleTelemetry& telemetry);

  /**
   * @brief Verifies passive destruction after Stopped and empty settlement.
   * @throws Nothing; violated ownership invariants terminate.
   */
  ~RunLifecycleRegistry() noexcept;

  /** @brief Prevents duplicating one lifecycle authority. */
  RunLifecycleRegistry(const RunLifecycleRegistry&) = delete;
  /** @brief Prevents assigning one lifecycle authority. */
  RunLifecycleRegistry& operator=(const RunLifecycleRegistry&) = delete;

  /**
   * @brief Publishes one fully constructed Graph lifetime row.
   * @param anchor Complete runtime anchor with matching exact identity.
   * @return Nothing after GraphRegistered publication.
   * @throws std::logic_error unless service is Accepting.
   * @throws std::invalid_argument for null/duplicate/retired anchors.
   * @throws std::bad_alloc while staging the row before the fence.
   * @note Runtime identity, anchor, lanes, and close record must all exist
   * before this call.
   */
  void register_graph(std::shared_ptr<GraphLifetimeAnchor> anchor);

  /**
   * @brief Rolls back an Open empty row after later load publication failure.
   * @param graph_instance_id Exact row identity.
   * @return Nothing after row removal.
   * @throws std::logic_error for a nonempty, Closing, or absent row.
   * @throws std::system_error when registry locking fails.
   * @note This is valid only before Graph/Host publication, never close
   * recovery.
   */
  void rollback_graph_registration(GraphInstanceId graph_instance_id);

  /**
   * @brief Installs one pending candidate and acquires a Graph lease.
   * @param graph_instance_id Exact live Graph identity.
   * @return Move-only candidate resolved by commit or destructor rollback.
   * @throws GraphError with NotFound unless service/row are accepting/open.
   * @throws std::bad_alloc while preallocating candidate/control/list storage.
   * @throws std::overflow_error when candidate identity is exhausted.
   * @note The lifecycle fence is released before this method returns.
   */
  RunLifecycleAdmissionCandidate begin_graph_admission(
      GraphInstanceId graph_instance_id);

  /**
   * @brief Atomically installs one standalone Run and consumes its candidate.
   * @param candidate Matching unresolved Graph candidate.
   * @param run_lease Minimum lease for the complete registry lifetime.
   * @param cancellation Request-level source used only outside the fence.
   * @return Installed bundle handle consumed by finalize_admission().
   * @throws GraphError with NotFound when close/shutdown won installation.
   * @throws std::invalid_argument for identity mismatch or null source.
   * @throws std::logic_error for stale/foreign candidate or duplicate Run id.
   * @throws std::bad_alloc only while staging before lifecycle mutation.
   * @note Registry mutation is nonallocating and is the admission
   * linearization point.
   */
  RunLifecycleAdmissionHandle commit_standalone(
      RunLifecycleAdmissionCandidate candidate, ComputeRunLease run_lease,
      std::shared_ptr<ComputeRequestCancellationSource> cancellation);

  /**
   * @brief Atomically installs one realtime group and both child Runs.
   * @param candidate Matching unresolved Graph candidate.
   * @param run_group_id Exact nonzero group identity.
   * @param hp_lease Minimum HP child registry lease.
   * @param rt_lease Minimum RT child registry lease.
   * @param cancellation Shared group cancellation fan-out source.
   * @param sibling_commit_gate Pending/committed/denied sibling gate.
   * @return One complete installed group handle.
   * @throws The standalone commit errors plus duplicate child/group identity.
   * @note No observer holding the lifecycle fence can see only one child.
   */
  RunLifecycleAdmissionHandle commit_realtime_group(
      RunLifecycleAdmissionCandidate candidate, RunGroupId run_group_id,
      ComputeRunLease hp_lease, ComputeRunLease rt_lease,
      std::shared_ptr<ComputeRequestCancellationSource> cancellation,
      std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate);

  /**
   * @brief Completes terminal/quiescent settlement and unregisters one bundle.
   * @param handle Exact persistent bundle authority, finalized on success.
   * @return Nothing only after every registry Run lease is the sole lease and
   * every child record has left the registry.
   * @throws std::invalid_argument for an inactive/foreign handle; a previously
   * finalized handle is an idempotent no-op.
   * @throws std::logic_error before every child is terminal.
   * @throws std::system_error when registry or Run waiting fails.
   * @note This method never waits while holding the lifecycle fence. A
   * terminal-not-ready or synchronization failure restores the shared
   * authority to Active for retry. Concurrent callers serialize through
   * Active/Finalizing/Finalized state and observe one successful finalization;
   * no by-value duplicate authority is created. Callers must complete
   * commit/discard and root resource release first.
   */
  void finalize_admission(RunLifecycleAdmissionHandle& handle);

  /**
   * @brief Validates one visible commit under graph-state then lifecycle order.
   * @param graph_instance_id Exact staged/live Graph identity.
   * @param run_id Exact registered child Run identity.
   * @return True only while the row is Open and Run remains Installed or
   * Finalizing.
   * @throws std::system_error when registry locking fails.
   * @note Callers already execute on the graph-state lane; this method waits
   * for nothing and invokes no callback.
   */
  bool permits_visible_commit(GraphInstanceId graph_instance_id,
                              ComputeRunId run_id) const;

  /**
   * @brief Linearizes one Graph row to Closing and dispatches cancellation.
   * @param graph_instance_id Exact row identity.
   * @param reason GraphClose or ProcessShutdown.
   * @return Stable nonzero close generation.
   * @throws Validation, lookup, or lifecycle-fence synchronization errors only
   * before `Open -> Closing` linearizes.
   * @note The method returns after cancellation fan-out but before waiting for
   * candidate/bundle settlement, allowing Kernel to stop request admission in
   * the exact row-before-lane order. After linearization, cleanup callback
   * failures are contained and cancellation synchronization/invariant failures
   * fail stop rather than unwind an irreversible transition.
   */
  std::uint64_t begin_graph_close(GraphInstanceId graph_instance_id,
                                  ComputeRunCancellationReason reason);

  /**
   * @brief Waits for one Closing row to empty and removes it.
   * @param graph_instance_id Exact row identity.
   * @return Nothing after GraphRowRemoved publication.
   * @throws GraphError with NotFound for an absent row.
   * @throws std::logic_error unless the row is Closing.
   * @throws std::system_error when waiting fails.
   * @note The wait never holds the lifecycle fence.
   */
  void finish_graph_close(GraphInstanceId graph_instance_id);

  /**
   * @brief Monotonically closes, cancels, settles, and removes one Graph row.
   * @param graph_instance_id Exact row identity.
   * @param reason GraphClose or ProcessShutdown.
   * @return Nothing after pending candidates and admitted bundles are gone and
   * GraphRowRemoved was published.
   * @throws GraphError with NotFound for an absent row.
   * @throws std::invalid_argument for any other reason.
   * @throws std::system_error from registry lookup or settlement waits.
   * @note Cancellation fan-out and settlement waits occur outside the fence.
   * Row removal precedes compute-request and graph-state lane drain. Once
   * Closing linearizes, cancellation synchronization/invariant failures fail
   * stop rather than escape as a recoverable close error.
   */
  void close_graph(GraphInstanceId graph_instance_id,
                   ComputeRunCancellationReason reason);

  /**
   * @brief Starts the one process shutdown generation and cancels every row.
   * @return Stable nonzero generation; repeated calls return the same value.
   * @throws std::overflow_error when shutdown generation is exhausted.
   * @throws std::system_error from lifecycle-fence synchronization before the
   * Stopping transition.
   * @note This method does not stop workers/routes or wait for Graph lanes.
   * After Stopping linearizes, cancellation synchronization/invariant failures
   * fail stop and cleanup callback failures are contained.
   */
  std::uint64_t begin_service_shutdown();

  /**
   * @brief Waits until every Graph row/candidate/admission is removed.
   * @return Nothing after the registry is empty.
   * @throws std::logic_error before service Stopping.
   * @throws std::system_error when waiting fails.
   */
  void wait_until_empty();

  /**
   * @brief Publishes final Stopped state after physical ownership retires.
   * @param final_counters Complete final counters, which must all be zero.
   * @return Final ServiceStopped sequence.
   * @throws std::logic_error unless Stopping and empty.
   * @throws std::invalid_argument unless every supplied counter is zero.
   * @throws std::system_error when registry/telemetry locking fails.
   */
  std::uint64_t mark_service_stopped(
      const ExecutionLifecycleCounters& final_counters);

  /**
   * @brief Copies current registry-derived lifecycle counters.
   * @return Exact Graph/candidate/bundle/finalization counters; physical
   * execution/plugin counters remain zero for caller augmentation.
   * @throws std::system_error when registry locking fails.
   */
  ExecutionLifecycleCounters counters() const;

  /**
   * @brief Reports whether service lifecycle admission remains Accepting.
   * @return True only before begin_service_shutdown().
   * @throws std::system_error when registry locking fails.
   */
  bool accepting() const;

  /**
   * @brief Returns current shutdown generation, or zero before shutdown.
   * @return Stable generation scalar.
   * @throws std::system_error when registry locking fails.
   */
  std::uint64_t shutdown_generation() const;

 private:
  friend class ::ps::testing::RunLifecycleRegistryTestAccess;
  friend class RunLifecycleAdmissionCandidate;

  /**
   * @brief Repository-test observer for one finalization wait boundary.
   *
   * @param context Opaque test context.
   * @param bundle_id Exact installed bundle.
   * @param resource_phase False before lease quiescence, true before root
   * resource settlement.
   * @return Nothing.
   * @throws Test-injected synchronization exception unchanged.
   * @note Production leaves this pointer null. The private support bridge uses
   * it to prove failed-owner retry without changing installed APIs.
   */
  using FinalizationWaitObserver = void (*)(void* context,
                                            std::uint64_t bundle_id,
                                            bool resource_phase);

  class Impl;

  /**
   * @brief Rolls back one unresolved candidate from its destructor.
   * @param control Exact indexed candidate control.
   * @return Nothing.
   * @throws Nothing; invariant/synchronization failure terminates.
   */
  void rollback_candidate(
      const std::shared_ptr<RunLifecycleAdmissionCandidateControl>&
          control) noexcept;

  /** @brief Optional repository-test finalization wait observer. */
  FinalizationWaitObserver finalization_wait_observer_ = nullptr;
  /** @brief Opaque context paired with finalization_wait_observer_. */
  void* finalization_wait_observer_context_ = nullptr;

  /** @brief Heap-owned stable implementation and lifecycle fence. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::compute
