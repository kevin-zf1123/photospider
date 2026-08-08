/**
 * @file single_tenant_job_service.hpp
 * @brief Declares Issue #98 Job truth and process-lifetime artifact authority.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "server/job_contract.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Closed control-plane Job lifecycle for the Issue #98 profile.
 * @throws Nothing for value operations.
 */
enum class JobState : std::uint8_t {
  /** @brief Accepted immutable Job whose worker thread has not entered. */
  Queued,
  /** @brief Current assignment is executing. */
  Running,
  /** @brief Monotonic cancellation intent awaits worker settlement. */
  Cancelling,
  /** @brief Current settled attempt and required artifact receipt succeeded. */
  Succeeded,
  /** @brief Current attempt or report validation failed. */
  Failed,
  /** @brief Accepted cancellation won before commit and worker settled. */
  Cancelled,
};

/**
 * @brief Reports whether a Job state is terminal.
 * @param state Candidate lifecycle state.
 * @return True for Succeeded, Failed, or Cancelled.
 * @throws std::invalid_argument for an invalid enum representation.
 */
bool is_terminal_job_state(JobState state);

/**
 * @brief Closed outcome facts a worker may report for one attempt.
 * @throws Nothing for value operations.
 * @note A worker outcome never publishes overall Job state.
 */
enum class JobAttemptOutcome : std::uint8_t {
  /** @brief Settled Host compute returned one image and no failure. */
  Succeeded,
  /** @brief A typed worker failure returned no image; settlement stays exact.
   */
  Failed,
  /** @brief Worker observed cancellation, settled, and returned no image. */
  Cancelled,
};

/**
 * @brief Stable failure category for one worker attempt fact.
 * @throws Nothing for value operations.
 */
enum class JobAttemptFailure : std::uint8_t {
  /** @brief No failure is present; valid only with successful outcome. */
  None,
  /** @brief Cancellation was observed; valid only with cancelled outcome. */
  CancellationObserved,
  /** @brief Assignment or immutable JobSpec validation failed. */
  InvalidAssignment,
  /** @brief Trusted graph artifact resolution failed. */
  GraphResolution,
  /** @brief Embedded Host construction or setup failed. */
  HostSetup,
  /** @brief Graph loading failed. */
  GraphLoad,
  /** @brief Host compute or output validation failed. */
  Compute,
  /** @brief Loaded graph close/settlement failed. */
  Settlement,
  /** @brief Control-plane-only rejection; never valid in a worker report. */
  ReportRejected,
  /** @brief Control-plane-only artifact commit failure. */
  ArtifactCommit,
  /** @brief An unexpected exception crossed the worker adapter. */
  Unexpected,
};

/**
 * @brief Complete immutable assignment delegated to one Issue #98 worker.
 * @throws Nothing for default construction; copies may allocate.
 * @note `spec` is shared read-only and its digest must equal
 * `identity.job_spec_digest` before any graph resolution.
 */
struct JobAssignment final {
  /** @brief Full retained current assignment identity. */
  AttemptIdentity identity;
  /** @brief Immutable accepted JobSpec shared with this worker only. */
  std::shared_ptr<const JobSpec> spec;
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
 * is not an OS-process or security-isolation claim.
 */
class JobAttemptWorker {
 public:
  /** @brief Destroys worker-private resources after its attempt. */
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
  /** @brief Destroys factory-owned configuration after all workers. */
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
 * @brief Complete request to commit one worker-produced image candidate.
 * @throws Nothing for default construction; copies may allocate.
 */
struct ArtifactCommitRequest final {
  /** @brief Exact assignment authorized by the control plane. */
  AttemptIdentity attempt;
  /** @brief Exact required output slot being fulfilled. */
  OutputSlotId output_slot_id;
  /** @brief Borrowed image descriptor retained through synchronous commit. */
  ImageBuffer image;
};

/**
 * @brief Separate in-memory authority for immutable process-lifetime artifacts.
 *
 * Commit validates one nonempty CPU image, copies active rows to tight
 * immutable storage, hashes those bytes, and mints independent artifact/commit
 * ids. The store intentionally has no filesystem, restart recovery, quota,
 * retention, TTL, idempotency, IPC delivery, or durable-manifest claim.
 *
 * @throws Nothing for construction; methods document validation/allocation
 * failures.
 * @note Every method is thread-safe. Returned records are immutable shared
 * owners and remain valid after store destruction.
 */
class ProcessLifetimeArtifactStore final {
 public:
  /** @brief Creates an empty process-lifetime artifact authority. */
  ProcessLifetimeArtifactStore() = default;

