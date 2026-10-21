#include "photospider/data/storage.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "photospider/core/checked_math.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps {
namespace data_internal {
// Immutable provenance links are shared by allocator copies and buffers. The
// quota counter is separately synchronized; parent links never change.
struct AllocationScope final {
  std::shared_ptr<const AllocationScope> parent;
  std::mutex mutex;
  std::uint64_t live = 0;
};
}  // namespace data_internal
namespace {
void notify_failure(const BufferAllocator::FailureObserver& observer,
                    const Status& status) noexcept {
  if (observer) {
    try {
      observer(status);
    } catch (...) {
    }
  }
}
}  // namespace
ByteView CpuStorage::bytes() const noexcept {
  if (native_owner_)
    return ByteView(native_bytes_, static_cast<std::size_t>(byte_size_));
  if (adopted_)
    return ByteView(adopted_->data(), adopted_->size());
  return ByteView(allocated_.get(), static_cast<std::size_t>(capacity_));
}
void CpuStorage::AlignedDelete::operator()(
    std::uint8_t* pointer) const noexcept {
  ::operator delete(pointer, std::align_val_t(alignment));
}
CpuStorage::~CpuStorage() noexcept = default;
std::uint8_t* MutableBuffer::data() noexcept {
  return storage_ ? (storage_->native_owner_ ? storage_->native_bytes_
                                             : storage_->allocated_.get())
                  : nullptr;
}
std::size_t MutableBuffer::size() const noexcept {
  return storage_ ? static_cast<std::size_t>(storage_->native_owner_
                                                 ? storage_->byte_size_
                                                 : storage_->capacity_)
                  : 0;
}
std::shared_ptr<const CpuStorage> MutableBuffer::freeze() && noexcept {
  if (storage_)
    storage_->native_writable_ = false;
  return std::move(storage_);
}
BufferAllocator::BufferAllocator(Reserve reserve,
                                 std::shared_ptr<const void> domain)
    : reserve_(std::move(reserve)), domain_(std::move(domain)) {}
bool BufferAllocator::owns(const CpuStorage& storage) const noexcept {
  return domain_ && domain_ == storage.domain_;
}
bool BufferAllocator::same_owner(const BufferAllocator& other) const noexcept {
  return domain_ && domain_ == other.domain_;
}
BufferAllocator BufferAllocator::limited(std::uint64_t maximum_bytes,
                                         FailureObserver failure) const {
  return limited_impl(maximum_bytes, std::move(failure), false);
}
BufferAllocator BufferAllocator::limited_requested(
    std::uint64_t maximum_bytes, FailureObserver failure) const {
  return limited_impl(maximum_bytes, std::move(failure), true);
}
BufferAllocator BufferAllocator::limited_impl(std::uint64_t maximum_bytes,
                                              FailureObserver failure,
                                              bool requested) const {
  try {
    using State = data_internal::AllocationScope;
    struct Lease {
      std::shared_ptr<State> state;
      std::uint64_t bytes = 0;
      std::shared_ptr<void> parent;
      ~Lease() {
        if (bytes) {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->live -= bytes;
        }
      }
    };
    auto state = metadata_budget_
                     ? std::allocate_shared<State>(
                           ResourceAllocator<State>(*metadata_budget_))
                     : std::make_shared<State>();
    state->parent = allocation_scope_;
    const auto limited_reserve = [state, maximum_bytes,
                                  budget = metadata_budget_](Reserve parent) {
      return Reserve([parent = std::move(parent), state, budget,
                      maximum_bytes](std::uint64_t bytes) {
        auto lease =
            budget
                ? std::allocate_shared<Lease>(ResourceAllocator<Lease>(*budget))
                : std::make_shared<Lease>();
        lease->state = state;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (!core_internal::can_add(bytes, state->live, maximum_bytes))
            return Result<std::shared_ptr<void>>(
                Status{ErrorCode::ResourceExhausted,
                       "allocator live sublimit exceeded",
                       FailureReason::CapacityLimit});
          state->live += bytes;
          lease->bytes = bytes;
        }
        if (parent) {
          auto reserved = parent(bytes);
          if (!reserved.ok())
            return Result<std::shared_ptr<void>>(reserved.status());
          lease->parent = reserved.take_value();
        }
        return Result<std::shared_ptr<void>>(std::move(lease));
      });
    };
    auto result = *this;
    if (requested) {
      result.requested_reserve_ = limited_reserve(requested_reserve_);
    } else {
      result.reserve_ = limited_reserve(reserve_);
      result.native_shared_reserve_ = limited_reserve(
          native_shared_reserve_ ? native_shared_reserve_ : reserve_);
      if (allocation_committed_) {
        result.allocation_committed_ = [parent = allocation_committed_](
                                           const std::shared_ptr<void>& owner,
                                           std::uint64_t bytes,
                                           std::uint64_t metadata) {
          const auto lease = std::static_pointer_cast<Lease>(owner);
          parent(lease->parent, bytes, metadata);
        };
      }
    }
    result.allocation_scope_ = state;
    result.failure_ = [parent = failure_,
                       observer = failure](const Status& status) {
      if (parent) {
        try {
          parent(status);
        } catch (...) {
        }
      }
      if (observer) {
        try {
          observer(status);
        } catch (...) {
        }
      }
    };
    return result;
  } catch (const std::bad_alloc&) {
    notify_failure(
        failure_,
        {ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit});
    notify_failure(
        failure,
        {ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit});
    throw;
  } catch (...) {
    notify_failure(
        failure_,
        {ErrorCode::OperationFailed, {}, FailureReason::HostException});
    notify_failure(
        failure,
        {ErrorCode::OperationFailed, {}, FailureReason::HostException});
    throw;
  }
}

