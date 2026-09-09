#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/data/storage.hpp"

namespace ps::execution_internal {
class MemoryReservation;
/** @brief Per-Run observations shared by collector, Whole boundaries and tiles.
 */
struct MemoryObservation final {
  std::uint64_t live = 0, peak = 0, reserved = 0, peak_reserved = 0;
};

/** @brief Shared payload budget; retained results may outlive the context. */
class MemoryBudget final : public std::enable_shared_from_this<MemoryBudget> {
 public:
  explicit MemoryBudget(std::uint64_t maximum) : maximum_(maximum) {
    if (maximum == 0)
      throw std::invalid_argument("memory budget must be positive");
  }
  Result<std::shared_ptr<MemoryReservation>> reserve(
      std::uint64_t bytes, const std::function<ErrorCode()>& stop = {},
      std::shared_ptr<MemoryObservation> observation = {},
      const std::function<void()>& reclaim = {});
  std::pair<std::uint64_t, std::uint64_t> peaks(
      const std::shared_ptr<MemoryObservation>& observation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {observation->peak, observation->peak_reserved};
  }
  std::uint64_t available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maximum_ - reserved_;
  }
  std::uint64_t live() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_;
  }

 private:
  friend class MemoryReservation;
  mutable std::mutex mutex_;
  const std::uint64_t maximum_;
  std::uint64_t reserved_ = 0;
  std::uint64_t live_ = 0;
  std::size_t active_ = 0;
  std::uint64_t admission_epoch_ = 0;
  std::condition_variable changed_;
};

/**
 * @brief Complete working-set reservation split into allocation-owned leases.
 * @note Sealing returns unused capacity, while each retained allocation keeps
 * its own capacity reserved until destruction. All transitions use one mutex.
 */
class MemoryReservation final
    : public std::enable_shared_from_this<MemoryReservation> {
 public:
  ~MemoryReservation() { seal(); }
  BufferAllocator allocator(std::uint64_t limit = UINT64_MAX) {
    struct Local {
      std::mutex mutex;
      std::uint64_t used = 0;
    };
    struct Lease {
      std::shared_ptr<Local> local;
      std::shared_ptr<void> allocation;
      std::uint64_t bytes = 0;
      ~Lease() {
        if (local && bytes != 0) {
          std::lock_guard<std::mutex> lock(local->mutex);
          local->used -= bytes;
        }
      }
    };
    auto self = shared_from_this();
    auto local = std::make_shared<Local>();
    return BufferAllocator(
        [self, local, limit](std::uint64_t bytes) {
          auto lease = std::make_shared<Lease>();
          lease->local = local;
          {
            std::lock_guard<std::mutex> lock(local->mutex);
            if (bytes > limit - local->used)
              return Result<std::shared_ptr<void>>(
                  Status::failure(ErrorCode::ResourceExhausted,
                                  "invocation exceeds declared workspace"));
            local->used += bytes;
            lease->bytes = bytes;
          }
          auto allocation = self->allocate(bytes);
          if (!allocation.ok())
            return Result<std::shared_ptr<void>>(allocation.status());
          lease->allocation = allocation.take_value();
          return Result<std::shared_ptr<void>>(std::move(lease));
        },
        budget_);
  }
  void seal() {
    std::lock_guard<std::mutex> lock(budget_->mutex_);
    if (!sealed_ && admitted_) {
      budget_->reserved_ -= capacity_ - used_;
      observation_->reserved -= capacity_ - used_;
      capacity_ = used_;
      sealed_ = true;
      --budget_->active_;
      ++budget_->admission_epoch_;
      budget_->changed_.notify_all();
    }
  }
  std::uint64_t peak() const {
    std::lock_guard<std::mutex> lock(budget_->mutex_);
    return peak_;
  }
  std::uint64_t planned() const noexcept { return planned_; }

 private:
  friend class MemoryBudget;
  struct Allocation final {
    std::shared_ptr<MemoryReservation> owner;
    std::uint64_t bytes = 0;
    ~Allocation() {
      if (!owner || bytes == 0)
        return;
      std::lock_guard<std::mutex> lock(owner->budget_->mutex_);
      owner->used_ -= bytes;
      owner->budget_->live_ -= bytes;
      owner->observation_->live -= bytes;
      if (owner->sealed_) {
        owner->capacity_ -= bytes;
        owner->budget_->reserved_ -= bytes;
        owner->observation_->reserved -= bytes;
        owner->budget_->changed_.notify_all();
      }
    }
  };
  MemoryReservation(std::shared_ptr<MemoryBudget> budget, std::uint64_t bytes)
      : budget_(std::move(budget)), capacity_(bytes), planned_(bytes) {}
  Result<std::shared_ptr<void>> allocate(std::uint64_t bytes) {
    // Allocate lease metadata before locking; failure cannot leak accounting.
    auto lease = std::make_shared<Allocation>();
    lease->owner = shared_from_this();
    std::lock_guard<std::mutex> lock(budget_->mutex_);
    if (sealed_ || bytes > capacity_ - used_)
      return Result<std::shared_ptr<void>>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "operation exceeds reserved working set"));
    used_ += bytes;
    budget_->live_ += bytes;
    observation_->live += bytes;
    observation_->peak = std::max(observation_->peak, observation_->live);
    peak_ = std::max(peak_, used_);
    lease->bytes = bytes;
    return Result<std::shared_ptr<void>>(std::move(lease));
  }
  std::shared_ptr<MemoryBudget> budget_;
  std::shared_ptr<MemoryObservation> observation_;
  std::uint64_t capacity_;
  std::uint64_t planned_;
  std::uint64_t used_ = 0;
  std::uint64_t peak_ = 0;
  bool sealed_ = false;
  bool admitted_ = false;
};