  /**
   * @brief Atomically commits one valid image under an exact assignment.
   * @param request Complete commit request.
   * @return Identity-complete process-lifetime receipt.
   * @throws std::invalid_argument for incomplete identities, invalid slot,
   * empty/non-CPU/malformed image, or invalid enum representation.
   * @throws std::overflow_error for unrepresentable payload arithmetic.
   * @throws std::bad_alloc when staging, hashing, ids, or storage exhaust
   * memory.
   * @note The source is copied before publication; later source mutation cannot
   * alter the artifact. Equal content never deduplicates identity.
   */
  OutputCommitReceipt commit(const ArtifactCommitRequest& request);

  /**
   * @brief Looks up one immutable artifact by ArtifactId.
   * @param artifact_id Exact artifact identity.
   * @return Shared immutable record, or null when absent.
   * @throws std::bad_alloc only if map lookup implementation allocates.
   * @note Lookup grants observation, not mutation or another authority domain.
   */
  std::shared_ptr<const ArtifactRecord> find(
      const ArtifactId& artifact_id) const;

  /**
   * @brief Returns the exact number of published artifact versions.
   * @return Current process-lifetime record count.
   * @throws Nothing.
   */
  std::size_t size() const noexcept;

 private:
  /** @brief Serializes publication and lookup of immutable records. */
  mutable std::mutex mutex_;
  /** @brief ArtifactId text to immutable record owner. */
  std::map<std::string, std::shared_ptr<const ArtifactRecord>> records_;
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
  /** @brief Exact first and only Issue #98 assignment tuple. */
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
  /** @brief Authoritative control-plane lifecycle state. */
  JobState state = JobState::Queued;
  /** @brief Whether cancellation intent was accepted monotonically. */
  bool cancellation_requested = false;
  /** @brief Whether the current worker reported completed settlement. */
  bool attempt_settled = false;
  /** @brief Worker attempt outcome when a report was accepted. */
  std::optional<JobAttemptOutcome> attempt_outcome;
  /** @brief Typed current attempt/control failure, if any. */
  JobAttemptFailure failure = JobAttemptFailure::None;
  /** @brief Human-readable current diagnostic. */
  std::string message;
  /** @brief Sole required output receipt, present only after successful commit.
   */
  std::optional<OutputCommitReceipt> output_receipt;
};

/**
 * @brief Source-private single-tenant Issue #98 Job control-plane owner.
 *
 * The service validates and freezes JobSpec values, mints one current
 * assignment, starts one fresh in-process worker, records monotonic
 * cancellation intent, validates the complete returned tuple, linearizes
 * cancellation versus artifact commit, and alone publishes Job terminal state.
 *
 * @throws std::invalid_argument when construction receives an invalid tenant or
 * null factory.
 * @note Methods are thread-safe. Destruction requests cancellation for active
 * Jobs and joins every worker, but cannot bound a provider that ignores current
 * cooperative observations. Callers must not race object destruction with a
 * new public method call.
 */
class SingleTenantJobService final {
 public:
  /**
   * @brief Creates an empty Job authority for one configured tenant.
   * @param tenant_id Exact sole tenant identity.
   * @param worker_factory Non-null fresh-worker factory.
   * @throws std::invalid_argument for invalid configuration.
   */
  SingleTenantJobService(
      TenantId tenant_id,
      std::shared_ptr<JobAttemptWorkerFactory> worker_factory);

