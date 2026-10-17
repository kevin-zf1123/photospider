#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "execution/tile_records.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
struct ResultCheckpointWitness final {
  explicit ResultCheckpointWitness(const ResourceBudget& root)
      : scope(ResourceAllocator<char>(root)),
        tiles(root),
        tile_ancestry(
            ResourceAllocator<std::shared_ptr<const TileRecord>>(root)) {}
  ResourceString scope;
  TileRecord tiles;
  TileRecords::Snapshot tile_ancestry;
  std::uint64_t weight = 0;
};
class ResultCheckpointScope final {
 public:
  ResultCheckpointScope(const ResourceBudget& root, std::uint64_t maximum)
      : maximum_(maximum),
        entries_(std::less<Key>{}, ResourceAllocator<Map::value_type>(root)) {}
  ResultCheckpoint find(std::uint32_t phase, std::uint64_t before) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.upper_bound({phase, before});
    if (found == entries_.begin())
      return {};
    --found;
    return found->first.first == phase ? found->second.first
                                       : ResultCheckpoint{};
  }
  bool put(const ResultCheckpoint& checkpoint, std::uint64_t weight) {
    if (!checkpoint.valid() || !weight || weight > maximum_)
      return false;
    std::array<ResultCheckpoint, 64> retired;
    std::size_t retired_count = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const Key key{checkpoint.phase(), checkpoint.sequence()};
    if (entries_.count(key))
      return false;
    while (!entries_.empty() &&
           (entries_.size() >= 64 ||
            !core_internal::can_add(used_, weight, maximum_))) {
      auto oldest = entries_.begin();
      used_ -= oldest->second.second;
      retired[retired_count++] = std::move(oldest->second.first);
      entries_.erase(oldest);
    }
    entries_.emplace(key, std::make_pair(checkpoint, weight));
    used_ += weight;
    return true;
  }
  void clear() {
    Map retired(std::less<Key>{}, entries_.get_allocator());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      retired.swap(entries_);
      used_ = 0;
    }
  }

 private:
  using Key = std::pair<std::uint32_t, std::uint64_t>;
  using Map =
      std::map<Key, std::pair<ResultCheckpoint, std::uint64_t>, std::less<Key>,
               ResourceAllocator<std::pair<
                   const Key, std::pair<ResultCheckpoint, std::uint64_t>>>>;
  std::uint64_t maximum_, used_ = 0;
  std::mutex mutex_;
  Map entries_;
};
class ResultCheckpoints final {
 public:
  explicit ResultCheckpoints(std::uint64_t maximum) : maximum_(maximum) {}
  std::shared_ptr<ResultCheckpointScope> acquire(const ResourceString& key,
                                                 const ResourceBudget& root,
                                                 std::uint64_t metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!scopes_)
      scopes_ = make_resource_map<std::weak_ptr<ResultCheckpointScope>>(root);
    auto found = scopes_->find(key);
    if (found != scopes_->end())
      if (auto scope = found->second.lock())
        return scope;
    for (auto it = scopes_->begin(); it != scopes_->end();) {
      if (it->second.expired())
        it = scopes_->erase(it);
      else
        ++it;
    }
    if (scopes_->size() >= maximum_)
      return {};
    auto scope = std::allocate_shared<ResultCheckpointScope>(
        ResourceAllocator<ResultCheckpointScope>(root), root, metadata);
    scopes_->emplace(key, scope);
    return scope;
  }
  void clear() {
    decltype(scopes_) retired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      retired = std::move(scopes_);
      scopes_.reset();
    }
    if (retired)
      for (const auto& entry : *retired)
        if (auto scope = entry.second.lock())
          scope->clear();
  }

 private:
  std::uint64_t maximum_;
  std::mutex mutex_;
  std::optional<ResourceMap<std::weak_ptr<ResultCheckpointScope>>> scopes_;
};
}  // namespace ps::execution_internal
