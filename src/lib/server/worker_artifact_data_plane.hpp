/**
 * @file worker_artifact_data_plane.hpp
 * @brief Declares the attempt-scoped Issue #105 worker artifact data plane.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {

/**
 * @brief Maximum bytes in one non-authorizing data-plane reference id.
 * @note References are exact metadata join keys. Direction-scoped inherited
 * stream descriptors, not reference text, grant byte-transfer capability.
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
 * @brief Closed result of one manager-side nonblocking stream operation.
 * @throws Nothing for value operations.
 * @note The caller owns all absolute deadlines, cancellation checks, process
 * termination, and exact-reap decisions between bounded operations.
 */
enum class WorkerDataPlaneIoStatus : std::uint8_t {
  /** @brief One bounded chunk was transferred; call again if appropriate. */
  Progress,
  /** @brief The nonblocking manager endpoint currently has no ready bytes. */
  WouldBlock,
  /** @brief The peer closed its exact direction after all prior bytes. */
  EndOfStream,
};

/**
 * @brief Owns two direction-reduced stream lanes for one exact worker attempt.
 *
 * Construction creates no pathname or bulk occurrence. One socket lane grants
 * only manager-to-worker checkpoint transfer; the other grants only
 * worker-to-manager candidate transfer. Child endpoints remain blocking so
 * stalled data-plane work stays inside the killable worker, while manager
 * endpoints are nonblocking and are pumped only by WorkerManager's absolute
 * lifecycle deadlines.
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
   * @brief Creates one exact pair of direction-reduced stream lanes.
   * @param assignment Valid manager-owned current assignment and optional
   * authorized checkpoint record.
   * @return Move-only descriptor owner and exact bounded control metadata.
   * @throws std::invalid_argument for an incomplete assignment, checkpoint
   * mismatch, invalid descriptor/digest, or resource-bound inconsistency.
   * @throws std::system_error for socket creation, direction reduction, or
   * descriptor-flag failure.
   * @throws std::overflow_error when byte or digest arithmetic overflows.
   * @throws std::bad_alloc when bounded metadata construction exhausts memory.
   * @note This method copies/hashes only bounded metadata and creates no
   * pathname, file, payload copy, or bulk digest. WorkerManager invokes it in
   * the registered supervision owner before `fork`.
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
   * @brief Returns the pre-fork worker checkpoint receive descriptor.
   * @return Nonnegative descriptor until parent-side delegation cleanup.
   * @throws Nothing.
   */
  int worker_checkpoint_descriptor() const noexcept {
    return worker_checkpoint_descriptor_;
  }

  /**
   * @brief Returns the pre-fork worker output send descriptor.
   * @return Nonnegative descriptor until parent-side delegation cleanup.
   * @throws Nothing.
   */
  int worker_output_descriptor() const noexcept {
    return worker_output_descriptor_;
  }

  /**
   * @brief Returns the manager's nonblocking checkpoint send descriptor.
   * @return Nonnegative descriptor until checkpoint-lane closure.
   * @throws Nothing.
   * @note WorkerManager may poll this endpoint for `POLLOUT`; callers receive
   * no checkpoint bytes, artifact identity, or publication authority.
   */
  int manager_checkpoint_descriptor() const noexcept {
    return manager_checkpoint_descriptor_;
  }

  /**
   * @brief Returns the manager's nonblocking output receive descriptor.
   * @return Nonnegative descriptor until output-lane closure.
   * @throws Nothing.
   * @note WorkerManager may poll this endpoint for `POLLIN` under its absolute
   * runtime/cancel/shutdown deadlines.
   */
  int manager_output_descriptor() const noexcept {
    return manager_output_descriptor_;
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
   * @brief Sends at most one checkpoint chunk without blocking the manager.
   * @param payload Exact trusted checkpoint bytes, or empty when absent.
   * @param offset Non-null count already sent; advanced after progress.
   * @return Progress, WouldBlock, or EndOfStream when the worker closed early.
   * @throws std::invalid_argument for null/out-of-range `offset`.
   * @throws std::system_error for a non-readiness socket failure.
   * @note The caller owns absolute deadline, cancellation, polling, and exact
   * process termination. This method never loops or waits for readiness.
   */
  WorkerDataPlaneIoStatus send_checkpoint_chunk(
      const std::vector<std::byte>& payload, std::size_t* offset);

  /**
   * @brief Closes the manager checkpoint sender after exact transfer.
   * @return Nothing after one non-retried close attempt.
   * @throws Nothing.
   * @note EOF is the lane commit marker consumed by the worker after the exact
   * receipt-sized byte range.
   */
  void close_manager_checkpoint_descriptor() noexcept;

  /**
   * @brief Receives at most one candidate chunk without blocking the manager.
   * @param payload Non-null bounded accumulation buffer.
   * @return Progress, WouldBlock, or EndOfStream after worker closure.
   * @throws std::invalid_argument for a null output.
   * @throws WorkerArtifactDataPlaneError when bytes exceed the assigned stage.
   * @throws std::system_error for a non-readiness socket failure.
   * @throws std::bad_alloc when bounded accumulation allocation fails.
   * @note The caller owns absolute deadline, cancellation, polling, and exact
   * process termination. This method performs one `recv` and never waits.
   */
  WorkerDataPlaneIoStatus receive_output_chunk(std::vector<std::byte>* payload);

  /**
   * @brief Closes the manager output receiver after EOF or revocation.
   * @return Nothing after one non-retried close attempt.
   * @throws Nothing.
   */
  void close_manager_output_descriptor() noexcept;

  /**
   * @brief Revalidates and materializes one completely received candidate.
   * @param report Metadata-only attempt report decoded from the control frame.
   * @param output Optional candidate reference decoded with that report.
   * @param payload Exact bytes drained from the attempt's output lane.
   * @param payload_digest Independently accumulated digest of those bytes.
   * @return Complete report with an independent CPU image only for a valid
   * settled success; other valid report shapes remain image-free.
   * @throws WorkerArtifactDataPlaneError for missing/extra/mismatched output,
   * size/descriptor/resource/digest mismatch, or truncated bytes.
   * @throws std::bad_alloc when bounded payload/image allocation fails.
   * @throws std::overflow_error when shape or hash arithmetic overflows.
   * @note This method performs no filesystem or descriptor I/O. WorkerManager
   * calls it after bounded stream EOF and before terminal handoff. It does not
   * commit an artifact or mutate Job/quota truth.
   */
  JobAttemptReport materialize_report(
      JobAttemptReport report,
      const std::optional<WorkerOutputDataReference>& output,
      std::vector<std::byte> payload,
      const ArtifactContentDigest& payload_digest) const;

 private:
  /**
   * @brief Retains already-created exact descriptor and metadata ownership.
   * @param checkpoint_descriptor Worker checkpoint receive descriptor.
   * @param checkpoint_sender_descriptor Manager checkpoint send descriptor.
   * @param output_descriptor Worker candidate send descriptor.
   * @param output_reader_descriptor Manager candidate receive descriptor.
   * @param metadata Exact data-plane Assignment metadata.
   * @throws Nothing after argument construction.
   */
  WorkerArtifactDataPlane(int checkpoint_descriptor,
                          int checkpoint_sender_descriptor,
                          int output_descriptor, int output_reader_descriptor,
                          WorkerDataPlaneAssignment metadata) noexcept;

  /** @brief Worker-facing checkpoint receive stream. */
  int worker_checkpoint_descriptor_ = -1;
  /** @brief Manager-facing nonblocking checkpoint send stream. */
  int manager_checkpoint_descriptor_ = -1;
  /** @brief Worker-facing candidate send stream. */
  int worker_output_descriptor_ = -1;
  /** @brief Manager-facing nonblocking candidate receive stream. */
  int manager_output_descriptor_ = -1;
  /** @brief Exact references and bounds carried over control metadata. */
  WorkerDataPlaneAssignment assignment_metadata_;
};

