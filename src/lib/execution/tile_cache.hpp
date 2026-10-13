#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "execution/tile_records.hpp"

namespace ps::execution_internal {
struct TileCacheSource final {
  ResourceString name;
  bool whole = false;
  TileSet tiles;
};
// Each dependency keeps all matching active input ports. If an old shared
// producer maps to different current clones, the candidate is ambiguous and
// must miss rather than manufacture an actual Need on an arbitrary clone.
struct TileCacheRoute final {
  ResourceVector<std::uint32_t> ports;
  ResourceString contract, source;
};
struct TileCacheRecord final {
  std::shared_ptr<const TileRecord> record;
  ResourceString contract;
  ResourceVector<TileCacheRoute> routes;
};
struct ReboundTileCache final {
  explicit ReboundTileCache(const ResourceBudget& root)
      : task(root),
        ancestry(ResourceAllocator<std::shared_ptr<const TileRecord>>(root)) {}
  TileRecord task;
  TileRecords::Snapshot ancestry;
};
/** @brief Payload-free actual-Need closure with portable input-port routes.
 * The first record is the completed task. Remaining records are its producer
 * closure in discovery order. Capturing and rebinding use explicit Root and
 * optional-cache work admission; failures leave live execution tables alone.
 */
class TileCacheSnapshot final {
 public:
  explicit TileCacheSnapshot(const ResourceBudget& root)
      : records(ResourceAllocator<TileCacheRecord>(root)),
        sources(ResourceAllocator<TileCacheSource>(root)) {}
  static Result<std::shared_ptr<const TileCacheSnapshot>> capture(
      const ResourceBudget& root, const ExecutionPlan& plan,
      const std::vector<std::string>& contracts, const TileRecords& table,
      const TileRecord& task, std::uint64_t maximum_metadata,
      const std::function<Status(std::uint64_t)>& charge);
  Result<ReboundTileCache> rebind(
      const ResourceBudget& root, const ExecutionPlan& plan,
      const std::vector<std::string>& contracts, TileUnit task,
      const std::function<Status(std::uint64_t)>& charge) const;
  ResourceVector<TileCacheRecord> records;
  ResourceVector<TileCacheSource> sources;
  std::uint64_t metadata = 0;
};
}  // namespace ps::execution_internal
