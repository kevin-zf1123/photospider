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

/**
 * @brief Reports whether a Job state is terminal.
 * @param state Candidate lifecycle state.
 * @return True for Succeeded, Failed, or Cancelled.
 * @throws std::invalid_argument for an invalid enum representation.
 */
bool is_terminal_job_state(JobState state);

/**
 * @brief Complete immutable assignment delegated to one Issue #99 worker.
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
 * @brief One attempt worker owned by a single assignment thread.
 * @throws Implementations document execution failures through their report;
 * allocation and system failures may still propagate to the control plane.
 * @note The object receives exactly one assignment and is then destroyed. It
 * is not an OS-process or security-isolation claim. Implementations receive no
 * owning service handle and must return from `execute()` before any external
 * owner destroys that service; reentrant self-destruction is not a supported
 * worker action.
 */
class JobAttemptWorker {
 public:
  /**
   * @brief Destroys worker-private resources after its attempt.
   * @throws Nothing.
   * @note The control plane destroys the object only after `execute()`
   * returns or unwinds.
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
  /** @brief Whether the current worker reported completed settlement. */
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
 * @brief Source-private single-tenant Issue #99 Job control-plane owner.
 *
 * The service validates and freezes JobSpec values, authorizes optional
 * checkpoints, atomically reserves a complete tenant quota envelope, persists
 * accepted/current/terminal truth, starts one fresh in-process worker per
 * explicit attempt, validates the complete returned tuple, linearizes
 * cancellation versus crash-durable artifact commit, settles quota, and alone
 * publishes Job/retry truth. One private infrastructure reaper joins completed
 * assignment threads throughout the service lifetime. A Job-journal failure
 * after atomic record publication aligns in-memory truth with that record,
 * retains active quota, stops every later durable mutation, and requires a
 * service restart for conservative recovery. Artifact deletion likewise
 * distinguishes not-removed, visibility-unconfirmed, cleanup-pending, and
 * fully-cleaned truth: confirmed visibility removal releases exact retained
 * quota even when cleanup later fails, while every irreversible deletion
 * failure revokes lookup and fail-stops mutation until restart.
 *
 * @throws std::invalid_argument when construction receives an invalid tenant or
 * null factory.
 * @note Methods are thread-safe. Destruction requests cancellation for active
 * Jobs, wakes the reaper, and joins it only after it has joined every worker.
 * No worker joins itself and no join occurs while `mutex_` is held. Destruction
 * still cannot bound a provider that ignores current cooperative observations.
 * Callers must not race object destruction with a new public method call or
 * trigger reentrant destruction from a worker callback.
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
   * @throws std::invalid_argument for invalid configuration.
   * @throws std::bad_alloc when service/reaper state allocation fails.
   * @throws DurableStateError and derived durability, corruption, or commit
   * errors while recovering the durable root or publishing a repaired Job.
   * @throws std::system_error for filesystem or private reaper creation
   * failures.
   */
  SingleTenantJobService(
      TenantId tenant_id, TenantQuotaLimits quota_limits,
      std::filesystem::path state_root,
      std::shared_ptr<JobAttemptWorkerFactory> worker_factory,
      DurableServerStateOptions state_options = {});

  /**
   * @brief Requests cancellation and joins all process-local workers.
   * @throws Nothing; the reaper joins only valid worker handles owned by other
   * threads, and this destructor joins only its valid reaper handle.
   * @note No service mutex is held while either worker or reaper join waits.
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
   * @throws std::system_error when process-local worker-thread creation fails.
   * @note Checkpoint validation and complete quota reservation precede durable
   * accepted-state publication. Thread construction occurs while `mutex_`
   * prevents worker entry and before journal publication. A pre-publication
   * journal failure removes process-local acceptance, releases quota, and lets
   * the reaper join the fenced worker. A post-publication failure throws
   * `DurableJobCommitError`, retains aligned truth/quota, and fail-stops
   * writes. The remaining successful return path is a non-throwing move.
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
   * @brief Owns one joinable assignment thread until the reaper joins it.
   *
   * The map key supplies the exact attempt identity. An empty record is first
   * published while the service mutex is held, then receives the newly started
   * thread through no-throw move assignment. `completed` becomes true only
   * after the worker has published or rejected its terminal attempt report.
   * The dedicated reaper then moves `thread` out while holding the service
   * mutex and joins it only after releasing that mutex.
   *
   * @throws Nothing for empty construction and moves. Thread creation is a
   * separate operation documented by `start_assignment_thread`.
   * @note Empty records exist only while the service mutex excludes the reaper;
   * a start failure erases the record before that mutex is released. Records
   * are move-only and never expose their thread outside the service/reaper
   * boundary.
   */
  struct WorkerThreadRecord final {
    /**
     * @brief Creates one temporarily empty ownership record.
     * @throws Nothing.
     * @note The caller holds the service mutex until a joinable thread is moved
     * into `thread` or the record is erased.
     */
    WorkerThreadRecord() noexcept = default;

