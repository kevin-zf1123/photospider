/**
 * @file single_tenant_job_service.hpp
 * @brief Declares Issue #99 durable Job truth, quota, and retry authority.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>

#include "server/durable_server_state.hpp"  // NOLINT(build/include_subdir)
#include "server/job_contract.hpp"          // NOLINT(build/include_subdir)
#include "server/tenant_quota.hpp"          // NOLINT(build/include_subdir)

namespace ps::server {

class SingleTenantJobServiceTestAccess;
class WorkerManager;
struct WorkerManagerCompletion;

/**
 * @brief Reports whether a Job state is terminal.
 * @param state Candidate lifecycle state.
 * @return True for Succeeded, Failed, or Cancelled.
 * @throws std::invalid_argument for an invalid enum representation.
 */
bool is_terminal_job_state(JobState state);

/**
 * @brief Complete immutable assignment delegated to one Issue #100 worker.
 * @throws Nothing for default construction; copies may allocate.
 * @note `spec` is shared read-only and its digest must equal
 * `identity.job_spec_digest` before any graph resolution.
 */
struct JobAssignment final {
  /** @brief Full retained current assignment identity. */
  AttemptIdentity identity;
  /** @brief Immutable accepted JobSpec shared with this worker only. */
  std::shared_ptr<const JobSpec> spec;
  /**
   * @brief Optional read-only validated checkpoint record.
   * @note The worker receives immutable bytes/receipt only, never a store root,
   * mutable path, quota reservation, or publication capability.
   */
  std::shared_ptr<const ArtifactRecord> checkpoint;
};

/**
 * @brief Trusted graph material prepared for one external worker assignment.
 * @throws Nothing for default construction; string values may allocate.
 * @note Paths are trusted source-private adapter configuration. They are
 * transported to one exact worker but grant no Job, quota, artifact, network,
 * or durable-state authority.
 */
struct ResolvedGraphArtifact final {
  /** @brief True only when trusted resolution found immutable graph material.
   */
  bool ok = false;
  /** @brief Trusted graph-session root used by the Embedded Host adapter. */
  std::string root_dir;
  /** @brief Trusted explicit YAML path for this immutable graph artifact. */
  std::string yaml_path;
  /** @brief Optional trusted Host configuration path. */
  std::string config_path;
  /** @brief Optional trusted cache root private to this attempt. */
  std::string cache_root_dir;
  /** @brief Resolver-owned diagnostic when `ok` is false. */
  std::string message;
};

/**
 * @brief Immutable attempt facts returned by one worker execution.
 * @throws Nothing for default construction; values may allocate on mutation.
 * @note The semantic shape is closed. `Succeeded` requires `settled=true`,
 * `failure=None`, and one image. `Cancelled` requires `settled=true`,
 * `failure=CancellationObserved`, and no image. `Failed` requires a
 * worker-owned non-None failure and no image; `settled` records actual cleanup.
 * A successful image remains only a candidate, never artifact authority.
 */
struct JobAttemptReport final {
  /** @brief Full assignment identity echoed by the reporting worker. */
  AttemptIdentity identity;
  /** @brief Worker-local terminal outcome fact. */
  JobAttemptOutcome outcome = JobAttemptOutcome::Failed;
  /** @brief Whether the worker proved graph/Host settlement before reporting.
   */
  bool settled = false;
  /** @brief Typed failure category, or None only for successful compute. */
  JobAttemptFailure failure = JobAttemptFailure::Unexpected;
  /** @brief Human-readable worker diagnostic. */
  std::string message;
  /** @brief Candidate image present only for a successful output fact. */
  std::optional<ImageBuffer> image;
};

/**
 * @brief One attempt worker used inside a fresh worker process or test seam.
 * @throws Implementations document execution failures through their report;
 * allocation and system failures may still propagate to the control plane.
 * @note The object receives exactly one assignment and is then destroyed.
 * Product construction invokes it only inside `photospider-worker`; direct
 * control-plane invocation exists solely behind the non-installed deterministic
 * test marker. Implementations receive no owning service handle.
 */
