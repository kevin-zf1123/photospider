#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/core/resources.hpp"
#include "photospider/core/status.hpp"

namespace ps {
namespace data_internal {
struct AllocationScope;
}
/** @brief System page size in bytes, used as the minimum alignment of Host tile
 * allocation bases. Thread-safe and independent of the Plan's saved alignment.
 */
PHOTOSPIDER_API std::size_t host_page_alignment() noexcept;

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
class PHOTOSPIDER_API CpuStorage final
    : public std::enable_shared_from_this<CpuStorage> {
 public:
  /** @brief Returns borrowed immutable bytes, valid while this owner lives. */
  ByteView bytes() const noexcept;
  /** @brief Returns allocated payload capacity, excluding owner metadata. */
  std::uint64_t capacity() const noexcept { return capacity_; }
  /** @brief Returns whether a reservation is attached to this allocation. */
  bool accounted() const noexcept { return lease_ != nullptr; }
  /** @brief Destroys bytes before releasing their reservation; never throws. */
  ~CpuStorage() noexcept;
  /** @brief Capability checks for a native allocation. The native object
   * remains borrowed until this storage owner is released. */
  bool native_domain(const std::shared_ptr<const void>& domain) const noexcept {
    return domain && native_domain_ == domain;
  }
  void* native_handle() const noexcept { return native_owner_.get(); }
  bool writable_native() const noexcept { return native_writable_; }

 private:
  friend class BufferAllocator;
  friend class MutableBuffer;
  friend class TensorSpan;
  friend class ResultRef;
  std::shared_ptr<const void> domain_;
  std::shared_ptr<const data_internal::AllocationScope> allocation_scope_;
  CpuStorage() = default;
  // Declaration order makes bytes retire before their accounting lease.
  std::shared_ptr<void> lease_;
  std::shared_ptr<void> requested_lease_;
  // Native resources retire before their reservation, including after teardown.
  std::shared_ptr<void> native_owner_;
  std::shared_ptr<const void> native_domain_;
  std::uint8_t* native_bytes_ = nullptr;
  std::uint64_t byte_size_ = 0;
  bool native_writable_ = false;
  std::shared_ptr<const std::vector<std::uint8_t>> adopted_;
  struct AlignedDelete {
    static_assert(alignof(std::max_align_t) % 2 == 0);
    // Even: aligned-new alignment. Odd: page-rounded mapping length plus one.
    // Both lengths and normalized alignments are even; keep the owner layout.
    std::size_t alignment_or_mapping = alignof(std::max_align_t);
    void operator()(std::uint8_t* pointer) const noexcept;
  };
  std::unique_ptr<std::uint8_t, AlignedDelete> allocated_{nullptr,
                                                          AlignedDelete{}};
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
  /** @brief Adopt host-visible native bytes and an owning release object.
   * Capacity must cover the logical size. Destroying the last view releases
   * the native owner before its accounting lease. */
  static Result<MutableBuffer> adopt_shared(
      std::uint8_t* bytes, std::uint64_t size, std::uint64_t capacity,
      std::shared_ptr<void> native, std::shared_ptr<const void> native_domain,
      std::shared_ptr<const void> accounting_domain,
      std::shared_ptr<void> lease);
  /** @brief Retain backing while constructing a private result envelope.
   * Publication must wait for the mutable producer to finish. */
  std::shared_ptr<const CpuStorage> backing() const noexcept {
    return storage_;
  }

 private:
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
  /** @brief Tests whether both allocators share a nonempty accounting domain.
   * Limited allocators and native/resource descendants of one Root can compare
   * equal. The test does not compare quotas or provenance scopes, retain either
   * allocator, or grant access to payload owned by that domain.
   */
  bool same_owner(const BufferAllocator& other) const noexcept;
  /** @brief Allocates exactly size zero-initialized bytes after reservation.
   * Ordinary Host backing starts on a system page boundary.
   * Local size, quota and allocation exhaustion use ResourceExhausted with
   * CapacityLimit. External reservation/native failures preserve their Status.
   * Host allocation exceptions become ResourceExhausted; other host exceptions
   * become OperationFailed. Both notify all scoped failure observers before
   * returning, so a callback cannot catch an exception and recover publication.
   */
  Result<MutableBuffer> allocate(std::uint64_t size) const;
  /** @brief Allocate host storage at a positive power-of-two alignment.
   * Host backing also satisfies host_page_alignment(). Alignment is an address
   * contract, not additional logical payload. Native allocator conversions
   * reject this overload; their device preparation owns alignment negotiation.
   * No allocation occurs for invalid alignment.
   */
  Result<MutableBuffer> allocate_aligned(std::uint64_t size,
                                         std::uint64_t alignment) const;
  /** @brief Allocation-failure notification with the original reason and
   * detail. Status is borrowed for the call; observer exceptions are fenced. */
  using FailureObserver = std::function<void(const Status&)>;
  /** @brief Creates an aggregate live-capacity sublimit retaining this domain.
   * @note Copies of the returned allocator share its quota. Parent reservation
   * and native allocation policies remain active; last-owner release returns
   * capacity. Zero rejects every positive allocation. Metadata exceptions
   * notify parent/child failure observers before propagating (including
   * bad_alloc).
   * @param maximum_bytes Maximum aggregate live capacity under this scope.
   * @param failure Optional failure observer; all parent/child observers run
   * independently even if another observer throws.
   * @return A quota-sharing allocator that retains the original services.
   */
  BufferAllocator limited(std::uint64_t maximum_bytes,
                          FailureObserver failure = {}) const;
  /** @brief Limits aggregate live requested bytes, retaining actual-capacity
   * root accounting and any enclosing capacity sublimits.
   * @note A native allocation charges its requested size here and its actual
   * backing capacity to the parent. Copies and native allocator conversions
   * share this quota. The last storage owner releases it. Failure notifications
   * and allocation provenance follow the same rules as limited().
   */
  BufferAllocator limited_requested(std::uint64_t maximum_bytes,
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
  friend class ResourceBudget;
  Result<MutableBuffer> allocate_aligned_impl(std::uint64_t size,
                                              std::uint64_t alignment,
                                              bool dense_host) const;
  Result<MutableBuffer> allocate_impl(std::uint64_t size,
                                      std::uint64_t alignment,
                                      bool dense_host = false) const;
  BufferAllocator limited_impl(std::uint64_t maximum_bytes,
                               FailureObserver failure, bool requested) const;
  // Successful allocation increments: payload bytes, then host-owner metadata
  // bytes. Either increment may be zero; this never reserves new capacity.
 public:
  using AllocationCommit = std::function<void(const std::shared_ptr<void>&,
                                              std::uint64_t, std::uint64_t)>;
  using NativeAllocate = std::function<Result<MutableBuffer>(
      std::uint64_t, const Reserve&, std::shared_ptr<const void>,
      const AllocationCommit&)>;
  /** @brief Bind a native shared-storage factory to this accounting domain. */
  BufferAllocator native_adapter(NativeAllocate allocate) const;
  /** @brief Use a pre-admitted block's per-allocation leases and accounting
   * hook. */
  void set_reservations(Reserve host, Reserve shared, AllocationCommit commit);
  const Reserve& reservation() const { return reserve_; }
  void set_reservation(Reserve reserve) { reserve_ = std::move(reserve); }
  const AllocationCommit& allocation_commit() const {
    return allocation_committed_;
  }
  Result<MutableBuffer> allocate_reserved(std::uint64_t bytes,
                                          std::uint64_t alignment,
                                          bool dense) const {
    return allocate_aligned_impl(bytes, alignment, dense);
  }

 private:
  NativeAllocate native_allocate_;
  AllocationCommit allocation_committed_;
  Reserve reserve_;
  // Alternate root accounting for a host-visible native allocation. Scoped
  // allocators wrap both policies with the same live-byte counter.
  Reserve native_shared_reserve_;
  Reserve requested_reserve_;
  std::shared_ptr<const void> domain_;
  std::shared_ptr<const data_internal::AllocationScope> allocation_scope_;
  // Explicit root for quota/provenance owners, preserved by native conversion.
  std::optional<ResourceBudget> metadata_budget_;
  FailureObserver failure_;
};
}  // namespace ps
