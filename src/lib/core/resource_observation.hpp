#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "photospider/core/resources.hpp"

namespace ps::core_internal {

// This object owns counters only, never a Root, Run, Result or allocation.
// Payload leases can retain it after their driver and context have retired.
class PayloadObservation final {
 public:
  using Snapshot = std::pair<std::uint64_t, std::uint64_t>;
  Snapshot peaks() const noexcept;
  Snapshot live_bytes() const noexcept;

 private:
  friend struct ResourcePayloadAccess;
  mutable std::mutex mutex_;
  std::uint64_t live_ = 0, peak_ = 0;
  std::uint64_t reserved_ = 0, peak_reserved_ = 0;
};

struct PayloadCapture final {
  std::shared_ptr<PayloadObservation> run;
  std::shared_ptr<PayloadObservation> producer;
};

// Admission captures the matching Root scope. Commit and retirement use the
// lease's stored capture, including on another worker or after scope exit.
class ResourcePayloadScope final {
 public:
  ResourcePayloadScope(const ResourceBudget& root,
                       PayloadCapture capture) noexcept;
  ~ResourcePayloadScope() noexcept;
  ResourcePayloadScope(const ResourcePayloadScope&) = delete;
  ResourcePayloadScope& operator=(const ResourcePayloadScope&) = delete;
  static PayloadCapture capture(const ResourceBudget& root) noexcept;
  static PayloadCapture capture_owner(const void* root) noexcept;

 private:
  const ResourceBudget& root_;
  PayloadCapture capture_;
  ResourcePayloadScope* previous_;
};

struct ResourcePayloadAccess final {
  // Trusted allocation fences only. They do not admit capacity or allocate.
  static void commit(ResourceLease& lease, std::uint64_t bytes) noexcept;
  static void withdraw(ResourceLease& lease, std::uint64_t bytes) noexcept;
  static void commit_owner(const std::shared_ptr<void>& owner,
                           std::uint64_t bytes) noexcept;
  static void update(const PayloadCapture& capture, std::uint64_t reserved_add,
                     std::uint64_t reserved_remove, std::uint64_t actual_add,
                     std::uint64_t actual_remove) noexcept;
};

}  // namespace ps::core_internal