class JobAttemptWorker {
 public:
  /**
   * @brief Destroys worker-private resources after its attempt.
   * @throws Nothing.
   * @note Its owning worker process or explicit test seam destroys the object
   * only after `execute()` returns or unwinds.
   */
  virtual ~JobAttemptWorker() = default;

  /**
   * @brief Executes one immutable assignment and returns settlement evidence.
   * @param assignment Exact current assignment.
   * @param cancellation_requested Read-only monotonic control-plane observer.
   * @return Immutable worker facts and optional candidate image.
   * @throws std::bad_alloc or std::system_error when unavoidable process-local
   * setup or synchronization failure cannot be represented safely.
   * @note The worker cannot commit an artifact or publish Job state. An
   * exception that escapes this boundary carries no settlement proof; the
   * control plane records an unsettled failed fact.
   */
  virtual JobAttemptReport execute(
      const JobAssignment& assignment,
      const std::function<bool()>& cancellation_requested) = 0;
};

/**
 * @brief Factory that creates one fresh worker object per assignment.
 * @throws Implementations may propagate allocation or setup failures.
 */
class JobAttemptWorkerFactory {
 public:
  /**
   * @brief Destroys factory-owned configuration after all workers.
   * @throws Nothing.
   * @note The service retains the factory through worker drainage.
   */
  virtual ~JobAttemptWorkerFactory() = default;

  /**
   * @brief Creates one worker that will receive exactly this assignment.
   * @param assignment Immutable current assignment for configuration only.
   * @return Non-null fresh worker owner.
   * @throws std::bad_alloc when worker allocation exhausts memory.
   * @note Returning null is a contract failure handled as a failed Job.
   * Neither a null return nor a propagated exception can establish attempt
   * settlement.
   */
  virtual std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) = 0;

  /**
   * @brief Reports whether this factory can prepare a product process payload.
   * @return True only when `prepare_external_graph()` is implemented.
   * @throws Nothing.
   * @note Returning false is rejected by ordinary product service
   * construction. The non-installed test marker is the only in-process
   * exception.
   */
  virtual bool supports_external_assignment() const noexcept { return false; }

  /**
   * @brief Resolves trusted graph material for one external worker process.
   * @param assignment Exact immutable current assignment.
   * @return Bounded trusted graph material for transport to that worker.
   * @throws std::logic_error when external preparation is unsupported.
   * @throws Resolver-specific allocation or trusted-I/O failures unchanged.
   * @note The default implementation fails closed and creates no worker.
   */
  virtual ResolvedGraphArtifact prepare_external_graph(
      const JobAssignment& assignment) const;
};

/**
 * @brief Bounded source-private WorkerManager process-lifecycle configuration.
 * @throws Nothing for default construction; path copies may allocate.
 * @note Ordinary product construction requires an executable path. Durations
 * govern local process supervision only and mint no Job or quota authority.
 */
struct WorkerManagerOptions final {
  /** @brief Exact non-installed `photospider-worker` executable path. */
  std::filesystem::path worker_executable;
  /** @brief Maximum assignment/acceptance handshake duration. */
  std::chrono::milliseconds startup_timeout{5000};
  /** @brief Heartbeat cadence requested from the worker. */
  std::chrono::milliseconds heartbeat_interval{250};
  /** @brief Maximum silence after assignment acceptance. */
  std::chrono::milliseconds heartbeat_timeout{3000};
  /** @brief Maximum total runtime before manager-owned termination. */
  std::chrono::milliseconds attempt_runtime_timeout{std::chrono::minutes(30)};
  /** @brief Maximum wait for clean exit after one report. */
  std::chrono::milliseconds post_report_timeout{2000};
  /** @brief Cooperative cancellation grace before `SIGTERM`. */
  std::chrono::milliseconds cooperative_cancel_timeout{1000};
  /** @brief `SIGTERM` grace before `SIGKILL`. */
  std::chrono::milliseconds terminate_timeout{1000};
  /** @brief Maximum `SIGKILL` reap observation interval. */
  std::chrono::milliseconds kill_reap_timeout{2000};
  /** @brief Per-frame read/write deadline bound. */
  std::chrono::milliseconds io_timeout{2000};
};

