#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "execution/dependency_records.hpp"

namespace ps::execution_internal {
/** @brief Completed state and structural provenance, with no worker ownership.
 */
struct CheckpointEntry final {
  DependencyCheckpoint checkpoint;
  std::vector<std::shared_ptr<const DependencyRecord>> upstream;
  std::uint64_t weight = 0;
};
/** @brief Bounded optional state shared only while matching Runs are active.
 * @note find never waits for work. Dropping optional entries cannot discard
 * already borrowed states; actual allocator leases govern their lifetime.
 */
class CheckpointScope final {
 public:
  explicit CheckpointScope(std::uint64_t maximum) : maximum_(maximum) {}
  std::shared_ptr<const CheckpointEntry> find(std::uint32_t phase,
                                              std::uint64_t before) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.upper_bound({phase, before});
    if (it == entries_.begin())
      return {};
    --it;
    if (it->first.first != phase)
      return {};
    return it->second;
  }
  bool put(CheckpointEntry entry) noexcept {
    try {
      std::array<std::shared_ptr<const CheckpointEntry>, 64> retired;
      std::size_t retired_count = 0;
      if (!entry.checkpoint.valid() || !entry.weight || entry.weight > maximum_)
        return false;
      const auto key =
          std::make_pair(entry.checkpoint.phase(), entry.checkpoint.sequence());
      const auto weight = entry.weight;
      auto stored = std::make_shared<const CheckpointEntry>(std::move(entry));
      std::lock_guard<std::mutex> lock(mutex_);
      if (entries_.count(key))
        return false;
      while (!entries_.empty() &&
             (entries_.size() >= retired.size() || used_ > maximum_ - weight)) {
        auto oldest = entries_.begin();
        retired[retired_count++] = std::move(oldest->second);
        used_ -= retired[retired_count - 1]->weight;
        entries_.erase(oldest);
      }
      entries_.emplace(key, stored);
      used_ += weight;
      return true;
    } catch (...) {
      // Optional metadata retention cannot fail the completed computation.
      return false;
    }
  }
  void clear() {
    std::map<std::pair<std::uint32_t, std::uint64_t>,
             std::shared_ptr<const CheckpointEntry>>
        retired;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.swap(retired);
    used_ = 0;
  }

 private:
  const std::uint64_t maximum_;
  std::uint64_t used_ = 0;
  std::mutex mutex_;
  std::map<std::pair<std::uint32_t, std::uint64_t>,
           std::shared_ptr<const CheckpointEntry>>
      entries_;
};
/** @brief Weak context directory; the last active Run releases its states. */
class DependencyCheckpoints final {
 public:
  explicit DependencyCheckpoints(std::uint64_t maximum) : maximum_(maximum) {}
  std::shared_ptr<CheckpointScope> acquire(const std::string& key,
                                           std::uint64_t metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = scopes_.find(key);
    if (found != scopes_.end())
      if (auto scope = found->second.lock())
        return scope;
    for (auto it = scopes_.begin(); it != scopes_.end();) {
      if (it->second.expired())
        it = scopes_.erase(it);
      else
        ++it;
    }
    if (scopes_.size() >= maximum_)
      return {};
    auto scope = std::make_shared<CheckpointScope>(metadata);
    scopes_[key] = scope;
    return scope;
  }
  void clear() {
    std::vector<std::shared_ptr<CheckpointScope>> active;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& entry : scopes_)
        if (auto scope = entry.second.lock())
          active.push_back(std::move(scope));
      scopes_.clear();
    }
    for (const auto& scope : active)
      scope->clear();
  }

 private:
  const std::uint64_t maximum_;
  std::mutex mutex_;
  std::map<std::string, std::weak_ptr<CheckpointScope>> scopes_;
};
}  // namespace ps::execution_internal
