#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/physical_program.hpp"
#include "photospider/data/tiles.hpp"
#include "photospider/plugin/operation_types.hpp"

namespace ps {
struct OperationLowering;

/** @brief Compile-time implementation dependency, excluded from the operation
 * semantic contract. Aliases are unique within the operation declaration.
 */
struct MapperRequirement final {
  std::string alias, contract, provider;
  std::uint32_t version = 1, revision = 1;
};
/** @brief A source slot can name an input or a private stage. Slots preserve
 * invocation order, including distinct semantic reads of one allocation.
 */
struct MappingSource final {
  std::uint32_t input = 0, member = 0;
  TileGrid grid;
  OperationMetadata metadata;
};
struct MappingQuery final {
  std::uint32_t output = 0, member = 0;
  TileIndex tile = 0;
  std::uint64_t tile_extent = 128;
  Region region;
  std::vector<MappingSource> sources;
  std::vector<std::uint64_t> config;
  OperationMetadata output_metadata;
};
/** @brief Synchronous callback-local collector. The first rejected request
 * rejects the whole query even when a solver ignores the return status.
 */
class PHOTOSPIDER_API MappingSink final {
 public:
  MappingSink(const MappingQuery& query, const ResourceBudget& budget,
              const physical::ValidationLimits& limits);
  MappingSink(MappingQuery&&, const ResourceBudget&,
              const physical::ValidationLimits&) = delete;
  Status tiles(std::uint32_t slot, TileSet set);
  Status region(std::uint32_t slot, const Region& region);
  Status regions(std::uint32_t slot, const std::vector<Region>& regions);
  Status interval(std::uint32_t slot, TileIndex begin, TileIndex end);
  Status descriptor(std::uint32_t slot);
  Status consume(std::uint64_t work);
  const Status& status() const { return status_; }
  std::vector<TileNeed> take() { return std::move(needs_); }

 private:
  Status remember(Status status);
  const MappingQuery& query_;
  ResourceBudget budget_;
  physical::ValidationLimits limits_;
  std::uint64_t edges_ = 0;
  Status status_;
  std::vector<TileNeed> needs_;
};
using MappingSolver = std::function<Status(const MappingQuery&, MappingSink&)>;
struct MapperDefinition final {
  std::string contract, provider;
  std::uint32_t version = 1, revision = 1;
  MappingSolver solve;
};
using MapperKey = std::pair<std::string, std::string>;
using MapperHandle = std::shared_ptr<const MapperDefinition>;
using MapperCatalog = std::map<MapperKey, MapperHandle>;
/** @brief Planning-local cumulative work budget shared by repeated lowerings.
 */
class PHOTOSPIDER_API LoweringSession final {
 public:
  explicit LoweringSession(const physical::ValidationLimits& limits);
  const ResourceBudget& budget() const noexcept { return budget_; }

  /** @brief Reuse an identical external query within this planning session.
   * Cache keys include the module's contract, revision, schemas, grids,
   * selection and configuration. Failed queries are never retained. */
  Result<std::vector<TileNeed>> solve(const MapperDefinition& mapper,
                                      const MappingQuery& query,
                                      const physical::ValidationLimits& limits);

 private:
  struct Cached {
    std::string key;
    std::vector<TileNeed> needs;
  };
  std::mutex mutex_;
  std::optional<Cached> previous_;
  ResourceBudget budget_;
};
struct TileTask final {
  std::string symbol, contract;
  physical::Placement placement;
  std::array<std::uint64_t, 3> scratch{};
  std::vector<std::uint8_t> parameters;
  std::uint32_t invocation_abi = 8;
};
/** @brief One producer per requested output member tile. Omitting local and
 * external selects logical-coordinate direct mapping of the projected inputs.
 */
struct TilewiseLowering final {
  std::function<Result<TileTask>(const OperationLowering&, const MappingQuery&)>
      describe;
  MappingSolver local;
  std::string external;
  std::function<Result<std::vector<std::uint64_t>>(const OperationLowering&)>
      configuration;
};
}  // namespace ps