/**
 * @brief Immutable receipt returned immediately after Job acceptance.
 * @throws Nothing for default construction; copied ids may allocate.
 * @note Move construction must remain non-throwing so `submit()` cannot report
 * failure after the Job and its worker have already been accepted.
 */
struct JobSubmission final {
  /** @brief Newly accepted Job identity. */
  JobId job_id;
  /** @brief SHA-256 of accepted canonical JobSpec bytes. */
  JobSpecDigest job_spec_digest;
  /** @brief Exact newly accepted current assignment tuple. */
  AttemptIdentity assignment;
};

/** @brief Guards the post-acceptance submission return as a no-throw move. */
static_assert(std::is_nothrow_move_constructible_v<JobSubmission>,
              "JobSubmission must move without throwing after acceptance");

/**
 * @brief Copied control-plane truth returned by query or wait.
 * @throws Nothing for default construction; copies may allocate.
 * @note The immutable JobSpec is shared as `const` and cannot be replaced by a
 * worker. A terminal snapshot never changes state afterward.
 */
struct JobSnapshot final {
  /** @brief Configured single tenant that owns the Job. */
  TenantId tenant_id;
  /** @brief Accepted Job identity. */
  JobId job_id;
  /** @brief Exact accepted immutable JobSpec. */
  std::shared_ptr<const JobSpec> spec;
  /** @brief Exact current assignment tuple. */
  AttemptIdentity assignment;
  /** @brief Stable artifact identity reserved at initial Job acceptance. */
  ArtifactId output_artifact_id;
  /** @brief Stable idempotent commit identity preserved across retry. */
  OutputCommitId output_commit_id;
  /** @brief Authoritative control-plane lifecycle state. */
  JobState state = JobState::Queued;
  /** @brief Whether cancellation intent was accepted monotonically. */
  bool cancellation_requested = false;
  /** @brief Whether worker settlement or exact process reaping completed. */
  bool attempt_settled = false;
  /** @brief Accepted worker-local outcome, never rewritten by control-plane
   * cancellation adjudication. */
  std::optional<JobAttemptOutcome> attempt_outcome;
  /** @brief Typed accepted attempt or control-plane terminal failure, if any.
   * A valid worker failure remains authoritative after accepted cancellation.
   */
  JobAttemptFailure failure = JobAttemptFailure::None;
  /** @brief Human-readable current diagnostic. */
  std::string message;
  /** @brief Sole required output receipt, present only after successful commit.
   */
  std::optional<OutputCommitReceipt> output_receipt;
};

/**
 * @brief Source-private single-tenant Issue #99/#100 Job control-plane owner.
 *
 * The service validates and freezes JobSpec values, authorizes optional
 * checkpoints, atomically reserves a complete tenant quota envelope, persists
 * accepted/current/terminal truth, delegates every product attempt to one
 * WorkerManager-owned freshly execed process, validates the complete returned
 * tuple or trusted post-reap failure, linearizes cancellation versus crash-
 * durable artifact commit, settles quota, and alone publishes Job/retry truth.
 * A Job-journal failure
 * after atomic record publication aligns in-memory truth with that record,
 * retains active quota, stops every later durable mutation, and requires a
 * service restart for conservative recovery. Artifact deletion likewise
 * distinguishes not-removed, visibility-unconfirmed, cleanup-pending, and
 * fully-cleaned truth: confirmed visibility removal releases exact retained
 * quota even when cleanup later fails, while every irreversible deletion
 * failure revokes lookup and fail-stops mutation until restart.
 * Once an exact matching artifact is observed, or manifest-visible lookup is
 * ambiguous, any later revalidation, quota conversion, or Succeeded-journal
 * failure enters a separate monotonic reconciliation fail-stop. That state
 * preserves the current active reservation or already-retained charge and
 * forbids a compensating `Failed/ArtifactCommit` record until restart.
 * Active-attempt release failure is another monotonic fail-stop boundary.
 * A terminal Job keeps its reservation owner in the control record; a
 * pre-publication rollback with no durable Job transfers its owner into one
 * service-owned stranded slot. No compensating terminal rewrite or same-
 * process release retry is attempted, and restart reconstructs only durable
 * Job/artifact truth with no process-local active reservation.
 *
 * @throws std::invalid_argument when construction receives an invalid tenant,
 * null/non-externalizable product factory, invalid manager bounds, or unusable
 * worker executable.
 * @note Methods are thread-safe. Destruction records cancellation intent,
 * releases `mutex_`, then asks WorkerManager to concurrently drain, terminate,
 * and reap every exact child. Callers must not race object destruction with a
 * new public method call or trigger reentrant destruction from a worker.
 */