  /**
   * @brief Requests cancellation and joins all process-local workers.
   * @throws Nothing; thread-join system failures terminate because destructor
   * recovery cannot preserve object lifetime.
   */
  ~SingleTenantJobService() noexcept;

  /** @brief Prevents duplicate control-plane authority ownership. */
  SingleTenantJobService(const SingleTenantJobService&) = delete;
  /** @brief Prevents duplicate control-plane authority assignment. */
  SingleTenantJobService& operator=(const SingleTenantJobService&) = delete;

  /**
   * @brief Validates, freezes, accepts, and starts one immutable Job.
   * @param spec Complete supported JobSpec copied into shared const storage.
   * @return Accepted Job/digest/current assignment receipt.
   * @throws std::invalid_argument for invalid JobSpec or service shutdown.
   * @throws std::bad_alloc when Job state or worker setup exhausts memory.
   * @throws std::system_error when process-local worker-thread creation fails.
   * @note The complete return value is allocated before acceptance. Acceptance
   * linearizes only after state insertion and successful worker-thread start;
   * the remaining return path is a non-throwing move. Acceptance creates no
   * retry policy or server quota authority.
   */
  JobSubmission submit(JobSpec spec);

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
   * @throws Nothing.
   * @note Terminal Jobs, absent Jobs, and repeated requests return false.
   * Active Host compute is not promised bounded preemption.
   */
  bool cancel(const JobId& job_id) noexcept;

  /**
   * @brief Looks up an immutable committed artifact by authoritative id.
   * @param artifact_id Exact ArtifactId from a receipt.
   * @return Shared immutable process-lifetime record, or null when absent.
   * @throws As `ProcessLifetimeArtifactStore::find`.
   */
  std::shared_ptr<const ArtifactRecord> find_artifact(
      const ArtifactId& artifact_id) const;

 private:
  /**
   * @brief Executes one worker and applies its report under control authority.
   * @param assignment Immutable accepted current assignment.
   * @return Nothing after terminal Job publication.
   * @throws Nothing; a null factory result and all worker/factory/report
   * exceptions become unsettled failed facts because this boundary has no
   * graph/Host settlement proof.
   */
  void run_assignment(JobAssignment assignment) noexcept;

  /**
   * @brief Reads monotonic cancellation state for a worker observer.
   * @param job_id Exact Job identity.
   * @return True when cancellation or service shutdown is active.
   * @throws Nothing.
   */
  bool cancellation_requested_for(const JobId& job_id) const noexcept;

  /**
   * @brief Validates and applies one worker report under the Job mutex.
   * @param expected Exact assignment owned by the invoking worker thread.
   * @param report Immutable attempt facts and candidate image.
   * @return Nothing after terminal state derivation and observer notification.
   * @throws Nothing; artifact exceptions become a typed failed Job.
   * @note Full identity, enum, outcome/failure/settlement/image shape, and
   * cancellation-context validation precede copying any report fact or
   * cancellation adjudication. A rejected report leaves `attempt_outcome`
   * unset and cannot establish settlement or an artifact receipt for the
   * retained current assignment, even when its untrusted `settled` field is
   * true.
   */
  void apply_report(const AttemptIdentity& expected,
                    JobAttemptReport report) noexcept;

  /** @brief Sole configured tenant authority. */
  TenantId tenant_id_;
  /** @brief Shared factory retained until every worker joins. */
  std::shared_ptr<JobAttemptWorkerFactory> worker_factory_;
  /** @brief Independent process-lifetime artifact authority. */
  ProcessLifetimeArtifactStore artifact_store_;
  /** @brief Serializes all Job truth and worker-list mutations. */
  mutable std::mutex mutex_;
  /** @brief Wakes bounded query observers after every meaningful transition. */
  mutable std::condition_variable condition_;
  /** @brief JobId text to authoritative copied current record. */
  std::map<std::string, JobSnapshot> jobs_;
  /** @brief Joinable worker threads, each assigned exactly once. */
  std::vector<std::thread> workers_;
  /** @brief Monotonic service destruction/cancellation intent. */
  bool shutting_down_ = false;
};

}  // namespace ps::server
