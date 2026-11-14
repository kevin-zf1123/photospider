#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "photospider/core/resources.hpp"

namespace ps::gpu_internal {
/** @brief Device-only allocation owner with no host address or byte access.
 * Each owner has one native allocation. Its native resource retires before
 * its capacity reservation; it may outlive Device and ExecutionContext.
 * Only Device populates an owner. GPU invocations borrow its opaque identity.
 */
class DeviceStorage final {
 public:
  /** @brief Create an uninitialized owner in the data library.
   * A supplied root accounts for the shared control block and keeps its
   * provider code loaded until native destruction and deallocation return.
   * A null root allocates unmanaged metadata. May throw std::bad_alloc.
   */
  static std::shared_ptr<DeviceStorage> create(const ResourceBudget* root);
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

  /** @brief Attach one opaque backing before making this allocation visible. */
  void adopt(std::uint64_t size, std::uint64_t capacity,
             std::shared_ptr<void> native, std::shared_ptr<const void> domain,
             std::shared_ptr<void> lease) {
    if (native_owner_ || !size || size > capacity || !native || !domain)
      throw std::invalid_argument("invalid device backing");
    size_ = size;
    capacity_ = capacity;
    native_owner_ = std::move(native);
    domain_ = std::move(domain);
    lease_ = std::move(lease);
  }
  bool native_domain(const std::shared_ptr<const void>& domain) const noexcept {
    return domain && domain_ == domain;
  }
  void* native_handle() const noexcept { return native_owner_.get(); }
  bool writable_native() const noexcept { return writable_.load(); }
  void account(const ResourceBudget& budget) {
    if (budget_)
      throw std::logic_error("device backing already accounted");
    budget_ = budget;
  }

 private:
  std::optional<ResourceBudget> budget_;
  std::shared_ptr<void> lease_;
  std::shared_ptr<const void> domain_;
  std::shared_ptr<void> native_owner_;
  std::uint64_t size_ = 0, capacity_ = 0;
  std::atomic<bool> writable_{true};
};
}  // namespace ps::gpu_internal