class SingleTenantJobService final {
 public:
  /**
   * @brief Opens and recovers one durable Job authority for one tenant.
   * @param tenant_id Exact sole tenant identity.
   * @param quota_limits Trusted complete tenant capacity.
   * @param state_root Trusted durable control/artifact root.
   * @param worker_factory Non-null fresh-worker factory.
   * @param state_options Optional source-private durable commit observer.
   * @param quota_options Optional source-private quota mutation observer.
   * @param worker_options Bounded process-supervision configuration.
   * @throws std::invalid_argument for invalid configuration.
   * @throws std::bad_alloc when service/manager state allocation fails.
   * @throws DurableStateError and derived durability, corruption, or commit
   * errors while recovering the durable root or publishing a repaired Job.
   * @throws std::system_error for filesystem or manager-thread creation
   * failures.
   * @note Product worker configuration is validated before the durable root is
   * opened or repaired. No manager record exists until recovery completes.
   */
  SingleTenantJobService(
      TenantId tenant_id, TenantQuotaLimits quota_limits,
      std::filesystem::path state_root,
      std::shared_ptr<JobAttemptWorkerFactory> worker_factory,
      DurableServerStateOptions state_options = {},
      TenantQuotaAuthorityOptions quota_options = {},
      WorkerManagerOptions worker_options = {});

  /**
   * @brief Requests cancellation and drains all process-local workers.
   * @throws Nothing; WorkerManager contains escalation and reaping failures.
   * @note No service mutex is held during any process or thread wait.
   */
  ~SingleTenantJobService() noexcept;

  /**
   * @brief Prevents duplicate control-plane authority ownership.
   * @param other Service authority that cannot be copied.
   */
  SingleTenantJobService(const SingleTenantJobService& other) = delete;

  /**
   * @brief Prevents duplicate control-plane authority assignment.
   * @param other Service authority that cannot be copied.
   * @return No assignment result because the operation is deleted.
   */
  SingleTenantJobService& operator=(const SingleTenantJobService& other) =
      delete;

  /**
   * @brief Validates, freezes, accepts, and starts one immutable Job.
   * @param spec Complete supported JobSpec copied into shared const storage.
   * @return Accepted Job/digest/current assignment receipt.
   * @throws std::invalid_argument for invalid JobSpec or service shutdown.
   * @throws std::bad_alloc when Job state or worker setup exhausts memory.
   * @throws std::system_error when manager supervision-thread creation fails.
   * @note Checkpoint validation and complete quota reservation precede durable
   * accepted-state publication. Supervision-handle construction occurs while
   * `mutex_` prevents assignment entry and before journal publication. A
   * pre-publication journal failure removes process-local acceptance, releases
   * quota, and lets WorkerManager retire the fenced record. If rollback release
   * itself fails, the service retains the reservation in its stranded-owner
   * slot, fail-stops all later mutation, and rethrows the triggering submit
   * failure. A post- publication failure throws `DurableJobCommitError`,
   * retains aligned truth/ quota, and fail-stops writes. The remaining
   * successful return path is a non-throwing move.
   */
  JobSubmission submit(JobSpec spec);

