/**
 * @file durable_server_state.hpp
 * @brief Declares Issue #99 durable Job and named-Value artifact authority.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "server/state/job_contract.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Base failure for durable state validation or filesystem authority.
 * @throws std::bad_alloc only while constructing diagnostic storage.
 */
class DurableStateError : public std::runtime_error {
 public:
  /** @brief Inherits diagnostic construction from `std::runtime_error`. */
  using std::runtime_error::runtime_error;
};

/**
 * @brief Requested crash-durability primitive is unavailable.
 * @throws std::bad_alloc only while constructing diagnostic storage.
 */
class DurableCapabilityError final : public DurableStateError {
 public:
  /** @brief Inherits diagnostic construction from `DurableStateError`. */
  using DurableStateError::DurableStateError;
};

/**
 * @brief Durable bytes or identity conflict with retained authority.
 * @throws std::bad_alloc only while constructing diagnostic storage.
 */
class DurableConflictError final : public DurableStateError {
 public:
  /** @brief Inherits diagnostic construction from `DurableStateError`. */
  using DurableStateError::DurableStateError;
};

/**
 * @brief Recovered state is malformed, ambiguous, or content-inconsistent.
 * @throws std::bad_alloc only while constructing diagnostic storage.
 */
class DurableCorruptionError final : public DurableStateError {
 public:
  /** @brief Inherits diagnostic construction from `DurableStateError`. */
  using DurableStateError::DurableStateError;
};

/**
 * @brief Deterministic durable-artifact commit observation stages for tests.
 * @throws Nothing for value operations.
 * @note The hook observes real product transitions and grants no path or
 * mutation authority. Production leaves the hook empty.
 */
enum class DurableArtifactCommitStage : std::uint8_t {
  /** @brief ArtifactId cache node is prepared in private index storage. */
  ArtifactIndexPrepared,
  /** @brief OutputCommitId cache node is prepared in private index storage. */
  CommitIndexPrepared,
  /** @brief Payload is synchronized and revalidated; manifest is absent. */
  PayloadSynchronized,
  /** @brief Authoritative manifest link exists; parent barriers are pending. */
  ManifestPublished,
  /** @brief Exact pending manifest/payload passed revalidation. */
  DurabilityRevalidationStarted,
  /** @brief Pending artifact-directory barrier is about to be replayed. */
  ArtifactDirectoryBarrierReplay,
  /** @brief Pending artifacts-directory barrier is about to be replayed. */
  ArtifactsDirectoryBarrierReplay,
  /** @brief Pending durability-root barrier is about to be replayed. */
  RootDirectoryBarrierReplay,
  /** @brief Every leaf-to-root directory barrier completed. */
  DirectoryBarriersCompleted,
};

/**
 * @brief Observable state of one authoritative artifact-visibility removal.
 * @throws Nothing for value operations.
 * @note The states form an irreversible transaction order. Once visibility is
 * removed in-process, both ArtifactId and OutputCommitId indexes are already
 * revoked. Only the last two states authorize retained-quota release.
 */
enum class DurableArtifactEraseState : std::uint8_t {
  /** @brief Manifest visibility was not removed; prior truth remains valid. */
  NotRemoved,
  /** @brief Manifest is absent in-process, but durability is unconfirmed. */
  ManifestRemovedDurabilityUnconfirmed,
  /** @brief Visibility removal is durable; private cleanup remains pending. */
  VisibilityRemovalConfirmedCleanupPending,
  /** @brief Visibility, payload/directory cleanup, and barriers completed. */
  FullyCleaned,
};

/**
 * @brief Deterministic artifact-deletion observation stages for maintained
 * tests.
 * @throws Nothing for value operations.
 * @note The observer runs while the durable-state mutex excludes readers. Its
 * exception is captured in `DurableArtifactEraseResult`; production leaves
 * the observer empty.
 */
