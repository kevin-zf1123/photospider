/**
 * @file worker_artifact_data_plane.hpp
 * @brief Declares the attempt-scoped Issue #105 worker artifact data plane.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Maximum bytes in one non-authorizing data-plane reference id.
 * @note References are exact metadata join keys. Direction-scoped inherited
 * descriptors, not reference text, grant access to an anonymous occurrence.
 */
inline constexpr std::size_t kMaximumWorkerDataPlaneReferenceBytes = 80U;

/**
 * @brief Metadata joining one authorized checkpoint to its read capability.
 * @throws Nothing for default construction; retained values may allocate.
 * @note The receipt contains artifact identity, provenance, descriptor,
 * digest, and durability but no payload, path, store root, or native handle.
 */
struct WorkerCheckpointDataReference final {
  /** @brief Manager-derived exact join key for this attempt and checkpoint. */
  std::string reference_id;
  /** @brief Complete crash-durable checkpoint receipt without payload bytes. */
  OutputCommitReceipt receipt;
};

/**
 * @brief Metadata authorizing one private attempt-local output stage.
 * @throws Nothing for default construction; retained values may allocate.
 * @note The value does not contain or mint a stable ArtifactId,
 * OutputCommitId, filesystem path, descriptor, quota token, or publication
 * authority.
 */
struct WorkerOutputStageReference final {
  /** @brief Manager-derived exact join key for this attempt and output slot. */
  std::string reference_id;
  /** @brief Sole immutable JobSpec output slot accepted from this stage. */
  OutputSlotId output_slot_id;
  /** @brief Inclusive tight-byte maximum delegated to this private stage. */
  std::size_t maximum_payload_bytes = 0U;
};

/**
 * @brief Complete metadata for one attempt's two direction-scoped data lanes.
 * @throws Nothing for default construction; retained values may allocate.
 * @note The checkpoint lane is absent when JobSpec declares no checkpoint.
 * The output lane is always present for the one supported image output.
 */
struct WorkerDataPlaneAssignment final {
  /** @brief Optional immutable checkpoint receipt and read-reference join. */
  std::optional<WorkerCheckpointDataReference> checkpoint;
  /** @brief Exact private output-stage write-reference and byte maximum. */
  WorkerOutputStageReference output;
};

/**
 * @brief Worker-produced metadata describing bytes in its private output stage.
 * @throws Nothing for default construction; retained values may allocate.
 * @note This value is candidate evidence only. It cannot select a published
 * artifact identity, commit transaction, path, quota owner, or Job outcome.
 */
struct WorkerOutputDataReference final {
  /** @brief Exact manager-assigned output-stage reference echoed by worker. */
  std::string reference_id;
  /** @brief Exact immutable JobSpec output slot echoed by worker. */
  OutputSlotId output_slot_id;
  /** @brief Tight image descriptor for the staged active rows. */
  ArtifactImageDescriptor descriptor;
  /** @brief SHA-256 of the exact staged tight payload. */
  ArtifactContentDigest content_digest;
};

/**
 * @brief Typed fail-closed rejection of one data-plane occurrence or join.
 * @throws std::bad_alloc when retaining the diagnostic exhausts memory.
 * @note WorkerManager maps this trusted local validation failure to its
 * existing worker protocol failure domain after exact process cleanup.
 */
class WorkerArtifactDataPlaneError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one data-plane validation failure.
   * @param message Stable human-readable rejection diagnostic.
   * @throws std::bad_alloc when retaining the diagnostic exhausts memory.
   */
  explicit WorkerArtifactDataPlaneError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Validates metadata against one exact immutable worker assignment.
 * @param identity Complete current attempt/worker/lease identity.
 * @param spec Exact immutable JobSpec joined by `identity.job_spec_digest`.
 * @param data_plane Candidate checkpoint/output data-plane metadata.
 * @return Nothing after every identity, direction, slot, resource, receipt,
 * descriptor, digest, and deterministic reference join is validated.
 * @throws std::invalid_argument when manager-originated metadata is invalid.
 * @throws std::overflow_error when reference hashing cannot represent input.
 * @throws std::bad_alloc when canonical reference construction exhausts memory.
 * @note This validates metadata only and performs no descriptor or filesystem
 * operation. The same function runs before encoding and after decoding.
 */