bool BufferAllocator::owns_allocation(
    const MutableBuffer& buffer) const noexcept {
  return buffer.storage_ && owns_allocation(*buffer.storage_);
}
bool BufferAllocator::owns_allocation(
    const CpuStorage& storage) const noexcept {
  if (!allocation_scope_)
    return false;
  for (auto scope = storage.allocation_scope_; scope; scope = scope->parent)
    if (scope == allocation_scope_)
      return true;
  return false;
}
Result<MutableBuffer> BufferAllocator::allocate(std::uint64_t size) const {
  return allocate_impl(size, alignof(std::max_align_t));
}
Result<MutableBuffer> BufferAllocator::allocate_aligned(
    std::uint64_t size, std::uint64_t alignment) const {
  if (!alignment || (alignment & (alignment - 1)) || alignment > SIZE_MAX)
    return Result<MutableBuffer>(
        {ErrorCode::InvalidArgument, "invalid host allocation alignment"});
  if (native_allocate_)
    return Result<MutableBuffer>({ErrorCode::BackendUnavailable,
                                  "native alignment requires preparation"});
  return allocate_impl(
      size, std::max<std::uint64_t>(alignment, alignof(std::max_align_t)));
}
Result<MutableBuffer> BufferAllocator::allocate_impl(
    std::uint64_t size, std::uint64_t alignment) const {
  const auto reject = [&](Status status) {
    if (failure_) {
      try {
        failure_(status);
      } catch (...) {
      }
    }
    return Result<MutableBuffer>(std::move(status));
  };
  try {
    std::shared_ptr<void> requested_lease;
    if (requested_reserve_) {
      auto reserved = requested_reserve_(size);
      if (!reserved.ok())
        return reject(reserved.status());
      requested_lease = reserved.take_value();
    }
    if (native_allocate_) {
      auto result =
          native_allocate_(size, reserve_, domain_, allocation_committed_);
      if (!result.ok())
        return reject(result.status());
      result.value().storage_->allocation_scope_ = allocation_scope_;
      result.value().storage_->requested_lease_ = std::move(requested_lease);
      return result;
    }
    if (size == 0 || size > static_cast<std::uint64_t>(INT64_MAX) ||
        size > std::numeric_limits<std::size_t>::max()) {
      return reject(Status{ErrorCode::ResourceExhausted,
                           "CPU allocation size is not addressable",
                           FailureReason::CapacityLimit});
    }
    std::shared_ptr<void> storage_lease;
    if (reserve_) {
      auto lease = reserve_(size);
      if (!lease.ok())
        return reject(lease.status());
      storage_lease = lease.take_value();
    }
    MutableBuffer result;
    auto owner = std::unique_ptr<CpuStorage>(new CpuStorage());
    owner->lease_ = std::move(storage_lease);
    if (allocation_committed_)
      allocation_committed_(owner->lease_, 0, sizeof(CpuStorage));
    result.storage_ = std::shared_ptr<CpuStorage>(std::move(owner));
    result.storage_->domain_ = domain_;
    result.storage_->allocation_scope_ = allocation_scope_;
    result.storage_->requested_lease_ = std::move(requested_lease);
    result.storage_->allocated_ = {
        static_cast<std::uint8_t*>(::operator new(
            static_cast<std::size_t>(size),
            std::align_val_t(static_cast<std::size_t>(alignment)))),
        CpuStorage::AlignedDelete{static_cast<std::size_t>(alignment)}};
    std::fill_n(result.storage_->allocated_.get(),
                static_cast<std::size_t>(size), std::uint8_t{0});
    result.storage_->capacity_ = size;
    if (allocation_committed_)
      allocation_committed_(result.storage_->lease_, size, 0);
    return Result<MutableBuffer>(std::move(result));
  } catch (const std::bad_alloc&) {
    return reject(
        Status{ErrorCode::ResourceExhausted, {}, FailureReason::CapacityLimit});
  } catch (...) {
    return reject(
        Status{ErrorCode::OperationFailed, {}, FailureReason::HostException});
  }
}
}  // namespace ps
