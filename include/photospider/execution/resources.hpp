#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "photospider/core/status.hpp"
#include "photospider/data/storage.hpp"

namespace ps {

/** @brief Independent capacity constraints; overlapping dimensions are not
 * summed. Host includes Shared and Metadata; Device includes Shared. Referenced
 * is a separate bound on caller-owned input capacity. Disk counts encoded
 * temporary extents, including padding, not filesystem allocation blocks or OS
 * cache.
 */
enum class ResourceKind : std::uint32_t {
  Host,
  Device,
  Shared,
  Metadata,
  Referenced,
  Disk,
  Entries,
  Files,
  IoSlots,
  Queue,
  /** @brief Computation-buffer bytes; preserves maximum_live_bytes as a
     sublimit. */
  Payload,
  Count
};

/** @brief Fixed, allocation-free capacity vector, initialized to zero. */
struct PHOTOSPIDER_API ResourceCapacity final {
  std::array<std::uint64_t, static_cast<std::size_t>(ResourceKind::Count)>
      values{};
  std::uint64_t& operator[](ResourceKind kind) noexcept {
    return values[static_cast<std::size_t>(kind)];
  }
  std::uint64_t operator[](ResourceKind kind) const noexcept {
    return values[static_cast<std::size_t>(kind)];
  }
  /** @brief Host payload, with optional metadata already included in bytes. */
  static ResourceCapacity host(std::uint64_t bytes, std::uint64_t metadata = 0);
};

/** @brief Explicit managed-capacity limits, independent of optional cache.
 * The scope is requested payload and C++ object capacity, not allocator
 * headers, thread stacks, provider-private state, OS page cache, device drivers
 * or RSS. Uninstrumented allocations are outside this model and cannot inherit
 * its guarantee. Resource exhaustion is finite failure, not a completion
 * promise.
 */
struct PHOTOSPIDER_API ResourceLimits final {
  ResourceCapacity capacity;
  /** @brief Capacity unavailable to ordinary stages; reserved for cleanup. */
  ResourceCapacity cleanup;
  std::uint64_t maximum_work = UINT64_MAX;
  std::uint64_t maximum_io_bytes = UINT64_MAX;
  std::uint64_t maximum_io_requests = UINT64_MAX;
  std::uint64_t maximum_stages = UINT64_MAX;
  ResourceLimits();
};

/** @brief Cumulative quantities precharged atomically before submission. */
struct ResourceWork final {
  std::uint64_t work = 0, io_bytes = 0, io_requests = 0, stages = 0;
};

/** @brief Synchronized capacity observations; peak is measured under this
 * model. */
struct ResourceStatistics final {
  ResourceCapacity live, peak, protected_cleanup, quarantined;
  ResourceWork issued;
};

class ResourceBudget;
/** @brief Shared reservation owner; aliases charge once and may outlive
 * context. A lease owns declared capacity, not payload bytes. Keep it until all
 * associated storage has retired. Growth admits the entire increment
 * atomically. Shrinking is legal only after corresponding storage is released
 * or an unissued action is abandoned. No operation refunds already issued work
 * or I/O.
 */
class PHOTOSPIDER_API ResourceLease final {
 public:
  ResourceLease() = default;
  bool valid() const noexcept { return impl_ != nullptr; }
  ResourceCapacity capacity() const;
  Status grow(ResourceCapacity additional);
  Status shrink(ResourceCapacity released);
  /** @brief Keeps capacity charged permanently after unverified cleanup.
   * No allocation occurs; aliases all observe quarantine. This root cannot
   * reuse the capacity. Destroying the root ends observations, not OS cleanup.
   */
  void quarantine() noexcept;
  /** @brief Ends quarantine only after the provider verified physical cleanup.
   * The lease remains charged until release. Trusted provider code must not
   * call this merely because cleanup was attempted or cancellation requested.
   */
  void settle_quarantine() noexcept;

 private:
  friend class ResourceBudget;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

/** @brief Thread-safe root for capacity and cumulative work admission.
 * Admission never waits for another reservation. Metadata owner construction is
 * exception-safe; failed admission changes no dimension. Public callers and
 * trusted callbacks must declare their owned resources completely.
 */
class PHOTOSPIDER_API ResourceBudget final {
 public:
  /** @throws std::invalid_argument For inconsistent cleanup/capacity limits.
   * @throws std::bad_alloc For root bookkeeping outside the managed arena.
   */
  explicit ResourceBudget(ResourceLimits limits = {});
  /** @brief Per-lease object capacity added automatically to Host/Metadata.
   * Allocator control blocks and heap headers remain outside this model.
   */
  static std::uint64_t lease_metadata_bytes() noexcept;
  Result<ResourceLease> reserve(ResourceCapacity capacity) const;
  Status consume(ResourceWork work) const;
  ResourceStatistics statistics() const;
  /** @brief Compares capacity ownership only; never a semantic result key. */
  bool same_owner(const ResourceBudget& other) const noexcept {
    return impl_ == other.impl_;
  }
  /** @brief Allocator charging payload plus actual CpuStorage object capacity.
   * Owner/control-block allocator overhead is explicitly outside this model.
   * Returned buffers retain this root and their charges until the last owner.
   */
  BufferAllocator allocator() const;
  /** @brief Retains an external allocation under the Referenced sublimit.
   * Charges its full capacity once per root, including simultaneous callers.
   * Returns an alias with unchanged CpuStorage identity; downstream views
   * retain the reference lease. Storage allocated by this root is already
   * charged.
   */
  Result<std::shared_ptr<const CpuStorage>> reference(
      std::shared_ptr<const CpuStorage> storage) const;

 private:
  friend class ResourceLease;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace ps