  /**
   * @brief Explicitly replaces one settled failed current attempt.
   * @param job_id Exact durable Job identity.
   * @return Fresh current assignment receipt when retry is accepted; null when
   * the Job is absent, not failed, unsettled, or service shutdown has begun.
   * @throws TenantQuotaExceeded when the unchanged envelope cannot be reserved.
   * @throws DurableStateError/system/allocation failures during durable
   * replacement or worker start. A pre-publication failure preserves prior
   * failed truth; a post-publication `DurableJobCommitError` preserves the new
   * attempt truth/quota and fail-stops writes until restart.
   * @note JobId, JobSpecDigest, checkpoint, stable artifact, and stable commit
   * identity are preserved. Attempt/worker/lease/quota identities are fresh.
   * If rollback release fails, the service retains the fresh reservation in
   * its stranded-owner slot, fail-stops mutation, and rethrows the triggering
   * retry failure without changing prior durable Job truth.
   */
  std::optional<JobSubmission> retry(const JobId& job_id);

  /**
   * @brief Returns current authoritative Job truth without blocking.
   * @param job_id Exact Job identity.
   * @return Copied snapshot, or nullopt when this service has no such Job.
   * @throws std::bad_alloc when copying the snapshot exhausts memory.
   */
  std::optional<JobSnapshot> query(const JobId& job_id) const;

  /**
   * @brief Waits for a Job terminal or a bounded observer timeout.
   * @param job_id Exact Job identity.
   * @param timeout Nonnegative observer wait bound.
   * @return Terminal copied snapshot, or nullopt for absent Job or timeout.
   * @throws std::invalid_argument for a negative timeout.
   * @throws std::system_error for condition-variable synchronization failure.
   * @throws std::bad_alloc when copying the terminal snapshot exhausts memory.
   */
  std::optional<JobSnapshot> wait_for(const JobId& job_id,
                                      std::chrono::milliseconds timeout) const;

  /**
   * @brief Records monotonic cancellation intent for one active Job.
   * @param job_id Exact Job identity.
   * @return True only when this call newly accepted cancellation intent.
   * @throws DurableStateError/system/allocation failures from journal
   * publication, including `DurableJobCommitError` after atomic publication.
   * @note Terminal Jobs, absent Jobs, and repeated requests return false.
   * Active Host compute is not promised bounded preemption. A post-publication
   * failure leaves cancellation accepted in memory and on disk, retains quota,
   * and fail-stops later mutations until restart.
   */
  bool cancel(const JobId& job_id);

  /**
   * @brief Looks up an immutable committed artifact by authoritative id.
   * @param artifact_id Exact ArtifactId from a receipt.
   * @return Shared immutable crash-durable record, or null when absent.
   * @throws As `DurableServerState::find_artifact`.
   */
  std::shared_ptr<const ArtifactRecord> find_artifact(
      const ArtifactId& artifact_id) const;

  /**
   * @brief Durably deletes one tenant artifact and releases retained quota.
   * @param artifact_id Exact durable artifact identity.
   * @return True when one committed artifact was removed; false when absent.
   * @throws The original pre-manifest failure while retaining visibility and
   * mutation availability.
   * @throws DurableArtifactEraseError after visibility was removed but its
   * durability/cleanup/acknowledgement did not complete cleanly; the service
   * has already coordinated quota according to the reported state and entered
   * durable-mutation fail-stop.
   * @note A receipt retained by an existing successful Job remains historical
   * truth but lookup/checkpoint admission no longer resolves the deleted bytes.
   * Visibility-confirmed deletion releases the quota authority's exact charge,
   * including an already-absent retry; unconfirmed visibility preserves it for
   * restart reconciliation.
   */
  bool delete_artifact(const ArtifactId& artifact_id);

