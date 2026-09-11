#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "photospider/core/status.hpp"

namespace ps {
namespace gpu_internal {
class Device;
}

/** @brief Borrowed immutable bytes; lifetime is bounded by the storage owner.
 */
class ByteView final {
 public:
  /** @brief Creates a borrowed range; pointer must address size valid bytes. */
  ByteView(const std::uint8_t* data, std::size_t size) noexcept
      : data_(data), size_(size) {}
  /** @brief Returns the borrowed address; never writable. */
  const std::uint8_t* data() const noexcept { return data_; }
  /** @brief Returns byte count. */
  std::size_t size() const noexcept { return size_; }
  /** @brief Returns whether the view contains no bytes. */
  bool empty() const noexcept { return size_ == 0; }
  /** @brief Returns the first byte iterator. */
  const std::uint8_t* begin() const noexcept { return data_; }
  /** @brief Returns one-past-end, without arithmetic on an empty null view. */
  const std::uint8_t* end() const noexcept {
    return size_ == 0 ? data_ : data_ + size_;
  }
  /** @brief Reads one byte; caller must ensure index < size(). */
  std::uint8_t operator[](std::size_t index) const noexcept {
    return data_[index];
  }
  /** @brief Compares byte contents without interpreting layout. */
  bool operator==(ByteView other) const noexcept {
    return size_ == other.size_ && std::equal(begin(), end(), other.begin());
  }
  /** @brief Reports unequal byte contents. */
  bool operator!=(ByteView other) const noexcept { return !(*this == other); }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
};

/**
 * @brief Immutable CPU allocation with independent reservation lifetime.
 * @note The owner can outlive its allocator or ExecutionContext. Capacity is
 * exact for host allocations; adopted caller vectors retain their capacity.
 */
class PHOTOSPIDER_API CpuStorage final {
 public:
  /** @brief Returns borrowed immutable bytes, valid while this owner lives. */
  ByteView bytes() const noexcept;
  /** @brief Returns allocated payload capacity, excluding owner metadata. */
  std::uint64_t capacity() const noexcept { return capacity_; }
  /** @brief Returns whether a reservation is attached to this allocation. */
  bool accounted() const noexcept { return lease_ != nullptr; }
  /** @brief Destroys bytes before releasing their reservation; never throws. */
  ~CpuStorage() noexcept;

 private:
  friend class gpu_internal::Device;
  friend class BufferAllocator;
  friend class MutableBuffer;
  friend class Value;
  std::shared_ptr<const void> domain_;
  std::vector<std::shared_ptr<const void>> allocation_scopes_;
  CpuStorage() = default;
  // Declaration order makes bytes retire before their accounting lease.
  std::shared_ptr<void> lease_;
  // Native resources retire before their reservation, including after teardown.
  std::shared_ptr<void> native_owner_;
  std::shared_ptr<const void> native_domain_;
  std::uint8_t* native_bytes_ = nullptr;
  std::uint64_t byte_size_ = 0;
  bool native_writable_ = false;
  std::shared_ptr<const std::vector<std::uint8_t>> adopted_;
  std::unique_ptr<std::uint8_t[]> allocated_;
  std::uint64_t capacity_ = 0;
};

/**
 * @brief Exclusive writable host allocation; move-only until publication.
 * @note Borrowed pointers must not survive freeze or destruction. A moved-from
 * buffer has zero size and no writable pointer. Concurrent mutation is
 * forbidden.
 */
class PHOTOSPIDER_API MutableBuffer final {
 public:
  MutableBuffer() noexcept = default;
  MutableBuffer(MutableBuffer&&) noexcept = default;
  MutableBuffer& operator=(MutableBuffer&&) noexcept = default;
  MutableBuffer(const MutableBuffer&) = delete;
  MutableBuffer& operator=(const MutableBuffer&) = delete;
  /** @brief Returns writable bytes, or null for a moved-from buffer. */
  std::uint8_t* data() noexcept;
  /** @brief Returns exact allocation size, or zero after move/publication. */
  std::size_t size() const noexcept;
  /** @brief Transfers sole mutable ownership into immutable shared storage. */
  std::shared_ptr<const CpuStorage> freeze() && noexcept;

 private:
  friend class gpu_internal::Device;
  friend class BufferAllocator;
  std::shared_ptr<CpuStorage> storage_;
};

/**
 * @brief Host CPU allocator with reservation-before-allocation semantics.
 * @note A reservation callback must be thread-safe and return an owned lease
 * that releases exactly its byte count on destruction. Empty reserve means
 * caller-owned allocation outside an ExecutionContext. Allocation returns
 * ResourceExhausted on overflow/failure; metadata bad_alloc may still
 * propagate.
 */
class PHOTOSPIDER_API BufferAllocator final {
 public:
  /** @brief Reservation function; failure prevents the payload allocation. */
  using Reserve = std::function<Result<std::shared_ptr<void>>(std::uint64_t)>;
  /** @brief Creates an allocator retaining the supplied reservation owner. */
  explicit BufferAllocator(Reserve reserve = {},
                           std::shared_ptr<const void> domain = {});
  /** @brief Reports whether storage belongs to this allocator's accounting
   * domain. */
  bool owns(const CpuStorage& storage) const noexcept;
  /** @brief Allocates exactly size zero-initialized bytes after reservation. */
  Result<MutableBuffer> allocate(std::uint64_t size) const;
  /** @brief Allocation-failure notification; observer exceptions are fenced. */
  using FailureObserver = std::function<void(ErrorCode)>;
  /** @brief Creates an aggregate live-capacity sublimit retaining this domain.
   * @note Copies of the returned allocator share its quota. Parent reservation
   * and native allocation policies remain active; last-owner release returns
   * capacity. Zero rejects every positive allocation. May throw bad_alloc.
   * @param maximum_bytes Maximum aggregate live capacity under this scope.
   * @param failure Optional failure observer; all parent/child observers run
   * independently even if another observer throws.
   * @return A quota-sharing allocator that retains the original services.
   */
  BufferAllocator limited(std::uint64_t maximum_bytes,
                          FailureObserver failure = {}) const;
  /** @brief Checks allocation provenance for a scoped allocator or descendant.
   * @note Unscoped allocators return false. Scope identity is process-local and
   * grants no content/cache identity. Safe after the original allocator
   * retires.
   */
  bool owns_allocation(const MutableBuffer& buffer) const noexcept;
  /** @brief Checks the same scoped provenance after immutable publication.
   * @note A shared context domain alone is insufficient; this checks the
   * allocator's exact live sublimit or one of its descendants.
   */
  bool owns_allocation(const CpuStorage& storage) const noexcept;

 private:
  friend class gpu_internal::Device;
  std::function<Result<MutableBuffer>(std::uint64_t, const Reserve&,
                                      std::shared_ptr<const void>)>
      native_allocate_;
  Reserve reserve_;
  std::shared_ptr<const void> domain_;
  std::vector<std::shared_ptr<const void>> allocation_scopes_;
  FailureObserver failure_;
};
}  // namespace ps
