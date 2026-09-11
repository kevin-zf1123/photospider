#include "photospider/data/storage.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace ps {
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
  BufferAllocator result(
      [parent = reserve_, state, maximum_bytes](std::uint64_t bytes) {
        auto lease = std::make_shared<Lease>();
        lease->state = state;
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (bytes > maximum_bytes - state->live)
            return Result<std::shared_ptr<void>>(
                Status::failure(ErrorCode::ResourceExhausted,
                                "allocator live sublimit exceeded"));
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
      },
      domain_);
  result.native_allocate_ = native_allocate_;
  result.allocation_scopes_ = allocation_scopes_;
  result.allocation_scopes_.push_back(state);
  result.failure_ = [parent = failure_,
                     observer = std::move(failure)](ErrorCode code) {
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
    if (native_allocate_) {
      auto result = native_allocate_(size, reserve_, domain_);
      if (!result.ok())
        return reject(result.status());
      result.value().storage_->allocation_scopes_ = allocation_scopes_;
      return result;
    }
    if (size == 0 || size > static_cast<std::uint64_t>(INT64_MAX) ||
        size > std::numeric_limits<std::size_t>::max()) {
      return reject(Status::failure(ErrorCode::ResourceExhausted,
                                    "CPU allocation size is not addressable"));
    }
    MutableBuffer result;
    result.storage_ = std::shared_ptr<CpuStorage>(new CpuStorage());
    result.storage_->domain_ = domain_;
    result.storage_->allocation_scopes_ = allocation_scopes_;
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
        Status::failure(ErrorCode::ResourceExhausted, "CPU allocation failed"));
  }
}
}  // namespace ps