enum class DurableArtifactEraseStage : std::uint8_t {
  /** @brief All throwing cache preparation precedes manifest mutation. */
  BeforeManifestRemoval,
  /** @brief Manifest is absent and both in-memory indexes are revoked. */
  ManifestRemoved,
  /** @brief The artifact-directory visibility barrier completed. */
  ArtifactDirectorySynchronized,
  /** @brief The artifacts-directory visibility barrier completed. */
  ArtifactsDirectorySynchronized,
  /** @brief Root barrier confirmed durable visibility removal. */
  VisibilityRemovalConfirmed,
  /** @brief The payload leaf is absent. */
  PayloadRemoved,
  /** @brief Any private-manifest residue is absent. */
  PrivateManifestRemoved,
  /** @brief The cleaned artifact directory itself is synchronized. */
  ArtifactDirectoryCleanupSynchronized,
  /** @brief The artifact-directory descriptor was closed. */
  ArtifactDirectoryClosed,
  /** @brief The now-empty artifact directory was removed. */
  ArtifactDirectoryRemoved,
  /** @brief The artifacts-directory cleanup barrier completed. */
  ArtifactsDirectoryCleanupSynchronized,
  /** @brief Every cleanup barrier through the durability root completed. */
  CleanupBarriersCompleted,
};

/**
 * @brief Allocation-free outcome of one durable artifact-deletion transaction.
 * @throws Nothing for construction, copying, and observation.
 * @note `payload_bytes` is the exact known retained charge for this occurrence,
 * or zero on an already-absent retry whose quota authority remains the exact
 * charge source. A non-null `failure` describes an operation/observer failure
 * at the reported irreversible state.
 */
struct DurableArtifactEraseResult final {
  /** @brief Furthest irreversible visibility/cleanup state reached. */
  DurableArtifactEraseState state = DurableArtifactEraseState::NotRemoved;
  /** @brief Exact known archive charge, or zero when not recoverable. */
  std::uint64_t payload_bytes = 0U;
  /** @brief Captured original deletion failure, or null after clean success. */
  std::exception_ptr failure;

  /**
   * @brief Reports whether authoritative visibility is absent in-process.
   * @return False only for `NotRemoved`.
   * @throws Nothing.
   */
  bool visibility_removed() const noexcept {
    return state != DurableArtifactEraseState::NotRemoved;
  }

  /**
   * @brief Reports whether parent barriers confirmed visibility removal.
   * @return True for cleanup-pending or fully-cleaned states.
   * @throws Nothing.
   */
  bool visibility_removal_confirmed() const noexcept {
    return state == DurableArtifactEraseState::
                        VisibilityRemovalConfirmedCleanupPending ||
           state == DurableArtifactEraseState::FullyCleaned;
  }

  /**
   * @brief Reports whether all cleanup and directory barriers completed.
   * @return True only for `FullyCleaned`, including lost final acknowledgement.
   * @throws Nothing.
   */
  bool cleanup_completed() const noexcept {
    return state == DurableArtifactEraseState::FullyCleaned;
  }

  /**
   * @brief Reports a clean fully durable deletion outcome.
   * @return True only for fully cleaned state without a captured failure.
   * @throws Nothing.
   */
  bool succeeded() const noexcept {
    return cleanup_completed() && failure == nullptr;
  }

  /**
   * @brief Rethrows the captured original failure when present.
   * @return Nothing.
   * @throws The exact exception captured by `erase_artifact`.
   */
  void rethrow_failure() const {
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  }
};

/**
 * @brief Typed service report for commit-unknown or cleanup-pending deletion.
 * @throws std::bad_alloc only while constructing base diagnostic storage.
 * @note The service has already revoked lookup visibility and either retained
 * quota for unconfirmed visibility or released its exact charge for confirmed
 * visibility. It then enters monotonic durable-mutation fail-stop.
 */
class DurableArtifactEraseError final : public DurableStateError {
 public:
  /**
   * @brief Captures the affected artifact and irreversible deletion state.
   * @param artifact_id Exact deleted artifact identity.
   * @param state Visibility-removed deletion state.
   * @param payload_bytes Exact known retained charge, or zero on absent retry.
   * @param cause Original filesystem/observer/quota failure.
   * @throws std::invalid_argument for invalid identity, `NotRemoved`, or null
   * cause.
   * @throws std::bad_alloc while constructing diagnostic storage.
   */
  DurableArtifactEraseError(ArtifactId artifact_id,
                            DurableArtifactEraseState state,
                            std::uint64_t payload_bytes,
                            std::exception_ptr cause);