inline Result<std::shared_ptr<MemoryReservation>> MemoryBudget::reserve(
    std::uint64_t bytes, const std::function<ErrorCode()>& stop,
    std::shared_ptr<MemoryObservation> observation,
    const std::function<void()>& reclaim) {
  auto reservation = std::shared_ptr<MemoryReservation>(
      new MemoryReservation(shared_from_this(), 0));
  reservation->observation_ = observation
                                  ? std::move(observation)
                                  : std::make_shared<MemoryObservation>();
  std::unique_lock<std::mutex> lock(mutex_);
  if (bytes > maximum_)
    return Result<std::shared_ptr<MemoryReservation>>(Status::failure(
        ErrorCode::ResourceExhausted, "minimum working set exceeds budget"));
  while (reserved_ > maximum_ - bytes) {
    if (reclaim) {
      const auto epoch = admission_epoch_;
      lock.unlock();
      reclaim();
      lock.lock();
      if (reserved_ <= maximum_ - bytes)
        break;
      // A producer can publish a new cache entry while reclamation runs.
      // Revisit it after its admission/seal transition before declaring
      // failure.
      if (epoch != admission_epoch_)
        continue;
    }
    if (stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok) {
        Status failure;
        failure.code = code;
        return Result<std::shared_ptr<MemoryReservation>>(std::move(failure));
      }
    }
    if (!stop || active_ == 0)
      return Result<std::shared_ptr<MemoryReservation>>(
          Status::failure(ErrorCode::ResourceExhausted,
                          "retained results exhaust available budget"));
    changed_.wait_for(lock, std::chrono::milliseconds(2));
  }
  reservation->capacity_ = bytes;
  reservation->planned_ = bytes;
  reservation->admitted_ = true;
  reserved_ += bytes;
  reservation->observation_->reserved += bytes;
  reservation->observation_->peak_reserved =
      std::max(reservation->observation_->peak_reserved,
               reservation->observation_->reserved);
  ++active_;
  ++admission_epoch_;
  return Result<std::shared_ptr<MemoryReservation>>(std::move(reservation));
}
}  // namespace ps::execution_internal
