#pragma once

#include <utility>

#include "data/result_window_access.hpp"
#include "photospider/plugin/tile_helpers.hpp"

namespace ps::plugin_internal {
// Retain a certified rectangle without materializing a potentially huge view.
inline Status append_publication_view(
    ResultBuilder& builder, const ResultRef& source,
    const ResultDescriptor& facts, std::uint32_t slot, const Region& region,
    const ResourceBudget& root, const CancellationToken& cancellation,
    const std::function<Status(std::uint64_t)>& consume, unsigned depth = 0) {
  auto charged = consume(1 + region.rank());
  if (!charged.ok())
    return charged;
  auto scratch = root.reserve(ResourceCapacity::host(4096, 4096));
  if (!scratch.ok())
    return scratch.status();
  auto window = source.acquire_tensor(facts, slot, region, cancellation);
  if (!window.ok())
    return window.status();
  ResultTensorViewTransform identity;
  for (unsigned axis = 0; axis < region.rank(); ++axis)
    identity.source_axes.push_back({static_cast<std::int32_t>(axis), 0, 1, 1});
  auto status = builder.publish_tensor_view(slot, region, window.value(),
                                            identity, cancellation);
  if (status.ok() || status.reason != FailureReason::ViewUnavailable)
    return status;
  if (source.schema().tensors[slot].layout.spatial) {
    status = builder.publish_tensor_view(slot, region, {&window.value()},
                                         cancellation);
    if (status.ok() || status.reason != FailureReason::ViewUnavailable)
      return status;
  }
  if (depth == 64)
    return Status{ErrorCode::InvalidArgument,
                  "publication backing partition did not terminate"};
  auto split = execution_internal::ResultWindowAccess::visit_backing_regions(
      window.value(), root, [&](const Region& part) {
        bool smaller = false;
        for (unsigned axis = 0; axis < region.rank(); ++axis)
          smaller |=
              part.dimensions()[axis].extent < region.dimensions()[axis].extent;
        if (!smaller)
          return Status{ErrorCode::InvalidArgument,
                        "publication backing partition did not shrink"};
        return append_publication_view(builder, source, facts, slot, part, root,
                                       cancellation, consume, depth + 1);
      });
  return !split.ok()     ? split.status()
         : split.value() ? Status::success()
                         : status;
}
// Validates complete physical tile contributions without expanding tile IDs.
inline Result<TileRequest> published_tiles(const SchemaTemplate& schema,
                                           const ResultDescriptor& descriptor,
                                           std::uint64_t extent,
                                           const ResourceBudget& root,
                                           const FootprintLimits& limits = {}) {
  using Answer = Result<TileRequest>;
  auto grids = TileGrid::members(schema, extent);
  if (!grids.ok())
    return Answer(grids.status());
  TileRequest result;
  for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
    const auto& samples = descriptor.tensor_coverage(slot);
    if (samples.empty())
      continue;
    auto tiles = sdk::tiles_for_samples(root, grids.value()[slot], samples);
    if (!tiles.ok())
      return Answer(tiles.status());
    auto closed = grids.value()[slot].coverage(tiles.value(), limits);
    if (!closed.ok())
      return Answer(closed.status());
    if (closed.value() != samples)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "publication contains an incomplete tile"});
    result.members.emplace(slot, tiles.take_value());
  }
  return Answer(std::move(result));
}
inline Result<TileRequest> visible_tiles(const SchemaTemplate& schema,
                                         const TileRequest& produced,
                                         std::uint64_t extent,
                                         const ResourceBudget& root) {
  using Answer = Result<TileRequest>;
  auto grids = TileGrid::members(schema, extent);
  if (!grids.ok())
    return Answer(grids.status());
  auto visible = produced;
  for (const auto& scope : schema.publication_scopes) {
    if (scope.policy == ResultPublicationPolicy::CompleteBundle) {
      bool complete = true;
      for (auto slot : scope.members) {
        const auto found = produced.members.find(slot);
        complete = complete && found != produced.members.end() &&
                   found->second.size() == grids.value()[slot].tile_count();
      }
      if (!complete)
        for (auto slot : scope.members)
          visible.members.erase(slot);
    } else if (scope.policy == ResultPublicationPolicy::StablePrefix) {
      auto found = visible.members.find(scope.members.front());
      if (found == visible.members.end())
        continue;
      const auto& ranges = found->second.intervals();
      if (ranges.empty() || ranges.front().begin) {
        visible.members.erase(found);
      } else {
        auto prefix = TileSet::range(root, 0, ranges.front().end);
        if (!prefix.ok())
          return Answer(prefix.status());
        found->second = prefix.take_value();
      }
    }
  }
  return Answer(std::move(visible));
}
}  // namespace ps::plugin_internal