  /** @brief Returns the exact affected artifact identity. */
  const ArtifactId& artifact_id() const noexcept { return artifact_id_; }

  /** @brief Returns the furthest irreversible deletion state. */
  DurableArtifactEraseState state() const noexcept { return state_; }

  /** @brief Returns the exact known retained charge, or zero when unknown. */
  std::uint64_t payload_bytes() const noexcept { return payload_bytes_; }

  /**
   * @brief Rethrows the original deletion/quota failure.
   * @return Nothing.
   * @throws The exact captured cause.
   */
  void rethrow_cause() const { std::rethrow_exception(cause_); }

 private:
  /** @brief Exact artifact whose visibility transition became irreversible. */
  ArtifactId artifact_id_;
  /** @brief Furthest irreversible visibility/cleanup state. */
  DurableArtifactEraseState state_;
  /** @brief Exact known retained payload charge, or zero on absent retry. */
  std::uint64_t payload_bytes_ = 0U;
  /** @brief Original filesystem, observer, or quota-coordination failure. */
  std::exception_ptr cause_;
};

/**
 * @brief Observable durability state of one Job-record replacement attempt.
 * @throws Nothing for value operations.
 * @note `RecordPublishedDurabilityUnconfirmed` is commit-unknown to the
 * caller: the atomic replacement is visible, but one or more required parent
 * directory barriers did not complete. The published record must never be
 * treated as rolled back or deleted by the caller.
 */
enum class DurableJobCommitState : std::uint8_t {
  /** @brief Atomic replacement did not occur; the prior record remains truth.
   */
  NotPublished,
  /** @brief Replacement occurred, but crash durability is not confirmed. */
  RecordPublishedDurabilityUnconfirmed,
  /** @brief Replacement and every required directory barrier completed. */
  ConfirmedCommitted,
};

/**
 * @brief Deterministic Job-journal observation stages for maintained tests.
 * @throws Nothing for value operations.
 * @note The observer runs while the durable-state mutex excludes readers. Its
 * exception is converted into `DurableJobCommitResult`; it grants no path or
 * mutation authority. Production leaves the observer empty.
 */
enum class DurableJobCommitStage : std::uint8_t {
  /** @brief Replacement cache storage is prepared before filesystem mutation.
   */
  CachePrepared,
  /** @brief The private replacement file is synchronized. */
  PrivateFileSynchronized,
  /** @brief Atomic rename made the replacement record visible. */
  RecordPublished,
  /** @brief The Job-record directory barrier completed. */
  JobsDirectorySynchronized,
  /** @brief The control-directory barrier completed. */
  ControlDirectorySynchronized,
  /** @brief The durability-root barrier completed. */
  DirectoryBarriersCompleted,
};

/**
 * @brief Allocation-free outcome of one durable Job-record transaction.
 * @throws Nothing for construction, copying, and observation.
 * @note A non-null `failure` describes an operation or deterministic observer
 * failure at the reported state. A clean success is exactly
 * `ConfirmedCommitted` with no failure.
 */
struct DurableJobCommitResult final {
  /** @brief Furthest irreversible publication/durability state reached. */
  DurableJobCommitState state = DurableJobCommitState::NotPublished;
  /** @brief Captured original failure, or null after clean confirmed commit. */
  std::exception_ptr failure;

  /**
   * @brief Reports whether publication and every barrier completed cleanly.
   * @return True only for confirmed commit without a captured failure.
   * @throws Nothing.
   */
  bool succeeded() const noexcept {
    return state == DurableJobCommitState::ConfirmedCommitted &&
           failure == nullptr;
  }

  /**
   * @brief Reports whether atomic record publication occurred.
   * @return False only for `NotPublished`.
   * @throws Nothing.
   */
  bool published() const noexcept {
    return state != DurableJobCommitState::NotPublished;
  }

