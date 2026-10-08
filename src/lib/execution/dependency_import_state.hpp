#pragma once

#include <set>
#include <string_view>
#include <utility>

#include "photospider/core/resource_allocator.hpp"

namespace ps::execution_internal {
// Owns only Root-accounted imported identities. Copies used for a backend retry
// retain independent mutable sets; immutable captured records remain shared.
class DependencyImportState final {
 public:
  const auto& identities() const noexcept { return identities_; }
  std::size_t size() const noexcept { return identities_.size(); }
  bool contains(std::string_view identity) const {
    return identities_.count(identity) != 0;
  }
  void remember(ResourceString identity) {
    identities_.insert(std::move(identity));
  }
  void clear() noexcept { identities_.clear(); }

 private:
  std::set<ResourceString, ResourceStringLess,
           ResourceAllocator<ResourceString>>
      identities_;
};
}  // namespace ps::execution_internal