  /**
   * @brief Captures exact current tenant quota accounting.
   * @return Active and durable-retention usage snapshot.
   * @throws As `TenantQuotaAuthority::snapshot`.
   */
  TenantQuotaSnapshot quota_snapshot() const;

 private:
  friend class SingleTenantJobServiceTestAccess;

  /**
   * @brief Private union of observable Job truth and active quota ownership.
   * @throws Nothing for default construction; copies may allocate.
   * @note `reservation` exists while the current attempt is active, or after a
   * terminal release failure until service destruction/restart. Workers and
   * query callers never receive this mutation authority.
   */
  struct JobControlRecord final {
    /** @brief Observable/persisted current control-plane truth. */
    JobSnapshot snapshot;
    /** @brief Exact active server quota reservation, absent after settlement.
     */
    std::optional<TenantQuotaReservation> reservation;
  };

  /** @brief Guards allocation-free retry publication and rollback. */
  static_assert(std::is_nothrow_move_constructible_v<JobSnapshot>);
  /** @brief Guards allocation-free published-snapshot cache alignment. */
  static_assert(std::is_nothrow_swappable_v<JobSnapshot>);
  /** @brief Guards allocation-free transfer of accepted quota ownership. */
  static_assert(std::is_nothrow_move_constructible_v<TenantQuotaReservation>);
  /** @brief Guards allocation-free transfer to the stranded-owner slot. */
  static_assert(
      std::is_nothrow_move_assignable_v<std::optional<TenantQuotaReservation>>);
  /** @brief Guards allocation-free atomic in-memory retry replacement. */
  static_assert(std::is_nothrow_swappable_v<JobControlRecord>);

  /**
   * @brief Fences and durably begins one manager-owned exact assignment.
   * @param expected Exact assignment retained by WorkerManager.
   * @return True when the assignment remains current and may execute.
   * @throws Durable-state failures from Running publication unchanged.
   * @note WorkerManager invokes this without its mutex. A false result spawns
   * no process and publishes no completion fact.
   */
  bool begin_managed_assignment(const AttemptIdentity& expected);

  /**
   * @brief Applies one report or trusted post-reap manager terminal fact.
   * @param completion Exact manager completion moved into service authority.
   * @return Nothing after current-attempt fencing and durable reconciliation.
   * @throws Nothing; durable/report exceptions are contained by existing
   * fail-stop rules.
   * @note WorkerManager has already closed capabilities and reaped any process.
   */
  void apply_worker_completion(WorkerManagerCompletion completion) noexcept;

  /**
   * @brief Reads monotonic cancellation state for a worker observer.
   * @param expected Exact assignment identity observed by its worker.
   * @return True when service shutdown, durable-mutation fail-stop,
   * replacement, or current cancellation is active.
   * @throws Nothing.
   */
  bool cancellation_requested_for(
      const AttemptIdentity& expected) const noexcept;

  /**
   * @brief Validates and applies one worker report under the Job mutex.
   * @param expected Exact assignment owned by the invoking manager callback.
   * @param report Immutable attempt facts and candidate image.
   * @return Nothing after terminal state derivation and observer notification.
   * @throws Nothing; an unambiguous pre-manifest artifact exception becomes a
   * typed failed Job. Manifest-visible ambiguity or any failure after an exact
   * artifact match preserves quota and enters reconciliation fail-stop. Any
   * terminal active-release exception preserves its exact reservation owner
   * and enters quota-release fail-stop without rewriting terminal truth.
   * @note A prior attempt whose `expected` identity is no longer current is
   * fenced without mutating the replacement attempt. For the current attempt,
   * full report identity, enum, outcome/failure/settlement/image shape, and
   * cancellation-context validation precede copying any report fact or
   * cancellation adjudication. An accepted `Failed` report takes precedence
   * over cancellation intent and retains its exact settlement, failure, and
   * diagnostic facts. A rejected current report leaves `attempt_outcome` unset
   * and cannot establish settlement or an artifact receipt, even when its
   * untrusted `settled` field is true.
   */
  void apply_report(const AttemptIdentity& expected,
                    JobAttemptReport report) noexcept;