  /**
   * @brief Rethrows the captured original failure when present.
   * @return Nothing.
   * @throws The exact exception captured by `persist_job`.
   */
  void rethrow_failure() const {
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  }
};

/**
 * @brief Typed service-level report of an ambiguous or lost Job-journal ack.
 * @throws std::bad_alloc only while constructing base diagnostic storage.
 * @note The service has already aligned process-local Job truth with the
 * published record and entered fail-stop before throwing this value. Restart
 * is required before another durable mutation.
 */
class DurableJobCommitError final : public DurableStateError {
 public:
  /**
   * @brief Captures the exact Job and furthest journal commit state.
   * @param job_id Durable Job whose replacement was published.
   * @param state Published or confirmed commit state.
   * @param cause Original filesystem/observer failure.
   * @throws std::invalid_argument when `job_id` is invalid or `state` says the
   * record was not published.
   * @throws std::bad_alloc while constructing diagnostic storage.
   */
  DurableJobCommitError(JobId job_id, DurableJobCommitState state,
                        std::exception_ptr cause);

  /**
   * @brief Returns the exact affected durable Job identity.
   * @return Borrowed immutable Job identity.
   * @throws Nothing.
   */
  const JobId& job_id() const noexcept { return job_id_; }

  /**
   * @brief Returns the furthest irreversible commit state.
   * @return Published-unconfirmed or confirmed-committed state.
   * @throws Nothing.
   */
  DurableJobCommitState state() const noexcept { return state_; }

  /**
   * @brief Rethrows the original persistence/observer failure when present.
   * @return Nothing.
   * @throws The exact captured cause.
   */
  void rethrow_cause() const {
    if (cause_ != nullptr) {
      std::rethrow_exception(cause_);
    }
  }

 private:
  /** @brief Exact Job whose replacement crossed atomic publication. */
  JobId job_id_;
  /** @brief Furthest irreversible commit state. */
  DurableJobCommitState state_;
  /** @brief Original persistence or deterministic observer failure. */
  std::exception_ptr cause_;
};

/**
 * @brief Source-private durable-state configuration and deterministic seam.
 * @throws Nothing for default construction; function copies may allocate.
 */
struct DurableServerStateOptions final {
  /**
   * @brief Optional observer/fault injector invoked at exact commit stages.
   * @note An exception before manifest publication causes safe private cleanup.
   * An exception after publication preserves manifest-visible aliases in a
   * durability-pending state. Lookup/retry then performs exact revalidation
   * and the complete barrier replay before returning a crash-durable receipt.
   * Once the root barrier completes, confirmation is recorded before the
   * final `DirectoryBarriersCompleted` observer acknowledgement.
   */
  std::function<void(DurableArtifactCommitStage)> artifact_commit_observer;
  /**
   * @brief Optional observer/fault injector for artifact deletion transitions.
   * @note Exceptions are captured in `DurableArtifactEraseResult`. A failure
   * before manifest removal preserves both indexes; every later failure keeps
   * them revoked and reports the exact irreversible deletion state.
   */
  std::function<void(DurableArtifactEraseStage)> artifact_erase_observer;
  /**
   * @brief Optional observer/fault injector for Job-journal transitions.
   * @note Exceptions are captured in `DurableJobCommitResult`. An exception
   * before `RecordPublished` restores the prior cache and permits ordinary
   * caller rollback; an exception at or after publication preserves the new
   * cache truth and requires reconciliation/fail-stop.
   */
  std::function<void(DurableJobCommitStage)> job_commit_observer;
};

/**
 * @brief Server-owned stable Value artifact commit request for one attempt.
 * @throws Nothing for default/value operations; copied fields may allocate.
 * @note `artifact_id` and `output_commit_id` are allocated at initial Job
 * acceptance and preserved across retry. Workers never construct this value.
 */
