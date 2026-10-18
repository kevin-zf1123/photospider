#include "execution/demand_query.hpp"

#include <utility>
#include <vector>

#include "photospider/core/checked_math.hpp"
#include "photospider/core/content_digest.hpp"
namespace ps::execution_internal {
Result<DemandKey> demand_key(const DemandQuery& query,
                             const ExecutionPlan& plan, std::uint64_t maximum,
                             const ResourceBudget& root) {
  auto charged = root.consume({query.size() + 1});
  if (!charged.ok())
    return Result<DemandKey>(charged);
  auto lease = root.reserve(ResourceCapacity::host(128, 128));
  if (!lease.ok())
    return Result<DemandKey>(lease.status());
  content_internal::Sha256 hash;
  hash.text("photospider.demand-query.tiles.v1");
  hash.integer(plan.tile_extent());
  hash.integer(query.size());
  std::uint64_t entries = 1;
  if (query.size() > maximum || !maximum)
    return Result<DemandKey>(Status{ErrorCode::ResourceExhausted, {}});
  for (const auto& item : query) {
    const auto found = plan.outputs().find(item.first);
    if (item.first.empty() || item.first.size() > 1024 ||
        found == plan.outputs().end())
      return Result<DemandKey>(
          Status{ErrorCode::InvalidArgument,
                 "unknown demand output",
                 FailureReason::None,
                 {FailureOrigin::Protocol, FailureScope::Group}});
    const auto& step = plan.output(item.first);
    const auto& allowed = plan.output_requests().at(item.first);
    if (!core_internal::can_add(item.second.interval_count(),
                                allowed.interval_count()) ||
        !core_internal::can_add(
            item.second.interval_count() + allowed.interval_count(),
            item.first.size() + 16))
      return Result<DemandKey>(Status{ErrorCode::ResourceExhausted, {}});
    charged = root.consume({item.second.interval_count() +
                            allowed.interval_count() + item.first.size() + 16});
    if (!charged.ok())
      return Result<DemandKey>(charged);
    auto request = item.second.resolve(step.tile_grids);
    if (!request.ok() ||
        !allowed.contains(request.ok() ? request.value() : TileRequest{}))
      return Result<DemandKey>(
          Status{ErrorCode::InvalidArgument,
                 "tile demand exceeds plan",
                 FailureReason::None,
                 {FailureOrigin::Protocol, FailureScope::Group}});
    const auto weight = 1 + request.value().interval_count();
    if (!core_internal::can_add(weight, entries, maximum))
      return Result<DemandKey>(
          Status{ErrorCode::ResourceExhausted, "demand query metadata limit"});
    entries += weight;
    hash.text(item.first);
    hash.integer(request.value().members.size());
    for (const auto& member : request.value().members) {
      hash.integer(member.first);
      hash.integer(member.second.intervals().size());
      for (const auto interval : member.second.intervals()) {
        hash.integer(interval.begin);
        hash.integer(interval.end);
      }
    }
  }
  return Result<DemandKey>(
      DemandKey{hash.finish(), entries, lease.take_value()});
}
Result<Footprint> demand_samples(const TileRequest& requested,
                                 const PlanOutput& step, std::uint32_t slot,
                                 const FootprintLimits& limits) {
  auto normalized = requested.resolve(step.tile_grids);
  if (!normalized.ok())
    return Result<Footprint>(normalized.status());
  if (slot >= step.tile_grids.size())
    return Result<Footprint>(Status{ErrorCode::InvalidArgument, {}});
  const auto found = normalized.value().members.find(slot);
  return step.tile_grids[slot].coverage(
      found == normalized.value().members.end() ? TileSet{} : found->second,
      limits);
}
}  // namespace ps::execution_internal