  /**
   * @brief Reconciles an already published stable artifact after an exception.
   * @param control Current exact Job/quota record under `mutex_`.
   * @return True after quota conversion and durable Succeeded publication;
   * false only when lookup proves no manifest is visible.
   * @throws Durable-state, quota, persistence, or allocation failures
   * unchanged.
   * @note The caller holds `mutex_`. This path revalidates and reapplies the
   * artifact barrier chain before publishing success. Once lookup returns an
   * occurrence, every mismatch or later quota/journal exception first enters
   * reconciliation fail-stop. A lookup exception is treated as potentially
   * manifest-visible and follows the same conservative rule; only a null
   * lookup authorizes ordinary pre-manifest failure publication.
   */
  bool reconcile_published_artifact_locked(JobControlRecord& control);

  /**
   * @brief Converts one observable snapshot to complete durable record truth.
   * @param snapshot Fully joined current snapshot.
   * @return Complete durable record.
   * @throws std::bad_alloc when copying retained values fails.
   */
  DurableJobRecord durable_record(const JobSnapshot& snapshot) const;

  /**
   * @brief Publishes one replacement to journal and matching memory truth.
   * @param control Current process-local Job/quota record under `mutex_`.
   * @param candidate Fully prepared replacement snapshot.
   * @return Nothing after a clean confirmed commit and no-throw memory swap.
   * @throws The original pre-publication failure while leaving `control`
   * unchanged. Throws `DurableJobCommitError` after publication, after first
   * swapping `candidate` into `control` and entering service fail-stop.
   * @note The caller holds `mutex_`. Active quota is deliberately untouched;
   * callers settle it only after this method returns successfully.
   */
  void publish_snapshot_locked(JobControlRecord& control,
                               JobSnapshot candidate);

  /**
   * @brief Enters monotonic artifact-reconciliation fail-stop.
   * @return Nothing after workers and later durable mutation are fenced.
   * @throws Nothing.
   * @note The caller holds `mutex_`. This transition deliberately leaves the
   * current Job snapshot and quota ownership unchanged: an active reservation
   * remains active, while a completed conversion remains retained, so restart
   * can reconstruct the strongest durable artifact truth.
   */
  void fail_stop_artifact_reconciliation_locked() noexcept;

  /**
   * @brief Attempts one exact active-reservation release under service lock.
   * @param reservation Service-owned reservation optional to settle in place.
   * @return True when absent or released; false when release failed.
   * @throws Nothing; every quota exception is converted to monotonic release
   * fail-stop while the exact owner remains in `reservation`.
   * @note The caller holds `mutex_`. Success resets the optional exactly once.
   * Failure changes no Job snapshot and performs no compensating durable
   * rewrite or same-process retry. Workers and later mutation are fenced.
   */
  bool try_release_attempt_locked(
      std::optional<TenantQuotaReservation>& reservation) noexcept;

  /**
   * @brief Transfers a rollback-only reservation into service ownership.
   * @param reservation Present candidate owner whose Job will not remain in
   * `jobs_` and whose quota release has already failed.
   * @return Nothing after allocation-free unique-owner transfer.
   * @throws Nothing; impossible invariant violations terminate rather than
   * silently discard either quota owner.
   * @note The caller holds `mutex_`. The first release failure has already
   * fail-stopped all mutation, so at most one rollback-only stranded owner can
   * exist. Terminal Job owners remain in their `JobControlRecord` instead.
   */
  void retain_stranded_reservation_locked(
      std::optional<TenantQuotaReservation>& reservation) noexcept;

  /**
   * @brief Reports whether any irreversible durable mutation failure occurred.
   * @return True after Job-journal publication ambiguity, artifact-deletion
   * visibility ambiguity/cleanup failure, artifact reconciliation failure, or
   * active-attempt quota release failure.
   * @throws Nothing.
   * @note The caller holds `mutex_`. Reads remain available in this state;
   * workers and every subsequent durable mutation are fenced until restart.
   */
  bool durable_mutation_faulted_locked() const noexcept {
    return journal_faulted_ || artifact_erase_faulted_ ||
           artifact_reconciliation_faulted_ || quota_release_faulted_;
  }

