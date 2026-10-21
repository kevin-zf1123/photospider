#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "photospider/core/resources.hpp"

namespace ps::execution_internal {
class BlockMemory;
}
namespace ps::gpu_internal {
/** @brief Device-only allocation owner with no host address or byte access.
 * Each owner has one native allocation. Its native resource retires before
 * its capacity reservation; it may outlive Device and ExecutionContext.
 * Only Device populates an owner. GPU invocations borrow its opaque identity.
 */
class DeviceStorage final {
 public:
  DeviceStorage() = default;
  DeviceStorage(const DeviceStorage&) = delete;
  DeviceStorage& operator=(const DeviceStorage&) = delete;
  std::uint64_t size() const noexcept { return size_; }
  std::uint64_t capacity() const noexcept { return capacity_; }
  /** @brief Borrowed opaque identity; never a CPU address or offset base. */
  void* handle() noexcept { return this; }
  const void* handle() const noexcept { return this; }
  /** @brief Revoke future writable bindings after producer completion.
   * Already borrowed bindings must be quiescent before sealing.
   */
  void seal() noexcept { writable_.store(false); }
  bool owned_by(const ResourceBudget& budget) const noexcept {
    return budget_ && budget_->same_owner(budget);
  }

 private:
  friend class Device;
  friend class ps::execution_internal::BlockMemory;
  std::optional<ResourceBudget> budget_;
  std::shared_ptr<void> lease_;
  std::shared_ptr<const void> domain_;
  std::shared_ptr<void> native_owner_;
  std::uint64_t size_ = 0, capacity_ = 0;
  std::atomic<bool> writable_{true};
};
}  // namespace ps::gpu_internal