    /**
     * @brief Starts one assignment thread through the source-private seam.
     * @param service Non-null service that owns the new worker record.
     * @param assignment Complete assignment moved into the worker callback.
     * @return Sole joinable thread owner for publication in `workers_`.
     * @throws std::bad_alloc when thread callback storage allocation fails.
     * @throws std::system_error when the source-private test seam injects a
     * start failure or native thread creation fails.
     * @note The deterministic test seam is checked on the submitter thread
     * before any `std::thread` is constructed. It is not installed or exposed
     * as a product contract. Normal production calls leave the seam disarmed.
     */
    static std::thread start_assignment_thread(SingleTenantJobService* service,
                                               JobAssignment assignment);

    /**
     * @brief Transfers unique joinable-thread ownership without throwing.
     * @param other Record whose thread handle is transferred.
     * @throws Nothing.
     */
    WorkerThreadRecord(WorkerThreadRecord&& other) noexcept = default;

    /**
     * @brief Transfers unique joinable-thread assignment without throwing.
     * @param other Record whose thread handle is transferred.
     * @return This uniquely owning record.
     * @throws Nothing.
     */
    WorkerThreadRecord& operator=(WorkerThreadRecord&& other) noexcept =
        default;

    /**
     * @brief Prevents duplicate joinable-thread ownership.
     * @param other Record that cannot be copied.
     */
    WorkerThreadRecord(const WorkerThreadRecord& other) = delete;

    /**
     * @brief Prevents duplicate joinable-thread assignment.
     * @param other Record that cannot be copied.
     * @return No assignment result because the operation is deleted.
     */
    WorkerThreadRecord& operator=(const WorkerThreadRecord& other) = delete;

    /** @brief Joinable assignment thread until moved to the reaper. */
    std::thread thread;
    /** @brief Whether assignment/report processing reached its final tail. */
    bool completed = false;
  };

  /**
   * @brief Private union of observable Job truth and active quota ownership.
   * @throws Nothing for default construction; copies may allocate.
   * @note `reservation` exists exactly while the current attempt is active.
   * Workers and query callers never receive this mutation authority.
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
  /** @brief Guards allocation-free atomic in-memory retry replacement. */
  static_assert(std::is_nothrow_swappable_v<JobControlRecord>);

  /**
   * @brief Executes one worker and applies its report under control authority.
   * @param assignment Immutable accepted current assignment.
   * @return Nothing after terminal Job publication.
   * @throws Nothing; a null factory result and all worker/factory/report
   * exceptions become unsettled failed facts because this boundary has no
   * graph/Host settlement proof.
   * @note Every non-terminating path marks the exact worker record complete so
   * the reaper can join it; the worker never touches its own thread handle.
   */
  void run_assignment(JobAssignment assignment) noexcept;

  /**
   * @brief Marks one assignment thread complete and wakes the reaper.
   * @param attempt_id Exact map key retained by the completing worker.
   * @return Nothing.
   * @throws Nothing.
   * @note The worker never moves or joins its own thread handle. The record is
   * marked under `mutex_`, then the reaper performs the join outside that lock.
   */
  void mark_worker_completed(const JobAttemptId& attempt_id) noexcept;

