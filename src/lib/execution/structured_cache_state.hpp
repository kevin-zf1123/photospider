#pragma once

#include <cstdint>
#include <memory>

#include "execution/result_cache.hpp"
#include "execution/result_checkpoints.hpp"

namespace ps::execution_internal {
// Owns the exact replay transcript and its optional-reuse admission flag.
// The coordinator supplies Needs and clears these owners on retirement/retry.
struct StructuredActorCache final {
  explicit StructuredActorCache(const ResourceBudget& root)
      : replay(ResourceAllocator<StructuredCacheNeed>(root)) {}
  ResourceVector<StructuredCacheNeed> replay;
  bool disabled = false;
};

// Owns the optional cache quota and this Run's acquired checkpoint scopes.
// Root is borrowed from the coordinator, which outlives this component. Scope
// owners retain their own Root leases. Calls borrow the coordinator's callback
// metadata gate; no callback or table pointer escapes a call. Charge fails
// before changing quota, and disabling optional reuse never refunds Root work.
class StructuredCacheState final {
 public:
  StructuredCacheState(const ResourceBudget& root, std::uint64_t work)
      : root_(root),
        remaining_(work),
        scopes_(
            make_resource_map<std::shared_ptr<ResultCheckpointScope>>(root)) {}
  std::uint64_t remaining() const noexcept { return remaining_; }
  void disable() noexcept { remaining_ = 0; }
  Status charge(std::uint64_t units);
  std::shared_ptr<ResultCheckpointScope> scope(const ResourceString& key,
                                               ResultCheckpoints* table,
                                               std::uint64_t maximum);

 private:
  const ResourceBudget& root_;
  std::uint64_t remaining_;
  ResourceMap<std::shared_ptr<ResultCheckpointScope>> scopes_;
};
}  // namespace ps::execution_internal