struct DurableArtifactCommitRequest final {
  /** @brief Current assignment producing the candidate. */
  AttemptIdentity attempt;
  /** @brief Exact declared required output slot. */
  OutputSlotId output_slot_id;
  /** @brief Stable immutable artifact identity for the Job/slot. */
  ArtifactId artifact_id;
  /** @brief Stable idempotent transaction identity for the Job/slot. */
  OutputCommitId output_commit_id;
  /** @brief Server-reserved output/staging/retention bounds. */
  JobResourceRequest reserved_resources;
};

/**
 * @brief Complete durable control-plane record for one accepted Job.
 * @throws Nothing for default/value operations; copies may allocate.
 * @note Runtime Host/Graph/Run/thread/ledger objects are intentionally absent.
 */
struct DurableJobRecord final {
  /** @brief Configured tenant owner. */
  TenantId tenant_id;
  /** @brief Durable accepted Job identity. */
  JobId job_id;
  /** @brief Exact immutable accepted JobSpec. */
  std::shared_ptr<const JobSpec> spec;
  /** @brief Current complete assignment tuple. */
  AttemptIdentity assignment;
  /** @brief Stable artifact identity reserved for the required output. */
  ArtifactId output_artifact_id;
  /** @brief Stable idempotent output transaction identity. */
  OutputCommitId output_commit_id;
  /** @brief Current control-plane Job state. */
  JobState state = JobState::Queued;
  /** @brief Monotonic cancellation intent for the current attempt. */
  bool cancellation_requested = false;
  /** @brief Exact current-attempt settlement fact. */
  bool attempt_settled = false;
  /** @brief Accepted current-attempt terminal outcome, if any. */
  JobAttemptOutcome attempt_outcome = JobAttemptOutcome::None;
  /** @brief Accepted worker/control-plane failure fact, if any. */
  JobAttemptFailure failure = JobAttemptFailure::None;
  /** @brief Human-readable diagnostic encoded without authority. */
  std::string message;
  /** @brief Durable required output receipt for successful Job state. */
  std::optional<OutputCommitReceipt> output_receipt;
};

/**
 * @brief Sole rooted persistence authority for Issue #99 control/artifact data.
 *
 * Construction canonicalizes and exclusively locks one trusted state root,
 * retains root/control/jobs/artifacts directory capabilities, removes only
 * unambiguous private residue, validates all complete records, reapplies
 * durability barriers, and reconstructs immutable indexes. Artifact commit
 * uses payload-first/private-manifest/manifest-last publication followed by
 * leaf-to-root directory barriers. Job records use synchronized private files
 * plus atomic replacement.
 *
 * @throws Constructor propagates filesystem/system/allocation failures and
 * typed capability/conflict/corruption errors. Method-specific behavior is
 * documented below.
 * @note The root is trusted configuration and one live process owns it. No
 * JobSpec, report, checkpoint, plugin, or caller-provided id selects a path
 * outside fixed internal namespaces. This object provides crash durability,
 * not worker process isolation or OS quota enforcement.
 */
class DurableServerState final {
 public:
  /**
   * @brief Opens, locks, initializes, and recovers one tenant state root.
   * @param root Trusted state-root path, created when absent.
   * @param tenant_id Valid configured tenant bound into every record.
   * @param options Optional source-private commit observer/fault seam.
   * @throws std::invalid_argument for invalid tenant/root input.
   * @throws DurableCapabilityError when lock/barrier/publication primitives are
   * unsupported.
   * @throws DurableCorruptionError for malformed or ambiguous retained state.
   * @throws std::system_error for other filesystem operations.
   */
  DurableServerState(std::filesystem::path root, TenantId tenant_id,
                     DurableServerStateOptions options = {});

  /**
   * @brief Closes all held directory capabilities and releases the root lock.
   * @throws Nothing; an unexpected close failure terminates.
   */
  ~DurableServerState() noexcept;

  /**
   * @brief Prevents duplicate ownership of one locked durability root.
   * @param other Authority that cannot be copied.
   */
  DurableServerState(const DurableServerState& other) = delete;

  /**
   * @brief Prevents duplicate assignment of one locked durability root.
   * @param other Authority that cannot be copied.
   * @return No assignment result because the operation is deleted.
   */
  DurableServerState& operator=(const DurableServerState& other) = delete;