/**
 * @brief Loads and validates the checkpoint through the worker's receive lane.
 * @param checkpoint_descriptor Exact inherited checkpoint stream fd.
 * @param assignment Metadata-only current assignment; `checkpoint` must still
 * be null when this function is called.
 * @param data_plane Exact decoded checkpoint/output references and bounds.
 * @return Null when JobSpec declares no checkpoint, otherwise an independently
 * owned, digest-consistent immutable `ArtifactRecord`.
 * @throws WorkerArtifactDataPlaneError for stream type, size, identity,
 * receipt, descriptor, digest, or metadata mismatch.
 * @throws std::system_error for stream receive failure.
 * @throws std::bad_alloc when bounded checkpoint allocation exhausts memory.
 * @throws std::overflow_error when digest arithmetic overflows.
 * @note Blocking receive executes only in the killable worker. The function
 * exposes no path or store authority. Call before graph/Host construction.
 */
std::shared_ptr<const ArtifactRecord> materialize_worker_checkpoint(
    int checkpoint_descriptor, const JobAssignment& assignment,
    const WorkerDataPlaneAssignment& data_plane);

/**
 * @brief Sends one worker candidate through its private lane and removes bytes
 * from the control report.
 * @param output_descriptor Exact inherited candidate stream fd.
 * @param spec Immutable current JobSpec and resource envelope.
 * @param output_stage Exact manager-assigned output reference and maximum.
 * @param report Non-null complete worker report; an image is consumed on
 * success and never remains in the returned control metadata.
 * @return Candidate descriptor/digest/reference on staged success, otherwise
 * empty for an image-free report or resource-bound fallback.
 * @throws std::invalid_argument for null/malformed report, identity/spec/stage
 * mismatch, invalid image, or wrong descriptor access mode.
 * @throws std::system_error for stream send failure.
 * @throws std::overflow_error when image or digest arithmetic overflows.
 * @throws std::bad_alloc when fallback/reference construction exhausts memory.
 * @note Blocking send executes only in the killable worker. An otherwise
 * valid settled success above the accepted output/staging/
 * retention maximum becomes a bounded settled `Failed/Compute` report with no
 * image. The function publishes no artifact and returns no stable artifact or
 * commit identity.
 */
std::optional<WorkerOutputDataReference> stage_worker_output(
    int output_descriptor, const JobSpec& spec,
    const WorkerOutputStageReference& output_stage, JobAttemptReport* report);

}  // namespace ps::server
