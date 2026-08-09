/**
 * @file durable_server_state.hpp
 * @brief Declares Issue #99 durable Job and image-artifact authority.
 */
#pragma once

#include <cstddef>
#include <cstdint>
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

#include "server/job_contract.hpp"  // NOLINT(build/include_subdir)

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
  /** @brief Payload is synchronized and revalidated; manifest is absent. */
  PayloadSynchronized,
  /** @brief Authoritative manifest link exists; parent barriers are pending. */
  ManifestPublished,
  /** @brief Every leaf-to-root directory barrier completed. */
  DirectoryBarriersCompleted,
};

/**
 * @brief Source-private durable-state configuration and deterministic seam.
 * @throws Nothing for default construction; function copies may allocate.
 */
struct DurableServerStateOptions final {
  /**
   * @brief Optional observer/fault injector invoked at exact commit stages.
   * @note An exception before manifest publication causes safe private cleanup.
   * An exception after publication preserves the committed occurrence for
   * retry/restart reconciliation.
   */
  std::function<void(DurableArtifactCommitStage)> artifact_commit_observer;
};

/**
 * @brief Server-owned stable image commit request for one current attempt.
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
   * @brief Returns all strictly recovered immutable artifacts.
   * @return Snapshot vector in unspecified identity order.
   * @throws std::bad_alloc while copying retained shared owners.
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
   * @brief Publishes or idempotently reconciles one crash-durable image.
   * @param request Current server-owned stable transaction request.
   * @param image Valid nonempty CPU image candidate.
   * @return Original or newly committed identity-complete durable receipt.
   * @throws std::invalid_argument for invalid request/image/quota bounds.
   * @throws DurableConflictError for same-id identity/content conflict.
   * @throws DurableCapabilityError when a required primitive is unsupported.
   * @throws DurableCorruptionError for retained namespace/content drift.
   * @throws std::system_error for other I/O failures.
   * @note An exception after manifest publication preserves that occurrence;
   * retry/restart reconciles it instead of publishing a second artifact.
   */
  OutputCommitReceipt commit_artifact(
      const DurableArtifactCommitRequest& request, const ImageBuffer& image);

  /**
   * @brief Looks up and, when needed, lazily revalidates one durable artifact.
   * @param artifact_id Valid tenant-scoped immutable identity.
   * @return Shared immutable record, or null when no manifest is committed.
   * @throws std::invalid_argument for an invalid identity.
   * @throws DurableCorruptionError/system/allocation failures during lazy load.
   */
  std::shared_ptr<const ArtifactRecord> find_artifact(
      const ArtifactId& artifact_id) const;

  /**
   * @brief Finds one committed occurrence by stable idempotency identity.
   * @param output_commit_id Valid stable transaction identity.
   * @return Shared immutable record, or null when absent.
   * @throws std::invalid_argument for an invalid identity.
   */
  std::shared_ptr<const ArtifactRecord> find_commit(
      const OutputCommitId& output_commit_id) const;

  /**
   * @brief Durably removes one committed artifact's authoritative visibility.
   * @param artifact_id Valid exact artifact identity.
   * @return Exact payload bytes removed, or zero when already absent.
   * @throws std::invalid_argument for an invalid identity.
   * @throws DurableCorruptionError/system failures for ambiguous retained
   * state.
   * @note Manifest removal and parent barriers complete before the charge may
   * be released by `TenantQuotaAuthority`.
   */
  std::uint64_t erase_artifact(const ArtifactId& artifact_id);

  /**
   * @brief Atomically persists one complete accepted Job truth record.
   * @param record Fully joined current control-plane truth.
   * @return Nothing after file and leaf-to-root directory barriers complete.
   * @throws std::invalid_argument for malformed identity/spec/state semantics.
   * @throws DurableCapabilityError/system/allocation failures from persistence.
   * @note The prior record remains authoritative until atomic replacement.
   */
  void persist_job(const DurableJobRecord& record);

  /**
   * @brief Durably removes one rolled-back pre-acceptance Job record.
   * @param job_id Valid exact Job identity.
   * @return True when a record was removed; false when already absent.
   * @throws std::invalid_argument for an invalid identity.
   * @throws DurableCapabilityError/system failures from persistence.
   */
  bool erase_job(const JobId& job_id);

 private:
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
  /** @brief Loaded durable Job records keyed by JobId text. */
  std::unordered_map<std::string, DurableJobRecord> jobs_;
  /** @brief Checked private Job-record filename sequence. */
  std::uint64_t next_private_record_sequence_ = 1U;
};

}  // namespace ps::server
