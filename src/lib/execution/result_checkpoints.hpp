#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "execution/dependency_records.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps::execution_internal {
// NOLINTBEGIN(whitespace/indent_namespace)
using ResultNeedKey = std::tuple<std::uint32_t, ResultSupportTarget,
                                 std::uint32_t, std::uint32_t>;
using ResultNeedHistory =
    std::map<ResultNeedKey, Footprint, std::less<ResultNeedKey>,
             ResourceAllocator<std::pair<const ResultNeedKey, Footprint>>>;
// Keep every observed identity, with one admitted allocation per port's
// growing sequence instead of one allocator header per identity. Object IDs
// usually arrive in increasing order; checkpoint restoration also permits
// insertion of an older ID without changing the port/ID iteration order.
class ResultInputFacts final {
 public:
  using Key = std::pair<std::uint32_t, std::uint64_t>;
  using value_type = std::pair<Key, std::uint64_t>;
  using Entry = std::pair<std::uint64_t, std::uint64_t>;
  struct Port {
    Port(std::uint32_t index, ResourceAllocator<Entry> allocator)
        : input(index), facts(allocator) {}
    std::uint32_t input;
    ResourceVector<Entry> facts;
  };
  class ConstIterator {
   public:
    ConstIterator(const Port* port, const Port* end, std::size_t offset = 0)
        : port_(port), end_(end), offset_(offset) {
      skip_empty();
    }
    value_type operator*() const {
      const auto& entry = port_->facts[offset_];
      return {{port_->input, entry.first}, entry.second};
    }
    ConstIterator& operator++() {
      if (++offset_ == port_->facts.size()) {
        ++port_;
        offset_ = 0;
      }
      skip_empty();
      return *this;
    }
    bool operator!=(const ConstIterator& other) const {
      return port_ != other.port_ || offset_ != other.offset_;
    }

   private:
    void skip_empty() {
      while (port_ != end_ && port_->facts.empty())
        ++port_;
    }
    const Port* port_;
    const Port* end_;
    std::size_t offset_;
  };
  ResultInputFacts() = default;
  ResultInputFacts(const ResultInputFacts&) = default;
  ResultInputFacts& operator=(const ResultInputFacts&) = default;
  ResultInputFacts(ResultInputFacts&& other) noexcept
      : ports_(std::move(other.ports_)), size_(std::exchange(other.size_, 0)) {}
  ResultInputFacts& operator=(ResultInputFacts&& other) noexcept {
    if (this != &other) {
      ports_ = std::move(other.ports_);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }
  explicit ResultInputFacts(const ResourceBudget& root)
      : ports_(ResourceAllocator<Port>(root)) {}
  std::uint64_t& operator[](Key key) {
    auto port = std::lower_bound(
        ports_.begin(), ports_.end(), key.first,
        [](const Port& entry, auto input) { return entry.input < input; });
    if (port == ports_.end() || port->input != key.first)
      port = ports_.insert(
          port,
          Port(key.first, ResourceAllocator<Entry>(ports_.get_allocator())));
    auto& entries = port->facts;
    auto found = std::lower_bound(
        entries.begin(), entries.end(), key.second,
        [](const Entry& entry, auto id) { return entry.first < id; });
    if (found == entries.end() || found->first != key.second) {
      found = entries.insert(found, {key.second, 0});
      ++size_;
    }
    return found->second;
  }
  std::size_t size() const { return size_; }
  void clear() {
    ports_.clear();
    size_ = 0;
  }
  ConstIterator begin() const {
    return {ports_.data(),
            ports_.empty() ? ports_.data() : ports_.data() + ports_.size()};
  }
  ConstIterator end() const {
    const auto* last =
        ports_.empty() ? ports_.data() : ports_.data() + ports_.size();
    return {last, last};
  }

 private:
  ResourceVector<Port> ports_;
  std::size_t size_ = 0;
};
// NOLINTEND
struct ResultCheckpointWitness final {
  explicit ResultCheckpointWitness(const ResourceBudget& root)
      : scope(ResourceAllocator<char>(root)),
        needs(std::less<ResultNeedKey>{},
              ResourceAllocator<ResultNeedHistory::value_type>(root)),
        facts(root),
        ancestry(ResourceAllocator<decltype(ancestry)::value_type>(root)) {}
  ResourceString scope;
  ResultNeedHistory needs;
  ResultInputFacts facts;
  ResourceVector<
      std::pair<std::uint32_t, std::shared_ptr<const DependencyBundle>>>
      ancestry;
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
           (entries_.size() >= 64 || used_ > maximum_ - weight)) {
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
