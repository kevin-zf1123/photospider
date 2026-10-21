#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "core/resource_observation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/resources.hpp"

namespace ps {
// Root and lease state own admission and counters. Storage aliases are opaque
// here; the data adapter owns their intrusive registrations and retirement.
struct ResourceBudget::Impl {
  explicit Impl(ResourceLimits value) : limits(std::move(value)) {
    stats.protected_cleanup = limits.cleanup;
  }
  struct Reference;
  Reference* references = nullptr;
  std::mutex reference_admission;
  std::condition_variable reference_changed;
  std::mutex mutex;
  ResourceLimits limits;
  ResourceStatistics stats;
  std::function<void(const ResourceCapacity&)> reclaimer;
  void reclaim(const ResourceCapacity& capacity) {
    thread_local bool active = false;
    if (active)
      return;
    std::function<void(const ResourceCapacity&)> callback;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (fits(capacity))
        return;
      callback = reclaimer;
    }
    if (!callback)
      return;
    // Reference retirement takes this gate. A caller already holding it must
    // preflight reclamation outside the gate rather than retire its own alias.
    std::unique_lock<std::mutex> admission(reference_admission,
                                           std::try_to_lock);
    if (!admission.owns_lock())
      return;
    admission.unlock();
    active = true;
    struct Restore {
      bool& flag;
      ~Restore() { flag = false; }
    } restore{active};
    try {
      callback(capacity);
    } catch (...) {
    }
  }
  std::atomic<std::uint64_t> issued_work{0};
  bool admit_work(std::uint64_t amount) {
    if (!amount)
      return true;
    auto prior = issued_work.load(std::memory_order_relaxed);
    do {
      if (!core_internal::can_add(amount, prior, limits.maximum_work))
        return false;
    } while (!issued_work.compare_exchange_weak(prior, prior + amount,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed));
    return true;
  }
  bool fits(const ResourceCapacity& c) const {
    for (std::size_t i = 0; i < c.values.size(); ++i) {
      const auto limit = limits.capacity.values[i];
      std::uint64_t used = 0;
      if (!core_internal::checked_add(stats.protected_cleanup.values[i],
                                      stats.live.values[i], &used, limit) ||
          !core_internal::can_add(used, c.values[i], limit))
        return false;
    }
    return true;
  }
  void add(const ResourceCapacity& c) {
    for (std::size_t i = 0; i < c.values.size(); ++i) {
      stats.live.values[i] += c.values[i];
      stats.peak.values[i] =
          std::max(stats.peak.values[i], stats.live.values[i]);
    }
  }
  void commit_metadata(std::uint64_t bytes) noexcept {
    stats.allocated_metadata_bytes += bytes;
    stats.peak_allocated_metadata_bytes = std::max(
        stats.peak_allocated_metadata_bytes, stats.allocated_metadata_bytes);
  }
};
struct ResourceLease::Impl {
  std::shared_ptr<ResourceBudget::Impl> root;
  ResourceCapacity amount;
  core_internal::PayloadCapture observation;
  std::weak_ptr<core_internal::MetadataObservation> metadata_observation;
  std::uint64_t committed_payload = 0;
  std::uint64_t committed_metadata = 0;
  bool quarantined = false;
  ~Impl() {
    if (!root)
      return;
    std::lock_guard<std::mutex> lock(root->mutex);
    // The bookkeeping object retires even when unverified external storage
    // remains quarantined. Its reserved capacity remains protected as before.
    root->stats.allocated_metadata_bytes -= sizeof(Impl);
    core_internal::ResourceMetadataAccess::update(
        metadata_observation, 0,
        quarantined ? 0 : amount[ResourceKind::Metadata], 0,
        sizeof(Impl) + (quarantined ? 0 : committed_metadata));
    if (!quarantined) {
      root->stats.allocated_metadata_bytes -= committed_metadata;
      for (std::size_t i = 0; i < amount.values.size(); ++i)
        root->stats.live.values[i] -= amount.values[i];
      core_internal::ResourcePayloadAccess::update(
          observation, 0, amount[ResourceKind::Payload], 0, committed_payload);
    }
  }
};
}  // namespace ps
