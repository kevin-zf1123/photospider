#pragma once

#include <memory>

#include "photospider/core/resources.hpp"

namespace ps::data_internal {
/** @brief Retains a physical owner under one root's Referenced limit.
 * identity is owner.get(), independent of an alias's control block.
 * owned must describe actual payload reservation provenance.
 */
class ResourceStorageAccess final {
 public:
  static Result<std::shared_ptr<const void>> reference(
      const ResourceBudget& budget, std::shared_ptr<const void> owner,
      std::uint64_t capacity, bool owned);
};
}  // namespace ps::data_internal
