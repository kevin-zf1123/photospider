#pragma once

#include <array>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

#include "photospider/compiler/plan.hpp"

namespace ps::execution_internal {
// Context-local immutable Host backings. Keys include the declared dependency
// closure, so independently compiled Jobs do not need to share plan IDs.
using TileKey = std::array<std::uint8_t, 32>;
using CheckReuse = std::function<void()>;
class TileCache final {
 public:
  using Owner = std::shared_ptr<const CpuStorage>;
  TileCache(const ResourceBudget& budget, std::uint64_t limit);
  std::uint64_t limit() const noexcept { return limit_; }
  TileKey input_key(const Owner& owner, std::uint64_t offset,
                    std::uint64_t bytes, const CheckReuse& check);
  Owner find(const TileKey& key);
  void store(const TileKey& key, const Owner& owner);
  bool clear();

 private:
  struct Entry {
    TileKey key;
    Owner owner;
    std::uint64_t charge;
  };
  std::uint64_t limit_, used_ = 0;
  std::mutex mutex_;
  using List = std::list<Entry, ResourceAllocator<Entry>>;
  template <class K, class V>
  using Map =
      std::map<K, V, std::less<K>, ResourceAllocator<std::pair<const K, V>>>;
  List recent_;
  Map<TileKey, List::iterator> index_;
  using Slice = std::tuple<std::uintptr_t, std::uint64_t, std::uint64_t>;
  struct Input {
    std::weak_ptr<const CpuStorage> owner;
    TileKey digest;
  };
  Map<Slice, Input> inputs_;
};

// Returns per-tile dependency keys. An empty key propagates through tasks
// whose producer or input cannot be proven reusable.
ResourceVector<TileKey> tile_keys(
    const ExecutionPlan& plan, physical::Id block,
    const std::function<TileKey(physical::Id)>& input_key,
    const CheckReuse& check);
}  // namespace ps::execution_internal
