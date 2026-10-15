#pragma once

#include <utility>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/resource_set.hpp"

namespace ps::plugin_internal {
/** @brief Resolve only declared input/output identities before callbacks. */
inline Result<ResourceSet> admit_operation_resources(
    const ResourceSet& supplied,
    const std::vector<ResourceIdentity>& required) {
  auto selected = supplied.select(required);
  if (!selected.ok())
    return selected;
  if (const auto* root = resource_internal::metadata_budget())
    return selected.value().reference(*root);
  return selected;
}
}  // namespace ps::plugin_internal