  /**
   * @brief Continuously joins completed assignment threads outside the mutex.
   * @return Nothing after shutdown and complete worker drainage.
   * @throws Nothing; every moved handle is joinable and belongs to a different
   * thread, so its checked `join()` cannot violate standard preconditions.
   * @note This is the sole ongoing worker-handle reaper. It never owns Job
   * execution policy and remains one bounded service-infrastructure thread.
   */
  void reap_workers() noexcept;

  /**
   * @brief Reports whether a completed worker record exists under `mutex_`.
   * @return True when the reaper can move one completed handle immediately.
   * @throws Nothing.
   * @note The caller must hold `mutex_` for the full scan.
   */
  bool has_completed_worker_locked() const noexcept;

  /**
   * @brief Reads monotonic cancellation state for a worker observer.
   * @param expected Exact assignment identity observed by its worker.
   * @return True when service shutdown, replacement, or current cancellation
   * is active.
   * @throws Nothing.
   */
  bool cancellation_requested_for(
      const AttemptIdentity& expected) const noexcept;

  /**
   * @brief Validates and applies one worker report under the Job mutex.
   * @param expected Exact assignment owned by the invoking worker thread.
   * @param report Immutable attempt facts and candidate image.
   * @return Nothing after terminal state derivation and observer notification.
   * @throws Nothing; artifact exceptions become a typed failed Job.
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
   * false when no identity-matching manifest is visible.
   * @throws Durable-state, quota, persistence, or allocation failures
   * unchanged.
   * @note The caller holds `mutex_`. This path revalidates and reapplies the
   * artifact barrier chain before publishing success. It is used only after a
   * primary terminal transition raised, so a manifest-last occurrence is not
   * misclassified or left without its retained quota charge.
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
   * @brief Reports whether any irreversible durable mutation failure occurred.
   * @return True after Job-journal publication ambiguity or artifact-deletion
   * visibility ambiguity/cleanup failure.
   * @throws Nothing.
   * @note The caller holds `mutex_`. Reads remain available in this state;
   * workers and every subsequent durable mutation are fenced until restart.
   */
  bool durable_mutation_faulted_locked() const noexcept {
    return journal_faulted_ || artifact_erase_faulted_;
  }

  /**
   * @brief Rejects a new mutation after any irreversible durability failure.
   * @return Nothing when durable mutation remains available.
   * @throws DurableStateError when restart reconciliation is required.
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
   * @note Called either during single-threaded construction before the reaper
   * and public access begin, or while the caller holds `mutex_`. Synchronous
   * metadata I/O is the current single-tenant linearization policy.
   */
  void persist_snapshot_locked(const JobSnapshot& snapshot);

  /** @brief Sole configured tenant authority. */
  TenantId tenant_id_;
  /** @brief Shared factory retained until every worker joins. */
  std::shared_ptr<JobAttemptWorkerFactory> worker_factory_;
  /** @brief Sole crash-durable Job/artifact state-root authority. */
  std::unique_ptr<DurableServerState> durable_state_;
  /** @brief Sole complete-envelope tenant quota authority. */
  std::unique_ptr<TenantQuotaAuthority> quota_authority_;
  /** @brief Collision-resistant service namespace used by durable identities.
   */
  std::string identity_namespace_;
  /** @brief Next checked Job identity sequence in this service namespace. */
  std::uint64_t next_job_sequence_ = 1U;
  /** @brief Serializes all Job truth and worker-ownership mutations. */
  mutable std::mutex mutex_;
  /**
   * @brief Wakes Job observers, the worker reaper, and ownership test waits.
   */
  mutable std::condition_variable condition_;
  /** @brief JobId text to authoritative copied current record. */
  std::map<std::string, JobControlRecord> jobs_;
  /** @brief JobAttemptId text to joinable record pending reaper ownership. */
  std::map<std::string, WorkerThreadRecord> workers_;
  /** @brief Completed handles currently moved out for an unlocked join. */
  std::size_t workers_joining_ = 0U;
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
  /** @brief Sole bounded infrastructure thread that joins worker handles. */
  std::thread reaper_;
};

}  // namespace ps::server
