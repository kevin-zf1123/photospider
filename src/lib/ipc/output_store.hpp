#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "ipc/compute_request_registry.hpp"
#include "photospider/core/image_buffer.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Immutable metadata for one private tight-row CPU image artifact.
 *
 * @throws std::bad_alloc when copied string storage cannot be allocated.
 * @note The value contains no pixel bytes, backend cache path, descriptor, or
 *       image-library object. Filesystem identity is captured after atomic
 *       publication and revalidated before every delivery.
 */
struct OutputArtifactMetadata {
  /** @brief Stable 32-lowercase-hex artifact identity. */
  std::string output_id;

  /** @brief Absolute path below the socket-specific private instance. */
  std::string path;

  /** @brief Image width in pixels. */
  int width = 0;

  /** @brief Image height in pixels. */
  int height = 0;

  /** @brief Number of tightly interleaved channels per pixel. */
  int channels = 0;

  /** @brief Validated channel scalar type. */
  DataType data_type = DataType::FLOAT32;

  /** @brief Materialized memory domain; always CPU for version one. */
  Device device = Device::CPU;

  /** @brief Exact tight row width in bytes. */
  std::size_t row_step = 0;

  /** @brief Exact regular-file byte size. */
  std::size_t byte_size = 0;

  /** @brief Filesystem device captured from the published regular file. */
  std::uint64_t filesystem_device = 0;

  /** @brief Filesystem inode captured from the published regular file. */
  std::uint64_t inode = 0;
};

/**
 * @brief One validated metadata delivery protected by a refreshed lease.
 *
 * @throws std::bad_alloc when copied string storage cannot be allocated.
 * @note Repeated deliveries for one artifact retain the same delivery id and
 *       atomically refresh its single active lease before this value returns.
 */
struct OutputArtifactDelivery {
  /** @brief Revalidated immutable artifact metadata. */
  OutputArtifactMetadata metadata;

  /** @brief Stable 32-lowercase-hex identity of the artifact's sole lease. */
  std::string delivery_id;
};

/**
 * @brief Injectable artifact quota and delivery-retention policy.
 *
 * @throws Nothing.
 * @note Production retains at most 64 artifacts, one GiB total, 512 MiB per
 *       artifact, and one active 60-second lease per retained artifact.
 */
struct OutputStoreLimits {
  /** @brief Maximum records retained by job ownership or an active lease. */
  std::size_t artifacts = 64;

  /** @brief Maximum sum of retained artifact bytes. */
  std::size_t total_bytes = 1024ULL * 1024ULL * 1024ULL;

  /** @brief Maximum byte size admitted for one artifact. */
  std::size_t artifact_bytes = 512ULL * 1024ULL * 1024ULL;

  /** @brief Defensive lifetime of store-side job ownership. */
  std::chrono::steady_clock::duration job_ttl = std::chrono::minutes(15);

  /** @brief Monotonic lifetime refreshed by every successful delivery. */
  std::chrono::steady_clock::duration delivery_ttl = std::chrono::seconds(60);
};

/**
 * @brief Private socket-specific secure store for compute image artifacts.
 *
 * Startup validates or creates a same-owner exact-mode `0700` base, removes
 * only recognized safe stale artifacts through directory descriptors, and
 * opens one `instance-<server_instance_id>` child without following symlinks.
 * Publication validates one CPU image, reserves quota transactionally, writes
 * tight rows to a private `0600` file, atomically renames it, revalidates its
 * identity, and only then publishes one job-owned record. Delivery validates
 * live ancestry and file identity before refreshing the stable lease.
 *
 * @throws std::bad_alloc when constructor callback or policy storage cannot be
 *         allocated.
 * @throws std::invalid_argument for zero quotas or a nonpositive TTL.
 * @note The caller must hold the socket lifecycle lock throughout `start()`.
 *       The injected clock and id generator must be thread-safe. The store
 *       must outlive every `ComputeOutputOwnership` returned by `publish()`.
 */
class OutputStore {
 public:
  /** @brief Monotonic timestamp used for lease expiry. */
  using TimePoint = std::chrono::steady_clock::time_point;

  /** @brief Injectable non-throwing monotonic clock. */
  using Clock = std::function<TimePoint()>;

  /** @brief Injectable source of valid opaque-id candidates. */
  using IdGenerator = std::function<std::string()>;

  /**
   * @brief Creates a stopped store with injected quotas and time sources.
   *
   * @param limits Artifact count/byte limits and delivery TTL.
   * @param clock Monotonic clock; empty selects `steady_clock::now`.
   * @param id_generator Candidate source; empty selects OS entropy.
   * @throws std::bad_alloc if implementation or callback storage fails.
   * @throws std::invalid_argument for an invalid quota or TTL policy.
   */
  explicit OutputStore(OutputStoreLimits limits = {}, Clock clock = {},
                       IdGenerator id_generator = {});

  /**
   * @brief Stops publication/leases and identity-cleans all owned artifacts.
   * @throws Nothing; active leases are awaited through release or TTL.
   */
  ~OutputStore() noexcept;