  /**
   * @brief Keeps the held root capability and lock at a stable address.
   * @param other Authority that cannot be moved.
   */
  DurableServerState(DurableServerState&& other) = delete;

  /**
   * @brief Keeps held directory capabilities stable during assignment.
   * @param other Authority that cannot be moved.
   * @return No assignment result because the operation is deleted.
   */
  DurableServerState& operator=(DurableServerState&& other) = delete;

  /**
   * @brief Returns the canonical trusted root for diagnostics/tests only.
   * @return Borrowed canonical path valid for this object's lifetime.
   * @throws Nothing.
   * @note Mutation authority remains the retained descriptors, not this path.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

  /**
   * @brief Returns all durability-confirmed immutable artifacts.
   * @return Snapshot vector in unspecified identity order after any pending
   * entries are exactly revalidated and their full barriers replayed.
   * @throws std::bad_alloc while copying retained shared owners.
   * @throws DurableCorruptionError/system failures during pending revalidation.
   */
  std::vector<std::shared_ptr<const ArtifactRecord>> recovered_artifacts()
      const;

  /**
   * @brief Returns all strictly recovered durable Job records.
   * @return Snapshot vector in unspecified identity order.
   * @throws std::bad_alloc while copying records.
   */
  std::vector<DurableJobRecord> recovered_jobs() const;

  /**
   * @brief Publishes or reconciles one crash-durable named-Value artifact set.
   * @param request Current server-owned stable transaction request.
   * @param values Valid nonempty canonical named-Value artifact candidate.
   * @return Original or newly committed identity-complete durable receipt.
   * @throws std::invalid_argument for invalid request/artifact/quota bounds.
   * @throws DurableConflictError for same-id identity/content conflict.
   * @throws DurableCapabilityError when a required primitive is unsupported.
   * @throws DurableCorruptionError for retained namespace/content drift.
   * @throws std::system_error for other I/O failures.
   * @note An exception after manifest publication preserves both aliases as a
   * manifest-visible, durability-pending occurrence. A same-commit retry must
   * revalidate exact bytes and replay every directory barrier before this
   * method can return a crash-durable receipt.
   */
  OutputCommitReceipt commit_artifact(
      const DurableArtifactCommitRequest& request,
      const NamedValueArtifactSet& values);

  /**
   * @brief Looks up and, when needed, lazily revalidates one durable artifact.
   * @param artifact_id Valid tenant-scoped immutable identity.
   * @return Shared immutable record, or null when no manifest is committed.
   * @throws std::invalid_argument for an invalid identity.
   * @throws DurableCorruptionError/system/allocation failures during lazy load
   * or durability-pending exact revalidation/barrier replay.
   * @note A manifest-visible alias remains internally recognizable while
   * pending, but this method returns it only after durability confirmation.
   */
  std::shared_ptr<const ArtifactRecord> find_artifact(
      const ArtifactId& artifact_id) const;

  /**
   * @brief Finds one committed occurrence by stable idempotency identity.
   * @param output_commit_id Valid stable transaction identity.
   * @return Shared immutable record, or null when absent.
   * @throws std::invalid_argument for an invalid identity.
   * @throws DurableCorruptionError/system/allocation failures during
   * durability-pending exact revalidation/barrier replay.
   * @note A manifest-visible alias remains internally recognizable while
   * pending, but this method returns it only after durability confirmation.
   */
  std::shared_ptr<const ArtifactRecord> find_commit(
      const OutputCommitId& output_commit_id) const;

  /**
   * @brief Durably removes one committed artifact's authoritative visibility.
   * @param artifact_id Valid exact artifact identity.
   * @return Explicit visibility/cleanup state, exact known payload charge, and
   * any captured failure.
   * @throws Nothing; validation, allocation, observer, and filesystem failures
   * are captured in the returned result.
   * @note Both indexes are pre-staged before filesystem mutation. Once manifest
   * visibility is absent, they are revoked together by no-throw swaps. Quota
   * may be released only when `visibility_removal_confirmed()` is true; a
   * remaining cleanup failure is restart-recoverable residue.
   */
  DurableArtifactEraseResult erase_artifact(
      const ArtifactId& artifact_id) noexcept;

