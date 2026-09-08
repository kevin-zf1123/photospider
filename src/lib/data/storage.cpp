#include "photospider/data/storage.hpp"

#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace ps {
ByteView CpuStorage::bytes() const noexcept {
  if (adopted_)
    return ByteView(adopted_->data(), adopted_->size());
  return ByteView(allocated_.get(), static_cast<std::size_t>(capacity_));
}
CpuStorage::~CpuStorage() noexcept = default;
std::uint8_t* MutableBuffer::data() noexcept {
  return storage_ ? storage_->allocated_.get() : nullptr;
}
std::size_t MutableBuffer::size() const noexcept {
  return storage_ ? static_cast<std::size_t>(storage_->capacity_) : 0;
}
std::shared_ptr<const CpuStorage> MutableBuffer::freeze() && noexcept {
  return std::move(storage_);
}
BufferAllocator::BufferAllocator(Reserve reserve,
                                 std::shared_ptr<const void> domain)
    : reserve_(std::move(reserve)), domain_(std::move(domain)) {}
bool BufferAllocator::owns(const CpuStorage& storage) const noexcept {
  return domain_ && domain_ == storage.domain_;
}
Result<MutableBuffer> BufferAllocator::allocate(std::uint64_t size) const {
  if (size == 0 || size > static_cast<std::uint64_t>(INT64_MAX) ||
      size > std::numeric_limits<std::size_t>::max()) {
    return Result<MutableBuffer>(
        Status::failure(ErrorCode::ResourceExhausted,
                        "CPU allocation size is not addressable"));
  }
  try {
    MutableBuffer result;
    result.storage_ = std::shared_ptr<CpuStorage>(new CpuStorage());
    result.storage_->domain_ = domain_;
    if (reserve_) {
      auto lease = reserve_(size);
      if (!lease.ok())
        return Result<MutableBuffer>(lease.status());
      result.storage_->lease_ = lease.take_value();
    }
    result.storage_->allocated_ =
        std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(size));
    result.storage_->capacity_ = size;
    return Result<MutableBuffer>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<MutableBuffer>(
        Status::failure(ErrorCode::ResourceExhausted, "CPU allocation failed"));
  }
}
}  // namespace ps
