#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "photospider/core/checked_math.hpp"
#include "photospider/data/storage.hpp"

namespace ps::data_internal {
class MemoryReservation;
/** @brief Independent aggregate capacity for all live snapshot blocks. */
class MemoryBudget final : public std::enable_shared_from_this<MemoryBudget> {
 public:
  explicit MemoryBudget(std::uint64_t maximum) : maximum_(maximum) {
    if (!maximum)
      throw std::invalid_argument("snapshot memory budget must be positive");
  }
  Result<std::shared_ptr<MemoryReservation>> reserve(std::uint64_t bytes);
  std::uint64_t live() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_;
  }

 private:
  friend class MemoryReservation;
  mutable std::mutex mutex_;
  const std::uint64_t maximum_;
  std::uint64_t reserved_ = 0, live_ = 0;
};
/** @brief Snapshot working-set reservation split into allocation-owned leases.
 * Sealing releases unused bytes; each retained block keeps its capacity until
 * the last storage owner retires. All accounting transitions use one mutex.
 */
class MemoryReservation final
    : public std::enable_shared_from_this<MemoryReservation> {
 public:
  ~MemoryReservation() { seal(); }
  BufferAllocator allocator() {
    return BufferAllocator(
        [self = shared_from_this()](std::uint64_t bytes) {
          return self->allocate(bytes);
        },
        budget_);
  }
  void seal() {
    std::lock_guard<std::mutex> lock(budget_->mutex_);
    if (!sealed_) {
      budget_->reserved_ -= capacity_ - used_;
      capacity_ = used_;
      sealed_ = true;
    }
  }

 private:
  friend class MemoryBudget;
  struct Allocation final {
    std::shared_ptr<MemoryReservation> owner;
    std::uint64_t bytes = 0;
    ~Allocation() {
      if (!owner || !bytes)
        return;
      std::lock_guard<std::mutex> lock(owner->budget_->mutex_);
      owner->used_ -= bytes;
      owner->budget_->live_ -= bytes;
      if (owner->sealed_) {
        owner->capacity_ -= bytes;
        owner->budget_->reserved_ -= bytes;
      }
    }
  };
  explicit MemoryReservation(std::shared_ptr<MemoryBudget> budget)
      : budget_(std::move(budget)) {}
  Result<std::shared_ptr<void>> allocate(std::uint64_t bytes) {
    // Construct the owner before charging so allocation failure cannot leak.
    auto lease = std::make_shared<Allocation>();
    lease->owner = shared_from_this();
    std::lock_guard<std::mutex> lock(budget_->mutex_);
    if (sealed_ || !core_internal::can_add(bytes, used_, capacity_))
      return Result<std::shared_ptr<void>>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "snapshot exceeds reserved working set"));
    used_ += bytes;
    budget_->live_ += bytes;
    lease->bytes = bytes;
    return Result<std::shared_ptr<void>>(std::move(lease));
  }
  std::shared_ptr<MemoryBudget> budget_;
  std::uint64_t capacity_ = 0, used_ = 0;
  bool sealed_ = false;
};
inline Result<std::shared_ptr<MemoryReservation>> MemoryBudget::reserve(
    std::uint64_t bytes) {
  auto reservation = std::shared_ptr<MemoryReservation>(
      new MemoryReservation(shared_from_this()));
  std::lock_guard<std::mutex> lock(mutex_);
  if (!core_internal::can_add(bytes, reserved_, maximum_))
    return Result<std::shared_ptr<MemoryReservation>>(
        Status::failure(ErrorCode::ResourceExhausted,
                        "live snapshots exhaust available budget"));
  reservation->capacity_ = bytes;
  reserved_ += bytes;
  return Result<std::shared_ptr<MemoryReservation>>(std::move(reservation));
}
}  // namespace ps::data_internal