  /**
   * @brief Atomically persists one complete accepted Job truth record.
   * @param record Fully joined current control-plane truth.
   * @return Explicit publication/durability state plus any captured failure.
   * @throws Nothing; validation, allocation, observer, and filesystem failures
   * are captured in the returned result.
   * @note All serialization and cache allocation occur before atomic rename.
   * `NotPublished` leaves the prior record/cache authoritative. Any published
   * state keeps the replacement in the no-throw cache even when a later
   * directory barrier or response-stage observer fails.
   */
  DurableJobCommitResult persist_job(const DurableJobRecord& record) noexcept;

  /**
   * @brief Durably removes one rolled-back pre-acceptance Job record.
   * @param job_id Valid exact Job identity.
   * @return True when a record was removed; false when already absent.
   * @throws std::invalid_argument for an invalid identity.
   * @throws DurableCapabilityError/system failures from persistence.
   */
  bool erase_job(const JobId& job_id);

 private:
  /**
   * @brief Confirms one internally visible artifact before external return.
   * @param record Non-null exact record retained by both aliases.
   * @return The same immutable record after exact disk validation and, when
   * pending, complete leaf-to-root barrier replay.
   * @throws std::invalid_argument for a null record.
   * @throws DurableCorruptionError when cache, status, manifest, or payload
   * truth diverges.
   * @throws std::bad_alloc or std::system_error from validation/barrier I/O.
   * @throws Any deterministic observer exception unchanged.
   * @note The caller holds `mutex_`. Confirmation is a single no-throw Boolean
   * transition under that mutex and is published before the final completion
   * observer, so a lost acknowledgement cannot erase completed barrier truth.
   */
  std::shared_ptr<const ArtifactRecord> confirm_artifact_durability_locked(
      const std::shared_ptr<const ArtifactRecord>& record) const;

  /** @brief Canonical trusted state-root path retained for binding checks. */
  std::filesystem::path root_;
  /** @brief Configured tenant bound into every record. */
  TenantId tenant_id_;
  /** @brief Source-private deterministic stage observer. */
  DurableServerStateOptions options_;
  /** @brief Serializes filesystem transactions and in-memory indexes. */
  mutable std::mutex mutex_;
  /** @brief Held locked root directory capability. */
  int root_descriptor_ = -1;
  /** @brief Held fixed control directory capability. */
  int control_descriptor_ = -1;
  /** @brief Held fixed Job-record directory capability. */
  int jobs_descriptor_ = -1;
  /** @brief Held fixed artifact directory capability. */
  int artifacts_descriptor_ = -1;
  /** @brief Constructor-time root device identity. */
  std::uint64_t root_device_ = 0U;
  /** @brief Constructor-time root inode identity. */
  std::uint64_t root_inode_ = 0U;
  /** @brief Loaded immutable artifacts keyed by ArtifactId text. */
  mutable std::unordered_map<std::string, std::shared_ptr<const ArtifactRecord>>
      artifacts_;
  /** @brief Loaded artifact aliases keyed by OutputCommitId text. */
  mutable std::unordered_map<std::string, std::shared_ptr<const ArtifactRecord>>
      commits_;
  /**
   * @brief ArtifactId-keyed manifest-visible durability confirmation truth.
   * @note This map is prepared, installed, rolled back, and revoked in the
   * same mutex transaction as both alias indexes. Restart reconstructs only
   * confirmed entries after exact validation and barrier replay.
   */
  mutable std::unordered_map<std::string, bool> artifact_durability_confirmed_;
  /** @brief Loaded durable Job records keyed by JobId text. */
  std::unordered_map<std::string, DurableJobRecord> jobs_;
  /** @brief Checked private Job-record filename sequence. */
  std::uint64_t next_private_record_sequence_ = 1U;
};

}  // namespace ps::server
