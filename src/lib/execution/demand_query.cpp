#include "execution/demand_query.hpp"

#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "data/content_digest.hpp"
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
    const auto& step = plan.steps()[found->second];
    const auto& allowed = plan.output_requests().at(item.first);
    if (!core_internal::can_add(item.second.tiles.size(),
                                allowed.tiles.size()) ||
        !core_internal::can_add(item.second.tiles.size() + allowed.tiles.size(),
                                item.first.size() + 16))
      return Result<DemandKey>(Status{ErrorCode::ResourceExhausted, {}});
    charged = root.consume({item.second.tiles.size() + allowed.tiles.size() +
                            item.first.size() + 16});
    if (!charged.ok())
      return Result<DemandKey>(charged);
    auto request = item.second.resolve(step.tile_grid, step.tiled);
    if (!request.ok() ||
        !allowed.contains(request.ok() ? request.value() : TileRequest{}))
      return Result<DemandKey>(
          Status{ErrorCode::InvalidArgument,
                 "tile demand exceeds plan",
                 FailureReason::None,
                 {FailureOrigin::Protocol, FailureScope::Group}});
    const auto weight = 1 + request.value().tiles.size();
    if (!core_internal::can_add(weight, entries, maximum))
      return Result<DemandKey>(
          Status{ErrorCode::ResourceExhausted, "demand query metadata limit"});
    entries += weight;
    hash.text(item.first);
    hash.integer(request.value().whole);
    hash.integer(request.value().tiles.size());
    for (const auto tile : request.value().tiles)
      hash.integer(tile);
  }
  return Result<DemandKey>(
      DemandKey{hash.finish(), entries, lease.take_value()});
}
Result<Footprint> demand_samples(const TileRequest& requested,
                                 const PlanStep& step,
                                 const FootprintLimits& limits) {
  auto normalized = requested.resolve(step.tile_grid, step.tiled);
  if (!normalized.ok())
    return Result<Footprint>(normalized.status());
  const auto& shape =
      step.output_result_schema && !step.output_result_schema->tensors.empty()
          ? step.output_result_schema->tensors[0].sample_shape()
          : std::vector<std::uint64_t>{1};
  const auto& request = normalized.value();
  if (request.whole)
    return Footprint::all(shape, limits);
  auto samples = Footprint::none(shape, limits);
  if (!samples.ok())
    return samples;
  for (auto tile : request.tiles) {
    if (limits.consume_work) {
      auto charged = limits.consume_work(1);
      if (!charged.ok())
        return Result<Footprint>(charged);
    }
    auto region = step.tile_grid->region(tile);
    if (!region.ok())
      return Result<Footprint>(region.status());
    auto part = Footprint::from_regions(shape, {region.take_value()}, limits);
    if (!part.ok())
      return part;
    samples = samples.value().unite(part.value(), limits);
    if (!samples.ok())
      return samples;
  }
  return samples;
}
}  // namespace ps::execution_internal
