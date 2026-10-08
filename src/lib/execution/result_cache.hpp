#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "execution/dependency_cache.hpp"
#include "execution/memory_budget.hpp"
#include "execution/structured_cache.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
/** @brief Tracks only opaque metadata that a generic Drop producer may publish.
 * @note Declarations and explicit semantic rules have known facets. Following
 * PreserveInput cannot introduce typed guarantees absent from the compiled IR.
 */
inline bool dynamic_opaque_output(const ExecutionPlan& plan,
                                  std::size_t index) {
  for (std::size_t remaining = plan.steps().size(); remaining; --remaining) {
    if (index >= plan.steps().size())
      return false;
    const auto& step = plan.steps()[index];
    const auto& traits = step.traits;
    if (traits.outputs[0].output_schema.kind != OperationPortKind::Value ||
        !step.output_facets.empty())
      return false;
    if (traits.outputs[0].output_semantic_rule == OperationSemanticRule::Drop)
      return true;
    if (traits.outputs[0].output_semantic_rule !=
            OperationSemanticRule::PreserveInput ||
        traits.outputs[0].output_semantic_input >= step.inputs.size())
      return false;
    const auto* producer = std::get_if<PlanStepInput>(
        &step.inputs[traits.outputs[0].output_semantic_input]);
    if (!producer)
      return false;
    index = producer->step_index;
  }
  return false;
}
/**
 * @brief Context-owned LRU and bounded coordinators for shared regional work.
 * @note Coordinators wait for the existing CPU callbacks, never occupy CPU
 * callback threads themselves. Destruction cancels and joins before CPU pools.
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
  ResultCache(std::uint64_t limit, std::shared_ptr<MemoryBudget> budget,
              std::size_t workers, std::size_t waiting,
              std::uint64_t dependency_metadata_limit = 65536)
      : limit_(limit),
        budget_(std::move(budget)),
        maximum_pending_(workers + waiting),
        dependency_metadata_limit_(dependency_metadata_limit),
        structured_manifests_(
            budget_->resources()
                ? make_resource_map<StructuredCandidates>(*budget_->resources())
                : StructuredIndex{}) {
    try {
      for (std::size_t i = 0; i < workers; ++i)
        workers_.emplace_back([this] { worker(); });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
      }
      changed_.notify_all();
      for (auto& t : workers_)
        t.join();
      throw;
    }
  }
  ~ResultCache() { close(); }
  void close() {
    std::lock_guard<std::mutex> closing(close_mutex_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closing_ = true;
      for (auto& item : flights_)
        item.second->cancellation.cancel();
    }
    changed_.notify_all();
    for (auto& t : workers_)
      if (t.joinable())
        t.join();
  }
  ResultCacheStatistics statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stats = stats_;
    stats.retained_bytes = bytes_;
    stats.entries = entries_.size();
    stats.in_flight = pending_;
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
    decltype(dependency_manifests_) proofs;
    auto structured =
        budget_->resources()
            ? make_resource_map<StructuredCandidates>(*budget_->resources())
            : StructuredIndex{};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++epoch_;
      stats_.evictions += entries_.size();
      retired.swap(entries_);
      proofs.swap(dependency_manifests_);
      structured.swap(structured_manifests_);
      lru_.clear();
      owners_.clear();
      bytes_ = dependency_metadata_ = structured_metadata_ = 0;
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
              structured_metadata_ + dependency_metadata_ >
                  dependency_metadata_limit_ - manifest->metadata)) {
        retired.insert(evict());
        fresh = 0;
        for (const auto& allocation : footprint.value())
          if (!owners_.count(allocation.owner))
            fresh += allocation.bytes;
      }
      if (structured_metadata_ + dependency_metadata_ >
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
  /** @brief Copies bounded proof references without pinning cached pixels. */
  std::vector<std::shared_ptr<const DependencyCacheManifest>>
  dependency_candidates(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = dependency_manifests_.find(key);
    return found == dependency_manifests_.end()
               ? std::vector<std::shared_ptr<const DependencyCacheManifest>>{}
               : found->second;
  }
  /** @brief Atomically acquires every fragment after external proof validation.
   * @note Missing pixels or a changed epoch are a miss, never partial success.
   */
  std::vector<Value> dependency_values(
      const DependencyCacheManifest& manifest) {
    std::vector<Value> result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (manifest.epoch != epoch_ || closing_) {
      ++stats_.misses;
      return result;
    }
    for (const auto& key : manifest.fragment_keys)
      if (!entries_.count(key)) {
        ++stats_.misses;
        return result;
      }
    for (const auto& key : manifest.fragment_keys) {
      auto& entry = entries_.at(key);
      result.push_back(entry.value);
      lru_.splice(lru_.end(), lru_, entry.order);
    }
    ++stats_.hits;
    return result;
  }
  /** @brief Installs a complete proof only when all ordinary LRU pixels exist.
   * @note Optional retention failure never fails the completed computation.
   * Caller values must already belong to this context's accounted allocator.
   */
  void put_dependency(const std::string& key,
                      std::shared_ptr<const DependencyCacheManifest> manifest,
                      const std::vector<Value>& values,
                      const std::vector<bool>& native = {}) noexcept {
    try {
      if ((!native.empty() && native.size() != values.size()) || !manifest ||
          manifest->fragment_keys.size() != values.size() || values.empty() ||
          values.size() > 4096 || !manifest->metadata_entries ||
          manifest->metadata_entries > dependency_metadata_limit_)
        return;
      BufferAllocator domain({}, budget_);
      std::set<const CpuStorage*> owners;
      std::uint64_t capacity = 0;
      for (const auto& value : values) {
        if (!value.valid() || value.resources().size() ||
            !domain.owns(*value.storage()))
          return;
        if (owners.insert(value.storage().get()).second) {
          const auto bytes = value.storage()->capacity();
          if (bytes > limit_ || capacity > limit_ - bytes)
            return;
          capacity += bytes;
        }
      }
      for (std::size_t i = 0; i < values.size(); ++i)
        put(manifest->fragment_keys[i], values[i], manifest->epoch,
            !native.empty() && native[i]);
      std::lock_guard<std::mutex> lock(mutex_);
      if (manifest->epoch != epoch_ || closing_)
        return;
      for (const auto& pixel : manifest->fragment_keys)
        if (!entries_.count(pixel))
          return;
      auto prior = dependency_manifests_.find(key);
      if (prior != dependency_manifests_.end())
        for (const auto& candidate : prior->second)
          if (candidate->content_identity == manifest->content_identity &&
              candidate->fragment_keys == manifest->fragment_keys)
            return;
      while (!dependency_manifests_.empty() &&
             dependency_metadata_ + structured_metadata_ >
                 dependency_metadata_limit_ - manifest->metadata_entries) {
        auto oldest = dependency_manifests_.begin();
        for (const auto& removed : oldest->second)
          dependency_metadata_ -= removed->metadata_entries;
        dependency_manifests_.erase(oldest);
      }
      if (dependency_metadata_ + structured_metadata_ >
          dependency_metadata_limit_ - manifest->metadata_entries)
        return;
      const auto found = dependency_manifests_.find(key);
      auto candidates =
          found == dependency_manifests_.end()
              ? std::vector<std::shared_ptr<const DependencyCacheManifest>>{}
              : found->second;
      std::uint64_t removed = 0;
      if (candidates.size() == 8) {
        removed = candidates.front()->metadata_entries;
        candidates.erase(candidates.begin());
      }
      candidates.push_back(std::move(manifest));
      const auto added = candidates.back()->metadata_entries;
      dependency_manifests_.insert_or_assign(key, std::move(candidates));
      dependency_metadata_ = dependency_metadata_ - removed + added;
    } catch (...) {
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
  /** @brief Revalidates a completed result against its resolved output
   * contract.
   * @note Generic Drop outputs retain their existing opaque-facet behavior.
   * Typed sample failures remain computed OperationFailed errors on hits.
   */
  Result<Value> get_output(const std::string& key, const PlanStep& step,
                           const std::function<ErrorCode()>& stop,
                           bool dynamic_opaque = false) {
    if (stop) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return Result<Value>(Status::failure(code, "cached output stopped"));
    }
    auto value = get(key);
    if (!value.valid())
      return Result<Value>(Value{});
    const bool allow_opaque =
        dynamic_opaque && step.output_facets.empty() &&
        step.traits.outputs[0].output_schema.kind == OperationPortKind::Value;
    const bool exact =
        !allow_opaque && (step.traits.outputs[0].output_schema.kind !=
                              OperationPortKind::Value ||
                          step.traits.outputs[0].output_semantic_rule !=
                              OperationSemanticRule::Drop);
    const bool facets_match =
        exact ? value.facets().size() == step.output_facets.size() &&
                    std::equal(value.facets().begin(), value.facets().end(),
                               step.output_facets.begin(),
                               [](const auto& a, const auto& b) {
                                 return a.key == b.key &&
                                        a.version == b.version &&
                                        a.payload == b.payload;
                               })
              : std::none_of(value.facets().begin(), value.facets().end(),
                             [](const auto& f) {
                               return input_internal::typed_facet(f.key);
                             });
    if (value.descriptor().element_type !=
            step.output_descriptor.element_type ||
        value.descriptor().shape != step.output_descriptor.shape ||
        !value.view(step.output_demand).ok() || !facets_match)
      return Result<Value>(Status::failure(ErrorCode::OperationFailed,
                                           "cached output contract mismatch"));
    auto status = input_internal::validate_port_value(
        {}, value, ErrorCode::OperationFailed, stop);
    if (!status.ok())
      return Result<Value>(status);
    return Result<Value>(std::move(value));
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
  /** @brief Reclaims cache references before strict working-set admission. */
  void reclaim_for(std::uint64_t requested, bool native = false) {
    decltype(entries_) retired;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!entries_.empty() && !budget_->can_allocate(requested, native)) {
      retired.insert(evict());
      lock.unlock();
      retired.clear();
      lock.lock();
    }
  }
  using Work = std::function<Result<ExecutionResult>(const CancellationToken&)>;
  Result<ExecutionResult> compute(const std::string& key,
                                  const std::function<ErrorCode()>& stop,
                                  Work work) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::shared_ptr<Flight> flight;
    bool shared = false;
    for (;;) {
      const auto code = stop();
      if (code != ErrorCode::Ok)
        return Result<ExecutionResult>(
            Status::failure(code, "shared waiter stopped"));
      if (closing_)
        return Result<ExecutionResult>(
            Status::failure(ErrorCode::Cancelled, "cache closing"));
      auto found = flights_.find(key);
      if (found != flights_.end() &&
          !found->second->cancellation.token().cancelled()) {
        flight = found->second;
        shared = true;
        ++stats_.shared_computations;
        break;
      }
      if (pending_ < maximum_pending_) {
        flight = std::make_shared<Flight>();
        flight->work = std::move(work);
        flight->key = key;
        queue_.push_back(flight);
        try {
          flights_.insert_or_assign(key, flight);
        } catch (...) {
          queue_.pop_back();
          throw;
        }
        ++pending_;
        changed_.notify_all();
        break;
      }
      changed_.wait_for(lock, std::chrono::milliseconds(2));
    }
    ++flight->subscribers;
    struct Subscriber {
      Flight* flight;
      ~Subscriber() {
        if (--flight->subscribers == 0 && !flight->done)
          flight->cancellation.cancel();
      }
    } subscriber{flight.get()};
    while (!flight->done) {
      const auto code = stop();
      if (code != ErrorCode::Ok) {
        if (flight->subscribers == 1) {
          flight->cancellation.cancel();
          while (!flight->done)
            changed_.wait(lock);
        }
        return Result<ExecutionResult>(
            Status::failure(code, "shared waiter stopped"));
      }
      changed_.wait_for(lock, std::chrono::milliseconds(2));
    }
    const auto code = stop();
    if (code != ErrorCode::Ok)
      return Result<ExecutionResult>(
          Status::failure(code, "shared publication stopped"));
    if (!flight->result->ok())
      return Result<ExecutionResult>(flight->result->status());
    auto result = flight->result->value();
    if (shared) {
      result.diagnostics.operation_timings.clear();
      result.diagnostics.native_dispatch_count = 0;
      result.diagnostics.native_submission_count = 0;
      result.diagnostics.native_compute_us = 0;
      result.diagnostics.native_constant_bytes = 0;
      result.diagnostics.transfer_count = 0;
      result.diagnostics.transfer_bytes = 0;
      result.diagnostics.host_access_count = 0;
      result.diagnostics.result_copy_bytes = 0;
      result.diagnostics.native_upload_hits = 0;
      result.diagnostics.source_read_count = 0;
      result.diagnostics.source_read_bytes = 0;
      result.diagnostics.shared_computations = 1;
    }
    return Result<ExecutionResult>(std::move(result));
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
  struct Flight {
    std::string key;
    Work work;
    CancellationSource cancellation;
    std::size_t subscribers = 0;
    bool done = false;
    std::optional<Result<ExecutionResult>> result;
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
  void worker() noexcept {
    for (;;) {
      std::shared_ptr<Flight> flight;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closing_ || !queue_.empty(); });
        if (queue_.empty())
          return;
        flight = queue_.front();
        queue_.pop_front();
      }
      std::optional<Result<ExecutionResult>> result;
      try {
        result.emplace(flight->work(flight->cancellation.token()));
      } catch (...) {
        Status failure;
        failure.code = ErrorCode::OperationFailed;
        result.emplace(std::move(failure));
      }
      flight->work = {};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        flight->result = std::move(result);
        flight->done = true;
        auto found = flights_.find(flight->key);
        if (found != flights_.end() && found->second == flight)
          flights_.erase(found);
        --pending_;
      }
      changed_.notify_all();
    }
  }
  const std::uint64_t limit_;
  std::shared_ptr<MemoryBudget> budget_;
  const std::size_t maximum_pending_;
  const std::uint64_t dependency_metadata_limit_;
  std::shared_ptr<const StructuredIdentity> structured_identity_;
  std::uint64_t dependency_metadata_ = 0, structured_metadata_ = 0;
  std::map<std::string,
           std::vector<std::shared_ptr<const DependencyCacheManifest>>>
      dependency_manifests_;
  using StructuredCandidates =
      ResourceVector<std::weak_ptr<const StructuredCacheManifest>>;
  using StructuredIndex = ResourceMap<StructuredCandidates>;
  StructuredIndex structured_manifests_;
  std::mutex close_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool closing_ = false;
  std::uint64_t bytes_ = 0, epoch_ = 0;
  std::size_t pending_ = 0;
  ResultCacheStatistics stats_;
  std::map<std::string, Entry> entries_;
  std::list<std::string> lru_;
  struct OwnerCount {
    std::size_t refs = 0;
    std::uint64_t bytes = 0;
  };
  std::map<const void*, OwnerCount> owners_;
  std::map<std::string, std::shared_ptr<Flight>> flights_;
  std::deque<std::shared_ptr<Flight>> queue_;
  std::vector<std::thread> workers_;
};
}  // namespace ps::execution_internal
