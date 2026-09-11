#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "photospider/data/value.hpp"
#include "photospider/execution/cancellation.hpp"

namespace ps {
/** @brief Per-call sample/copy bound and cooperative cancellation.
 * @note Failure publishes no snapshot or identity. A read may have partially
 * filled caller bytes when cancelled; only success validates those bytes.
 */
struct SnapshotAccessOptions final {
  std::uint64_t maximum_samples = 268435456;
  CancellationToken cancellation;
};
/** @brief Independent bounds for immutable rank-general input blocks. */
struct InputSnapshotStoreConfig final {
  std::uint64_t maximum_bytes = 256U * 1024U * 1024U;
  /** @brief Per-axis block extent; typed image C is kept complete. */
  std::uint32_t block_size = 128;
  /** @brief Maximum directory entries per version, checked before allocation.
   */
  std::uint64_t maximum_blocks = 65536;
};
/**
 * @brief Immutable rank-1..8 Value of any built-in dtype in shared blocks.
 * @note Generic IEEE/integer bits and semantic facets are retained exactly.
 * Typed image-v2 validation always includes all channels of each pixel.
 * Copies/reads are concurrent-safe. Captured storage outlives its store.
 * Default snapshots are invalid; metadata access throws logic_error for them.
 */
class PHOTOSPIDER_API InputSnapshot final {
 public:
  InputSnapshot() noexcept = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  const ValueDescriptor& descriptor() const;
  const std::vector<ValueFacet>& facets() const;
  /**
   * @brief Copies exact nonempty coverage into caller-owned packed bytes.
   * @param region Contained logical region, with all image channels.
   * @param destination Writable byte_size-byte range, borrowed until return.
   * @param byte_size Must equal the exact packed regional size.
   * @param options Sample bound and cooperative cancellation for this read.
   * @return Success or
   * InvalidArgument/TypeMismatch/ResourceExhausted/Cancelled. Cancellation may
   * leave partial caller bytes; no snapshot is modified.
   * @throws std::bad_alloc For metadata/diagnostic storage.
   */
  Status read(const Region& region, std::uint8_t* destination,
              std::uint64_t byte_size,
              const SnapshotAccessOptions& options = {}) const;
  /** @brief SHA-256 over canonical metadata and exact requested sample bits.
   * @param region Nonempty contained coverage with complete image channels.
   * @param options Sample bound and cancellation checked during hashing.
   * @return Identity or InvalidArgument/ResourceExhausted/Cancelled; no partial
   * digest is returned.
   * @note Identity domain v2 includes dtype, shape, coordinates, facets and
   * exact sample bits; allocation/layout/block geometry are not identity.
   */
  Result<std::string> content_identity(
      const Region& region, const SnapshotAccessOptions& options = {}) const;

 private:
  friend class InputSnapshotStore;
  struct Impl;
  explicit InputSnapshot(std::shared_ptr<const Impl> impl);
  std::shared_ptr<const Impl> impl_;
};
/**
 * @brief Owns a strict aggregate byte budget across all snapshot versions.
 * @note Concurrent imports/patches are supported. No mutation of published
 * snapshots occurs. Metadata and caller-supplied Values are outside this limit.
 */
class PHOTOSPIDER_API InputSnapshotStore final {
 public:
  /** @throws std::invalid_argument For zero bytes or block size
   * outside 1..4096, or a zero maximum_blocks.
   * @throws std::bad_alloc For budget metadata.
   */
  explicit InputSnapshotStore(InputSnapshotStoreConfig config = {});
  ~InputSnapshotStore();
  /** @brief Imports a complete Value into owned blocks, validating typed
   * samples.
   * @param value Complete immutable Value; any valid origin/stride layout.
   * @param options Sample bound and cancellation for validation and copying.
   * @return Immutable snapshot or typed validation/budget/cancellation error.
   * @throws std::bad_alloc For metadata allocation; no partial publication.
   */
  Result<InputSnapshot> import_value(
      const Value& value, const SnapshotAccessOptions& options = {}) const;
  /**
   * @brief Creates a new version by exact-region replacement in input order.
   * @param base Valid snapshot from this store.
   * @param replacement Matching descriptor/facets and nonempty Region with all
   * channels.
   * @param options Bounds validation and affected-block copy samples; observes
   * cancellation during scans and between bounded byte-copy chunks.
   * @return New snapshot or typed failure, leaving base unchanged.
   * @throws std::bad_alloc For metadata allocation.
   * @note Only intersecting blocks are copied; all old references remain valid.
   */
  Result<InputSnapshot> patch(const InputSnapshot& base,
                              const Value& replacement,
                              const SnapshotAccessOptions& options = {}) const;
  /** @brief Actual allocated block bytes including all retained old versions.
   */
  std::uint64_t live_bytes() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace ps
