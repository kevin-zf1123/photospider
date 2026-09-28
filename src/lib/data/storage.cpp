#include "photospider/data/storage.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace ps {
namespace {
void notify_failure(const BufferAllocator::FailureObserver& observer,
                    ErrorCode code) noexcept {
  if (observer) {
    try {
      observer(code);
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
    struct State {
      std::mutex mutex;
      std::uint64_t live = 0;
    };
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
    auto state = std::make_shared<State>();
    const auto limited_reserve = [state, maximum_bytes](Reserve parent) {
      return Reserve([parent = std::move(parent), state,
                      maximum_bytes](std::uint64_t bytes) {
        auto lease = std::make_shared<Lease>();
        lease->state = state;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (bytes > maximum_bytes - state->live)
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
    }
    result.allocation_scopes_.push_back(state);
    result.failure_ = [parent = failure_, observer = failure](ErrorCode code) {
      if (parent) {
        try {
          parent(code);
        } catch (...) {
        }
      }
      if (observer) {
        try {
          observer(code);
        } catch (...) {
        }
      }
    };
    return result;
  } catch (const std::bad_alloc&) {
    notify_failure(failure_, ErrorCode::ResourceExhausted);
    notify_failure(failure, ErrorCode::ResourceExhausted);
    throw;
  } catch (...) {
    notify_failure(failure_, ErrorCode::OperationFailed);
    notify_failure(failure, ErrorCode::OperationFailed);
    throw;
  }
}

bool BufferAllocator::owns_allocation(
    const MutableBuffer& buffer) const noexcept {
  return buffer.storage_ && owns_allocation(*buffer.storage_);
}
bool BufferAllocator::owns_allocation(
    const CpuStorage& storage) const noexcept {
  if (allocation_scopes_.empty())
    return false;
  const auto& scope = allocation_scopes_.back();
  return std::find(storage.allocation_scopes_.begin(),
                   storage.allocation_scopes_.end(),
                   scope) != storage.allocation_scopes_.end();
}
Result<MutableBuffer> BufferAllocator::allocate(std::uint64_t size) const {
  const auto reject = [&](Status status) {
    if (failure_) {
      try {
        failure_(status.code);
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
      auto result = native_allocate_(size, reserve_, domain_);
      if (!result.ok())
        return reject(result.status());
      result.value().storage_->allocation_scopes_ = allocation_scopes_;
      result.value().storage_->requested_lease_ = std::move(requested_lease);
      return result;
    }
    if (size == 0 || size > static_cast<std::uint64_t>(INT64_MAX) ||
        size > std::numeric_limits<std::size_t>::max()) {
      return reject(Status{ErrorCode::ResourceExhausted,
                           "CPU allocation size is not addressable",
                           FailureReason::CapacityLimit});
    }
    MutableBuffer result;
    result.storage_ = std::shared_ptr<CpuStorage>(new CpuStorage());
    result.storage_->domain_ = domain_;
    result.storage_->allocation_scopes_ = allocation_scopes_;
    result.storage_->requested_lease_ = std::move(requested_lease);
    if (reserve_) {
      auto lease = reserve_(size);
      if (!lease.ok())
        return reject(lease.status());
      result.storage_->lease_ = lease.take_value();
    }
    result.storage_->allocated_ =
        std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(size));
    result.storage_->capacity_ = size;
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
