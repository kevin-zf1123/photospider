#pragma once

#include <memory>

#include "photospider/execution/tile_dependencies.hpp"

namespace ps {
namespace execution_internal {
class TileRecords;
class StructuredExecution;
}  // namespace execution_internal
/** @brief Immutable actual-Need records of one plan and binding generation.
 * Records retain Root metadata and no payload owners. Const queries are safe
 * concurrently; allocation, work and cancellation failures return no partial
 * answer. Storage and published data may retire independently.
 */
class PHOTOSPIDER_API ExecutionDependencies final {
 public:
  ExecutionDependencies() = default;
  /** @brief Actual normalized Needs of a completed computation unit.
   * The shared immutable record retains Root metadata, never payload storage.
   * Returns an empty owner for an uncomputed unit; lookup charges Root work.
   */
  Result<std::shared_ptr<const TileRecord>> tile_record(TileUnit unit) const;
  /** @brief Returns each recorded unit reachable from changed producer units.
   * Descriptor changes traverse all dependencies on that producer. Payload
   * changes traverse only Whole or matching tile dependencies. Root limits
   * bound sparse traversal, and failure returns no partial answer.
   */
  Result<ResourceVector<TileUnit>> dirty_units(
      const ResourceVector<TileChange>& changes) const;
  bool valid() const noexcept { return tiles_ != nullptr; }

 private:
  friend class DemandHandle;
  friend class execution_internal::StructuredExecution;
  std::shared_ptr<const execution_internal::TileRecords> tiles_;
};
}  // namespace ps