  /** @brief Prevents copying descriptor, record, and quota ownership. */
  OutputStore(const OutputStore&) = delete;

  /**
   * @brief Prevents replacing descriptor ownership by copy assignment.
   * @return No value because copying is unavailable.
   */
  OutputStore& operator=(const OutputStore&) = delete;

  /**
   * @brief Starts one socket-specific instance and cleans safe stale children.
   *
   * @param socket_path Absolute bound Unix-socket path.
   * @param server_instance_id Valid daemon instance opaque id.
   * @param lifecycle_lock_fd Open socket lifecycle lock held by the caller.
   * @return Success, or daemon `internal_error` without opening the store.
   * @throws std::bad_alloc if path or diagnostic storage cannot be allocated.
   * @note Call only while holding the matching socket lifecycle lock and after
   *       proving no other live daemon owns that socket.
   */
  OperationStatus start(const std::string& socket_path,
                        const std::string& server_instance_id,
                        int lifecycle_lock_fd);

  /**
   * @brief Prevents all new or refreshed delivery leases during shutdown.
   * @throws Nothing.
   * @note Already accepted compute publication remains enabled until
   *       `shutdown()` because the joined compute worker drains first.
   */
  void stop_leases() noexcept;

  /**
   * @brief Materializes one valid image and publishes move-only job ownership.
   *
   * @param compute_id Stable identity of the accepted image job.
   * @param image Exact result of the single matching Host image compute.
   * @return Canonical success without ownership for an empty image, success
   *         with ownership for a nonempty artifact, nested daemon
   *         `artifact_limit_exceeded` for quota denial, or nested daemon
   *         `internal_error` for validation/publication failure.
   * @throws std::bad_alloc when status, record, callback, or metadata storage
   *         cannot allocate; the accepted-job registry converts it through its
   *         preallocated terminal fallback. Other validation and filesystem
   *         failures are represented by the returned nested status.
   * @note Quota and record publication are transactional. No partial file,
   *       record, or quota reservation remains after any failed return.
   */
  ComputeOutputPublication publish(const ComputeRequestId& compute_id,
                                   ImageBuffer image);

  /**
   * @brief Revalidates one job-owned artifact and refreshes its stable lease.
   *
   * @param output_id Stable private reference retained by a terminal job.
   * @return Delivery metadata; top-level daemon `artifact_not_found` when the
   *         record/file/ancestry is absent or identity-mismatched; or daemon
   *         `internal_error` when lifecycle or clock state rejects delivery.
   * @throws std::bad_alloc if result or diagnostic storage cannot allocate.
   * @note Metadata copying, validation, and expiry update occur under one store
   *       critical section. An unexpected clock failure maps to
   *       `internal_error` and does not activate or refresh the lease.
   */
  IpcResult<OutputArtifactDelivery> acquire_delivery(
      const std::string& output_id);

  /**
   * @brief Removes job ownership and optionally a matching active lease.
   *
   * @param output_id Stable private output reference.
   * @param delivery_id Optional stable lease identity released atomically.
   * @return True when the referenced retained output existed; false after it
   *         had already disappeared.
   * @throws Nothing; filesystem and clock failures are contained.
   * @note The file remains quota-accounted while any active lease survives.
   */
  bool release_job(
      const std::string& output_id,
      const std::optional<std::string>& delivery_id = std::nullopt) noexcept;

  /**
   * @brief Releases a matching active lease after its job may have disappeared.
   *
   * @param compute_id Original image-job identity bound at publication.
   * @param delivery_id Stable lease identity returned by delivery.
   * @return True only when an active matching lease was released.
   * @throws Nothing; filesystem failures are contained.
   */
  bool release_orphaned_delivery(const ComputeRequestId& compute_id,
                                 const std::string& delivery_id) noexcept;

  /**
   * @brief Expires store-side job owners and leases at their deadlines.
   * @return Number of active leases removed during this call.
   * @throws Nothing; clock and filesystem failures are contained.
   * @note An artifact with neither owner nor lease is identity-checked and
   *       removed. The return count includes delivery leases, not job owners.
   */
  std::size_t cleanup_expired() noexcept;

  /**
   * @brief Stops publication, drops job ownership, and awaits active leases.
   * @throws Nothing; all cleanup failures are contained.
   * @note Concurrent callers wait for the same shutdown. The final owner
   *       removes only its empty identity-matching instance, closes directory
   *       descriptors, and leaves the protected socket-specific base reusable.
   */
  void shutdown() noexcept;

  /**
   * @brief Returns the number of quota-accounted artifact records.
   * @return Current retained record count.
   * @throws Nothing.
   */
  std::size_t artifact_count() const noexcept;

  /**
   * @brief Returns total quota-accounted tight-row bytes.
   * @return Current retained byte count.
   * @throws Nothing.
   */
  std::size_t retained_bytes() const noexcept;

 private:
  /** @brief Opaque POSIX descriptor, record, and synchronization state. */
  class Impl;

  /** @brief Sole implementation owner. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::ipc::internal