  /**
   * @brief Rejects mutation after any durability or quota-release fail-stop.
   * @return Nothing when durable mutation remains available.
   * @throws DurableStateError when restart reconciliation or active-release
   * recovery is required.
   * @note The caller holds `mutex_`.
   */
  void require_durable_mutation_available_locked() const;

  /**
   * @brief Persists one recovery or current snapshot in serialized service
   * context.
   * @param snapshot Fully joined current truth.
   * @return Nothing after crash-durability barriers complete.
   * @throws The captured pre-publication failure or
   * `DurableJobCommitError` after an ambiguous/lost acknowledgement.
   * @note Called either during single-threaded construction before any manager
   * record or public access exists, or while the caller holds `mutex_`.
   * Synchronous metadata I/O is the current single-tenant linearization policy.
   */
  void persist_snapshot_locked(const JobSnapshot& snapshot);

  /** @brief Sole configured tenant authority. */
  TenantId tenant_id_;
  /** @brief Shared factory retained until every worker joins. */
  std::shared_ptr<JobAttemptWorkerFactory> worker_factory_;
  /** @brief Sole assignment/process/thread lifecycle owner. */
  std::unique_ptr<WorkerManager> worker_manager_;
  /** @brief Sole crash-durable Job/artifact state-root authority. */
  std::unique_ptr<DurableServerState> durable_state_;
  /** @brief Sole complete-envelope tenant quota authority. */
  std::unique_ptr<TenantQuotaAuthority> quota_authority_;
  /** @brief Collision-resistant service namespace used by durable identities.
   */
  std::string identity_namespace_;
  /** @brief Next checked Job identity sequence in this service namespace. */
  std::uint64_t next_job_sequence_ = 1U;
  /** @brief Serializes all durable Job and quota-authority mutations. */
  mutable std::mutex mutex_;
  /** @brief Wakes Job observers and source-private test waits. */
  mutable std::condition_variable condition_;
  /** @brief JobId text to authoritative copied current record. */
  std::map<std::string, JobControlRecord> jobs_;
  /**
   * @brief Sole active reservation whose rollback has no durable Job owner.
   * @note Present only after release failed during submit/retry supervision-
   * thread start or NotPublished rollback. The monotonic release fail-stop
   * prevents a second candidate. Destruction/restart drops this process-local
   * charge authority.
   */
  std::optional<TenantQuotaReservation> stranded_reservation_;
  /** @brief Monotonic service destruction/cancellation intent. */
  bool shutting_down_ = false;
  /**
   * @brief Monotonic fail-stop after an atomically published journal error.
   * @note While true, reads remain available, workers are fenced, active quota
   * remains reserved, and only destruction/restart may advance lifecycle.
   */
  bool journal_faulted_ = false;
  /**
   * @brief Monotonic fail-stop after artifact visibility became irreversible.
   * @note Confirmed visibility removal has already released exact retained
   * quota; unconfirmed visibility deliberately keeps it charged until restart.
   */
  bool artifact_erase_faulted_ = false;
  /**
   * @brief Monotonic fail-stop after manifest-visible reconciliation failed.
   * @note The current Job snapshot is not rewritten to Failed. Active
   * reservation or retained-charge truth stays at its strongest reached state
   * until constructor recovery revalidates and reconciles the artifact.
   */
  bool artifact_reconciliation_faulted_ = false;
  /**
   * @brief Monotonic fail-stop after active-attempt release raised.
   * @note The exact owner remains either on its terminal Job control or in
   * `stranded_reservation_`. No same-process mutation or compensating durable
   * rewrite proceeds; restart clears active quota and reloads durable truth.
   */
  bool quota_release_faulted_ = false;
};

}  // namespace ps::server
