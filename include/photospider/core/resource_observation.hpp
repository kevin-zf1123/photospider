#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "photospider/core/resources.hpp"

namespace ps::core_internal {

// Capture any matching-root capacity rejection during a compound admission.
// Fixed storage is caller-owned and must outlive this thread-local scope.
class ResourceAdmissionScope final {
 public:
  ResourceAdmissionScope(const ResourceBudget& root,
                         ResourceAdmissionFailure* failure,
                         bool* recorded) noexcept;
  ~ResourceAdmissionScope() noexcept;
  ResourceAdmissionScope(const ResourceAdmissionScope&) = delete;
  ResourceAdmissionScope& operator=(const ResourceAdmissionScope&) = delete;
  static void record(const ResourceBudget& root,
                     const ResourceAdmissionFailure& failure) noexcept;

 private:
  const ResourceBudget& root_;
  ResourceAdmissionFailure* failure_;
  bool* recorded_;
  ResourceAdmissionScope* previous_;
};

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

// Pure counters with no resource, Run or Result ownership. Leases hold weak
// references so a diagnostic reservation can observe itself without a cycle.
class MetadataObservation final {
 public:
  ResourceMetadataStatistics snapshot() const noexcept;

 private:
  friend struct ResourceMetadataAccess;
  mutable std::mutex mutex_;
  ResourceMetadataStatistics statistics_;
};

// New reservations capture only the innermost matching root. Existing leases
// keep their original attribution when aliased or used from another scope.
class ResourceMetadataScope final {
 public:
  ResourceMetadataScope(
      const ResourceBudget& root,
      std::weak_ptr<MetadataObservation> observation) noexcept;
  ~ResourceMetadataScope() noexcept;
  ResourceMetadataScope(const ResourceMetadataScope&) = delete;
  ResourceMetadataScope& operator=(const ResourceMetadataScope&) = delete;
  static std::weak_ptr<MetadataObservation> capture(
      const ResourceBudget& root) noexcept;

 private:
  const ResourceBudget& root_;
  std::weak_ptr<MetadataObservation> observation_;
  ResourceMetadataScope* previous_;
};

// Trusted allocation/deallocation fences for storage covered by a metadata
// reservation. Lease-object bytes are handled automatically and are excluded
// from these explicit amounts. Withdrawal is necessary when storage retires
// before its last lease alias; otherwise lease retirement withdraws the rest.
struct ResourceMetadataAccess final {
  // Bootstrap only: attribute an unpublished, previously unobserved lease,
  // including allocations already committed while constructing diagnostics.
  static void attribute(
      ResourceLease& lease,
      const std::shared_ptr<MetadataObservation>& observation) noexcept;
  static void update(const std::weak_ptr<MetadataObservation>& observation,
                     std::uint64_t reserved_add, std::uint64_t reserved_remove,
                     std::uint64_t actual_add,
                     std::uint64_t actual_remove) noexcept;
  static void commit(ResourceLease& lease, std::uint64_t bytes) noexcept;
  static void commit_owner(const std::shared_ptr<void>& owner,
                           std::uint64_t bytes) noexcept;
  static void withdraw(ResourceLease& lease, std::uint64_t bytes) noexcept;
};

// A subset of a shared reservation whose storage retires independently.
// Declare before the covered storage members so unwind destroys storage first.
// Commit only successful allocations; this guard neither reserves nor
// allocates.
class MetadataAllocation final {
 public:
  explicit MetadataAllocation(ResourceLease lease = {}) noexcept
      : lease_(std::move(lease)) {}
  ~MetadataAllocation() noexcept {
    ResourceMetadataAccess::withdraw(lease_, bytes_);
  }
  MetadataAllocation(const MetadataAllocation&) = delete;
  MetadataAllocation& operator=(const MetadataAllocation&) = delete;
  MetadataAllocation(MetadataAllocation&& other) noexcept
      : lease_(std::move(other.lease_)),
        bytes_(std::exchange(other.bytes_, 0)) {}
  // Storage owners swap this guard together with their covered buffers.
  // No move assignment: replacing a guard alone could precede deallocation.
  void swap(MetadataAllocation& other) noexcept {
    std::swap(lease_, other.lease_);
    std::swap(bytes_, other.bytes_);
  }
  void commit(std::uint64_t bytes) noexcept {
    ResourceMetadataAccess::commit(lease_, bytes);
    bytes_ += bytes;
  }

 private:
  ResourceLease lease_;
  std::uint64_t bytes_ = 0;
};

}  // namespace ps::core_internal
