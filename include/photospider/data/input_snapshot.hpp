#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "photospider/data/value.hpp"

namespace ps {
/** @brief Independent bounds for immutable image/mask input blocks. */
struct InputSnapshotStoreConfig final {
  std::uint64_t maximum_bytes = 256U * 1024U * 1024U;
  std::uint32_t block_size = 128;
};
/**
 * @brief Immutable complete Float32 image/mask represented by shared blocks.
 * @note Copies/reads are concurrent-safe. Captured storage outlives its store.
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
   * @return Success or InvalidArgument/TypeMismatch; no partial snapshot.
   * @throws std::bad_alloc For metadata/diagnostic storage.
   */
  Status read(const Region& region, std::uint8_t* destination,
              std::uint64_t byte_size) const;
  /** @brief SHA-256 over canonical metadata and exact requested sample bits.
   * @return Identity or InvalidArgument for invalid coverage/snapshot.
   * @note Reads only the requested region; allocation/layout is not identity.
   */
  Result<std::string> content_identity(const Region& region) const;

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
   * outside 1..4096.
   * @throws std::bad_alloc For budget metadata.
   */
  explicit InputSnapshotStore(InputSnapshotStoreConfig config = {});
  ~InputSnapshotStore();
  /** @brief Imports complete valid Float32 image/mask, copying into owned
   * blocks.
   * @return Immutable snapshot or typed validation/budget error.
   * @throws std::bad_alloc For metadata allocation; no partial publication.
   */
  Result<InputSnapshot> import_value(const Value& value) const;
  /**
   * @brief Creates a new version by exact-region replacement in input order.
   * @param base Valid snapshot from this store.
   * @param replacement Matching descriptor/profile and nonempty valid Region.
   * @return New snapshot or typed failure, leaving base unchanged.
   * @throws std::bad_alloc For metadata allocation.
   * @note Only intersecting blocks are copied; all old references remain valid.
   */
  Result<InputSnapshot> patch(const InputSnapshot& base,
                              const Value& replacement) const;
  /** @brief Actual allocated block bytes including all retained old versions.
   */
  std::uint64_t live_bytes() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace ps
