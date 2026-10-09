#pragma once

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "data/memory_budget.hpp"
#include "execution/structured_cache.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
/** @brief Context-owned native backing and structured Result LRU.
 * @note Retired owners drain after releasing the index mutex. Acquired
 * candidates retain ownership until their caller releases them.
 */
class ResultCache final {
  struct StructuredIdentity final {};

 public:
  class StructuredCandidate final {
   public:
    const StructuredCacheManifest* operator->() const noexcept {
      return manifest_.get();
    }

   private:
    friend class ResultCache;
    StructuredCandidate(std::shared_ptr<const StructuredCacheManifest> manifest,
                        std::shared_ptr<const StructuredIdentity> identity,
                        std::uint64_t epoch)
        : manifest_(std::move(manifest)),
          identity_(std::move(identity)),
          epoch_(epoch) {}
    std::shared_ptr<const StructuredCacheManifest> manifest_;
    std::shared_ptr<const StructuredIdentity> identity_;
    std::uint64_t epoch_;
  };
  ResultCache(std::uint64_t limit,
              std::shared_ptr<data_internal::MemoryBudget> budget,
              std::uint64_t dependency_metadata_limit = 65536)
      : limit_(limit),
        budget_(std::move(budget)),
        dependency_metadata_limit_(dependency_metadata_limit),
        structured_manifests_(
            budget_->resources()
                ? make_resource_map<StructuredCandidates>(*budget_->resources())
                : StructuredIndex{}) {}
  ~ResultCache() { close(); }
  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
  }
  ResultCacheStatistics statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stats = stats_;
    stats.retained_bytes = bytes_;
    stats.entries = entries_.size();
    std::set<const void*> native_owners;
    for (const auto& entry : entries_) {
      if (entry.second.structured) {
        for (const auto& owner : entry.second.allocations)
          if (owner.native && native_owners.insert(owner.owner).second)
            stats.native_retained_bytes += owner.bytes;
      } else if (entry.second.native &&
                 native_owners.insert(entry.second.value.storage().get())
                     .second) {
        stats.native_retained_bytes += entry.second.value.storage()->capacity();
      }
    }
    return stats;
  }
  std::uint64_t epoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return epoch_;
  }
  std::uint64_t dependency_metadata_limit() const noexcept {
    return dependency_metadata_limit_;
  }
  void clear() {
    decltype(entries_) retired;
    auto structured =
        budget_->resources()
            ? make_resource_map<StructuredCandidates>(*budget_->resources())
            : StructuredIndex{};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++epoch_;
      stats_.evictions += entries_.size();
      retired.swap(entries_);
      structured.swap(structured_manifests_);
      lru_.clear();
      owners_.clear();
      bytes_ = structured_metadata_ = 0;
    }
  }
  std::vector<StructuredCandidate> structured_candidates(
      const std::string& key, std::uint64_t epoch) const {
    std::vector<StructuredCandidate> candidates;
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || epoch != epoch_)
      return candidates;
    auto found = structured_manifests_.find(std::string_view(key));
    if (found == structured_manifests_.end())
      return candidates;
    for (auto item = found->second.rbegin(); item != found->second.rend();
         ++item) {
      if (auto candidate = item->lock()) {
        auto entry = entries_.find(std::string(candidate->key));
        if (candidate->epoch == epoch_ && entry != entries_.end() &&
            entry->second.structured == candidate)
          candidates.push_back(StructuredCandidate(
              std::move(candidate), structured_identity_, epoch_));
      }
    }
    return candidates;
  }
  bool structured_verified(const StructuredCandidate& candidate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || candidate.identity_ != structured_identity_ ||
        candidate.epoch_ != epoch_ || !candidate.manifest_ ||
        candidate.manifest_->epoch != epoch_) {
      ++stats_.misses;
      return false;
    }
    // Lookup minted this capability while the exact entry was resident.
    // Replay may evict it, but the candidate still owns its accounted Result.
    // Clear invalidates the epoch; a new lookup cannot acquire an evicted
    // entry.
    auto found = entries_.find(std::string(candidate.manifest_->key));
    if (found != entries_.end() &&
        found->second.structured == candidate.manifest_)
      lru_.splice(lru_.end(), lru_, found->second.order);
    ++stats_.hits;
    return true;
  }
  void put_structured(
      const std::string& key,
      std::shared_ptr<const StructuredCacheManifest> manifest,
      const std::function<Status(std::uint64_t)>& work) noexcept {
    try {
      if (!limit_ || !manifest || !budget_->resources() ||
          !manifest->result.owned_by(*budget_->resources()) ||
          !manifest->metadata ||
          manifest->metadata > dependency_metadata_limit_)
        return;
      ResourceAllocationScope optional(*budget_->resources());
      auto footprint = manifest->result.cache_storage(work);
      if (!footprint.ok())
        return;
      std::uint64_t total = 0;
      for (const auto& allocation : footprint.value()) {
        if (allocation.bytes > limit_ || total > limit_ - allocation.bytes)
          return;
        total += allocation.bytes;
      }
      std::shared_ptr<const StructuredIdentity> identity;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        identity = structured_identity_;
      }
      // Admission is optional and outside the cache lock: Root reclamation
      // may reenter the cache. No identity is allocated for a disabled cache.
      if (!identity)
        identity = std::allocate_shared<StructuredIdentity>(
            ResourceAllocator<StructuredIdentity>(*budget_->resources()));
      auto extra = budget_->resources()->reserve(ResourceCapacity::host(
          sizeof(Entry) + 2 * (manifest->key.size() + 1) +
              footprint.value().size() * 64,
          sizeof(Entry) + 2 * (manifest->key.size() + 1) +
              footprint.value().size() * 64));
      if (!extra.ok())
        return;
      decltype(entries_) retired;
      std::lock_guard<std::mutex> lock(mutex_);
      if (closing_ || manifest->epoch != epoch_ ||
          entries_.count(std::string(manifest->key)))
        return;
      std::uint64_t fresh = 0;
      for (const auto& allocation : footprint.value())
        if (!owners_.count(allocation.owner))
          fresh += allocation.bytes;
      while (!entries_.empty() &&
             (entries_.size() >= 4096 || bytes_ > limit_ - fresh ||
              structured_metadata_ >
                  dependency_metadata_limit_ - manifest->metadata)) {
        retired.insert(evict());
        fresh = 0;
        for (const auto& allocation : footprint.value())
          if (!owners_.count(allocation.owner))
            fresh += allocation.bytes;
      }
      if (structured_metadata_ >
          dependency_metadata_limit_ - manifest->metadata)
        return;
      for (auto item = structured_manifests_.begin();
           item != structured_manifests_.end();) {
        auto& stale = item->second;
        stale.erase(std::remove_if(stale.begin(), stale.end(),
                                   [](const auto& candidate) {
                                     return candidate.expired();
                                   }),
                    stale.end());
        if (stale.empty())
          item = structured_manifests_.erase(item);
        else
          ++item;
      }
      if (structured_manifests_.size() >= dependency_metadata_limit_ &&
          structured_manifests_.find(std::string_view(key)) ==
              structured_manifests_.end())
        return;
      auto position = structured_manifests_.try_emplace(
          ResourceString(key.data(), key.size(),
                         ResourceAllocator<char>(*budget_->resources())));
      auto& candidates = position.first->second;
      candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                      [](const auto& candidate) {
                                        return candidate.expired();
                                      }),
                       candidates.end());
      if (candidates.size() >= 8)
        candidates.erase(candidates.begin());
      const auto entry_key = std::string(manifest->key);
      lru_.push_back(entry_key);
      Entry entry;
      entry.order = std::prev(lru_.end());
      entry.structured = manifest;
      entry.allocations = footprint.take_value();
      entry.lease = extra.take_value();
      std::size_t registered = 0;
      try {
        entries_.emplace(entry_key, std::move(entry));
        for (const auto& allocation : entries_.at(entry_key).allocations) {
          auto& owner = owners_[allocation.owner];
          if (owner.refs++ == 0) {
            owner.bytes = allocation.bytes;
            bytes_ += owner.bytes;
          }
          ++registered;
        }
        candidates.push_back(manifest);
        if (!structured_identity_)
          structured_identity_ = std::move(identity);
        structured_metadata_ += manifest->metadata;
      } catch (...) {
        // Roll back only admitted owner references; no partial entry is usable.
        auto inserted = entries_.find(entry_key);
        if (inserted != entries_.end()) {
          for (std::size_t i = 0; i < registered; ++i) {
            const auto& allocation = inserted->second.allocations[i];
            auto owner = owners_.find(allocation.owner);
            if (owner != owners_.end() && owner->second.refs &&
                --owner->second.refs == 0) {
              bytes_ -= owner->second.bytes;
              owners_.erase(owner);
            }
          }
          retired.insert(entries_.extract(inserted));
        }
        lru_.pop_back();
        throw;
      }
    } catch (...) {
    }
  }
  void reclaim_capacity(const ResourceCapacity& requested) {
    decltype(entries_) retired;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !budget_->resources())
      return;
    auto fits = [&] {
      const auto available = budget_->resources()->available_capacity();
      for (std::size_t i = 0; i < requested.values.size(); ++i)
        if (requested.values[i] > available.values[i])
          return false;
      return true;
    };
    while (!entries_.empty() && !fits()) {
      retired.insert(evict());
      lock.unlock();
      retired.clear();
      lock.lock();
    }
  }
  Value get(const std::string& key, std::uint64_t expected_epoch = UINT64_MAX) {
    if (key.empty())
      return {};
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(key);
    if (found == entries_.end() || found->second.structured ||
        (expected_epoch != UINT64_MAX && expected_epoch != epoch_)) {
      ++stats_.misses;
      return {};
    }
    ++stats_.hits;
    lru_.splice(lru_.end(), lru_, found->second.order);
    return found->second.value;
  }
  /** @brief Optional retention is fenced; allocation failure cannot fail work.
   */
  void put(const std::string& key, const Value& value, std::uint64_t epoch,
           bool native = false) noexcept {
    // This optional LRU accounts sample allocations. Resource-bearing Values
    // remain owned by Runs/results until resource-aware cache admission exists.
    if (key.empty() || !value.valid() || value.resources().size())
      return;
    try {
      std::optional<ResourceAllocationScope> optional;
      ResourceLease metadata;
      if (budget_->resources()) {
        optional.emplace(*budget_->resources());
        std::uint64_t bytes = sizeof(Entry) + key.size() * 2 + 2;
        bytes += value.descriptor().shape.size() * sizeof(std::uint64_t) +
                 value.region().rank() * sizeof(RegionDimension) +
                 value.layout().origin.size() * sizeof(std::uint64_t) +
                 value.layout().byte_strides.size() * sizeof(std::int64_t) +
                 value.facets().size() * sizeof(ValueFacet);
        for (const auto& facet : value.facets())
          bytes += facet.key.size() + 1 + facet.payload.size();
        auto admitted =
            budget_->resources()->reserve(ResourceCapacity::host(bytes, bytes));
        if (!admitted.ok())
          return;
        metadata = admitted.take_value();
      }
      const auto owner = value.storage();
      const auto capacity = owner->capacity();
      if (capacity > limit_)
        return;
      decltype(entries_) retired;
      std::lock_guard<std::mutex> lock(mutex_);
      if (epoch != epoch_ || closing_ || entries_.count(key))
        return;
      while (!entries_.empty() &&
             (entries_.size() >= 4096 ||
              (!owners_.count(owner.get()) && bytes_ > limit_ - capacity)))
        retired.insert(evict());
      lru_.push_back(key);
      try {
        Entry entry;
        entry.value = value;
        entry.order = std::prev(lru_.end());
        entry.native = native;
        entry.lease = std::move(metadata);
        entries_.emplace(key, std::move(entry));
      } catch (...) {
        lru_.pop_back();
        throw;
      }
      try {
        auto& count = owners_[owner.get()];
        if (count.refs++ == 0) {
          count.bytes = capacity;
          bytes_ += capacity;
        }
      } catch (...) {
        entries_.erase(key);
        lru_.pop_back();
        throw;
      }
    } catch (...) {
    }
  }

 private:
  struct Entry {
    Value value;
    std::list<std::string>::iterator order;
    bool native = false;
    std::shared_ptr<const StructuredCacheManifest> structured;
    ResourceVector<ResultRef::CacheStorage> allocations;
    ResourceLease lease;
  };
  std::map<std::string, Entry>::node_type evict() {
    auto found = entries_.find(lru_.front());
    auto release = [&](const void* token) {
      auto count = owners_.find(token);
      if (--count->second.refs == 0) {
        bytes_ -= count->second.bytes;
        owners_.erase(count);
      }
    };
    if (found->second.structured) {
      structured_metadata_ -= found->second.structured->metadata;
      for (const auto& owner : found->second.allocations)
        release(owner.owner);
    } else {
      release(found->second.value.storage().get());
    }
    auto retired = entries_.extract(found);
    lru_.pop_front();
    ++stats_.evictions;
    return retired;
  }
  const std::uint64_t limit_;
  std::shared_ptr<data_internal::MemoryBudget> budget_;
  const std::uint64_t dependency_metadata_limit_;
  std::shared_ptr<const StructuredIdentity> structured_identity_;
  std::uint64_t structured_metadata_ = 0;
  using StructuredCandidates =
      ResourceVector<std::weak_ptr<const StructuredCacheManifest>>;
  using StructuredIndex = ResourceMap<StructuredCandidates>;
  StructuredIndex structured_manifests_;
  mutable std::mutex mutex_;
  bool closing_ = false;
  std::uint64_t bytes_ = 0, epoch_ = 0;
  ResultCacheStatistics stats_;
  std::map<std::string, Entry> entries_;
  std::list<std::string> lru_;
  struct OwnerCount {
    std::size_t refs = 0;
    std::uint64_t bytes = 0;
  };
  std::map<const void*, OwnerCount> owners_;
};
}  // namespace ps::execution_internal
