#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "photospider/data/value.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps::execution_internal {
class NativeRunUploads;
// Context-local reclamation directory. Registration is intrusive, so no
// allocation occurs while its lock is held. It owns neither Runs nor sources.
class NativeUploadRegistry final {
 public:
  explicit NativeUploadRegistry(ResourceBudget resources)
      : resources_(std::move(resources)) {}
  void reclaim_capacity(const ResourceCapacity& requested);

 private:
  friend class NativeRunUploads;
  ResourceBudget resources_;
  std::mutex mutex_;
  NativeRunUploads* first_ = nullptr;
};

// Optional Run-local physical backing reuse. Source proof is weak: a live
// immutable owner proves pointer identity without extending its lifetime.
// Entries contain no source Result, facets, resources or association owners.
class NativeRunUploads final {
 public:
  NativeRunUploads(std::shared_ptr<NativeUploadRegistry> registry,
                   ResourceBudget resources)
      : registry_(std::move(registry)), resources_(std::move(resources)) {
    std::lock_guard<std::mutex> lock(registry_->mutex_);
    next_ = registry_->first_;
    if (next_)
      next_->previous_ = this;
    registry_->first_ = this;
  }
  ~NativeRunUploads() {
    {
      std::lock_guard<std::mutex> lock(registry_->mutex_);
      if (previous_)
        previous_->next_ = next_;
      else
        registry_->first_ = next_;
      if (next_)
        next_->previous_ = previous_;
    }
    clear();
  }
  NativeRunUploads(const NativeRunUploads&) = delete;
  NativeRunUploads& operator=(const NativeRunUploads&) = delete;
  std::uint64_t lookup_work() {
    std::lock_guard<std::mutex> lock(mutex_);
    return 1 + entries_;
  }
  std::shared_ptr<const CpuStorage> find(
      std::string_view key, const std::shared_ptr<const CpuStorage>& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto item = first_; item; item = item->next)
      if (std::string_view(item->key) == key) {
        const auto proof = item->source.lock();
        if (proof && proof.get() == source.get())
          return item->backing;
      }
    return {};
  }
  void put(std::string_view key,
           const std::shared_ptr<const CpuStorage>& source,
           std::shared_ptr<const CpuStorage> backing) noexcept {
    try {
      // Optional indexing must not poison a callback's metadata failure fence.
      ResourceAllocationScope optional(resources_);
      auto item = std::allocate_shared<Entry>(
          ResourceAllocator<Entry>(resources_), resources_, key, source,
          std::move(backing));
      std::shared_ptr<Entry> retired;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_ >= 256) {
          retired = std::move(first_);
          entries_ = 0;
        }
        item->next = std::move(first_);
        first_ = std::move(item);
        ++entries_;
      }
    } catch (...) {
    }
  }
  void clear() {
    std::shared_ptr<Entry> retired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      retired = std::move(first_);
      entries_ = 0;
    }
  }

 private:
  struct Entry {
    Entry(const ResourceBudget& resources, std::string_view view,
          std::weak_ptr<const CpuStorage> proof,
          std::shared_ptr<const CpuStorage> native)
        : key(view.data(), view.size(), ResourceAllocator<char>(resources)),
          source(std::move(proof)),
          backing(std::move(native)) {}
    ResourceString key;
    std::weak_ptr<const CpuStorage> source;
    std::shared_ptr<const CpuStorage> backing;
    std::shared_ptr<Entry> next;
  };
  friend class NativeUploadRegistry;
  std::shared_ptr<NativeUploadRegistry> registry_;
  ResourceBudget resources_;
  NativeRunUploads *previous_ = nullptr, *next_ = nullptr;
  std::mutex mutex_;
  std::shared_ptr<Entry> first_;
  std::uint32_t entries_ = 0;
};
inline void NativeUploadRegistry::reclaim_capacity(
    const ResourceCapacity& requested) {
  auto fits = [&] {
    const auto available = resources_.available_capacity();
    for (std::size_t i = 0; i < requested.values.size(); ++i)
      if (requested.values[i] > available.values[i])
        return false;
    return true;
  };
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return;
  for (auto* run = first_; run && !fits(); run = run->next_)
    run->clear();
}
}  // namespace ps::execution_internal