void validate_worker_data_plane_assignment(
    const AttemptIdentity& identity, const JobSpec& spec,
    const WorkerDataPlaneAssignment& data_plane);

/**
 * @brief Owns manager-created anonymous files for one exact worker attempt.
 *
 * Construction writes an authorized checkpoint into one private occurrence,
 * creates a separate empty output occurrence, opens direction-scoped worker
 * descriptors plus a manager read descriptor, and unlinks both names before
 * returning. The object is move-only and closes every retained descriptor
 * exactly once.
 *
 * @throws Construction failures are reported by `create()`.
 * @note This is a local source-private adapter, not a standalone artifact
 * service, remote transport, authentication mechanism, or durable store.
 */
class WorkerArtifactDataPlane final {
 public:
  /** @brief Creates one empty descriptor owner for later move assignment. */
  WorkerArtifactDataPlane() noexcept = default;

  /**
   * @brief Creates and prepopulates one exact anonymous data-plane pair.
   * @param assignment Valid manager-owned current assignment and optional
   * authorized checkpoint record.
   * @return Move-only descriptor owner and exact bounded control metadata.
   * @throws std::invalid_argument for an incomplete assignment, checkpoint
   * mismatch, invalid descriptor/digest, or resource-bound inconsistency.
   * @throws std::system_error for temporary-file, permission, descriptor,
   * unlink, truncation, or write failure.
   * @throws std::overflow_error when byte or digest arithmetic overflows.
   * @throws std::bad_alloc when metadata/path construction exhausts memory.
   * @note All potentially throwing setup completes before `fork`. Temporary
   * names use mode 0600 and are unlinked before this method returns, so no
   * path is delegated to the worker or retained for recovery.
   */
  static WorkerArtifactDataPlane create(const JobAssignment& assignment);

  /** @brief Closes every descriptor still owned by this adapter. */
  ~WorkerArtifactDataPlane() noexcept;

  /** @brief Prevents duplicate descriptor ownership. */
  WorkerArtifactDataPlane(const WorkerArtifactDataPlane& other) = delete;
  /** @brief Prevents duplicate descriptor assignment. */
  WorkerArtifactDataPlane& operator=(const WorkerArtifactDataPlane& other) =
      delete;

  /**
   * @brief Transfers all exact descriptor and metadata ownership.
   * @param other Source adapter cleared by the move.
   * @throws Nothing.
   */
  WorkerArtifactDataPlane(WorkerArtifactDataPlane&& other) noexcept;

  /**
   * @brief Replaces this adapter with one transferred owner.
   * @param other Source adapter cleared by the move.
   * @return This adapter.
   * @throws Nothing.
   */
  WorkerArtifactDataPlane& operator=(WorkerArtifactDataPlane&& other) noexcept;

  /**
   * @brief Returns immutable metadata carried by the Assignment control frame.
   * @return Borrowed exact checkpoint/output references and bounds.
   * @throws Nothing.
   */
  const WorkerDataPlaneAssignment& assignment_metadata() const noexcept {
    return assignment_metadata_;
  }

  /**
   * @brief Returns the pre-fork read-only checkpoint descriptor.
   * @return Nonnegative descriptor until parent-side delegation cleanup.
   * @throws Nothing.
   */
  int worker_checkpoint_descriptor() const noexcept {
    return worker_checkpoint_descriptor_;
  }

  /**
   * @brief Returns the pre-fork write-only output-stage descriptor.
   * @return Nonnegative descriptor until parent-side delegation cleanup.
   * @throws Nothing.
   */
  int worker_output_descriptor() const noexcept {
    return worker_output_descriptor_;
  }

  /**
   * @brief Closes the two child-facing originals in the manager after fork.
   * @return Nothing after ownership is cleared and each descriptor receives
   * at most one close attempt.
   * @throws Nothing; close results, including `EINTR`, are ignored without
   * numeric-descriptor retry.
   * @note The execed child owns duplicated fixed descriptors independently.
   */
  void close_worker_descriptors() noexcept;

  /**
   * @brief Revalidates and materializes one reaped worker's staged candidate.
   * @param report Metadata-only attempt report decoded from the control frame.
   * @param output Optional candidate reference decoded with that report.
   * @return Complete report with an independent CPU image only for a valid
   * settled success; other valid report shapes remain image-free.
   * @throws WorkerArtifactDataPlaneError for missing/extra/mismatched output,
   * non-regular storage, size/descriptor/resource/digest mismatch, or
   * truncated bytes.
   * @throws std::system_error for manager-side `fstat`/`pread` failure.
   * @throws std::bad_alloc when bounded payload/image allocation fails.
   * @throws std::overflow_error when shape or hash arithmetic overflows.
   * @note WorkerManager calls this only after exact clean process reaping, so
   * the write descriptor is closed and the anonymous stage is immutable. This
   * method does not commit an artifact or mutate Job/quota truth.
   */
  JobAttemptReport materialize_report(
      JobAttemptReport report,
      const std::optional<WorkerOutputDataReference>& output) const;

 private:
  /**
   * @brief Retains already-created exact descriptor and metadata ownership.
   * @param checkpoint_descriptor Worker read-only checkpoint descriptor.
   * @param output_descriptor Worker write-only output-stage descriptor.
   * @param output_reader_descriptor Manager read-only stage descriptor.
   * @param metadata Exact data-plane Assignment metadata.
   * @throws Nothing after argument construction.
   */
  WorkerArtifactDataPlane(int checkpoint_descriptor, int output_descriptor,
                          int output_reader_descriptor,
                          WorkerDataPlaneAssignment metadata) noexcept;

  /** @brief Worker-facing read-only anonymous checkpoint occurrence. */
  int worker_checkpoint_descriptor_ = -1;
  /** @brief Worker-facing write-only anonymous output occurrence. */
  int worker_output_descriptor_ = -1;
  /** @brief Manager-facing read-only view of the same output occurrence. */
  int manager_output_descriptor_ = -1;
  /** @brief Exact references and bounds carried over control metadata. */
  WorkerDataPlaneAssignment assignment_metadata_;
};

/**
 * @brief Loads and validates the checkpoint through the worker's read lane.
 * @param checkpoint_descriptor Exact inherited read-only data-plane fd.
 * @param assignment Metadata-only current assignment; `checkpoint` must still
 * be null when this function is called.
 * @param data_plane Exact decoded checkpoint/output references and bounds.
 * @return Null when JobSpec declares no checkpoint, otherwise an independently
 * owned, digest-consistent immutable `ArtifactRecord`.
 * @throws WorkerArtifactDataPlaneError for descriptor access mode, file type,
 * size, identity, receipt, descriptor, digest, or metadata mismatch.
 * @throws std::system_error for `fcntl`, `fstat`, or `pread` failure.
 * @throws std::bad_alloc when bounded checkpoint allocation exhausts memory.
 * @throws std::overflow_error when digest arithmetic overflows.
 * @note The function reads only the manager-created descriptor and exposes no
 * path or store authority. Call before graph resolution or Host construction.
 */
std::shared_ptr<const ArtifactRecord> materialize_worker_checkpoint(
    int checkpoint_descriptor, const JobAssignment& assignment,
    const WorkerDataPlaneAssignment& data_plane);

/**
 * @brief Writes one worker candidate to its private stage and removes bytes
 * from the control report.
 * @param output_descriptor Exact inherited write-only data-plane fd.
 * @param spec Immutable current JobSpec and resource envelope.
 * @param output_stage Exact manager-assigned output reference and maximum.
 * @param report Non-null complete worker report; an image is consumed on
 * success and never remains in the returned control metadata.
 * @return Candidate descriptor/digest/reference on staged success, otherwise
 * empty for an image-free report or resource-bound fallback.
 * @throws std::invalid_argument for null/malformed report, identity/spec/stage
 * mismatch, invalid image, or wrong descriptor access mode.
 * @throws std::system_error for `fcntl`, `fstat`, `ftruncate`, or `pwrite`
 * failure.
 * @throws std::overflow_error when image or digest arithmetic overflows.
 * @throws std::bad_alloc when fallback/reference construction exhausts memory.
 * @note An otherwise valid settled success above the accepted output/staging/
 * retention maximum becomes a bounded settled `Failed/Compute` report with no
 * image. The function publishes no artifact and returns no stable artifact or
 * commit identity.
 */
std::optional<WorkerOutputDataReference> stage_worker_output(
    int output_descriptor, const JobSpec& spec,
    const WorkerOutputStageReference& output_stage, JobAttemptReport* report);

}  // namespace ps::server
