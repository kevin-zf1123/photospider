#include "plugin/dependency_discovery.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "photospider/core/checked_math.hpp"
#include "photospider/core/status_helpers.hpp"

namespace ps::plugin_internal {
namespace {

std::uint64_t word(const std::uint8_t* bytes, unsigned width) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i)
    value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
  return value;
}
}  // namespace
Status visit_discovery_records(
    const CpuStorage& table, std::uint32_t capacity, std::uint32_t candidates,
    const FootprintLimits& limits,
    const std::function<Status(const GpuDiscoveryRecord&)>& visit) {
  if (limits.cancellation.cancelled())
    return {ErrorCode::Cancelled, {}};
  const auto size = 16 + static_cast<std::uint64_t>(capacity) * 32;
  if (!visit || !capacity || capacity > 65536 || table.bytes().size() != size)
    return core_internal::invalid_argument("invalid GPU discovery allocation");
  const auto* bytes = table.bytes().data();
  const auto count = word(bytes, 4), overflow = word(bytes + 4, 4);
  if (word(bytes + 8, 8) || overflow > 1 || count > candidates)
    return core_internal::invalid_argument("invalid GPU discovery header");
  if (overflow)
    return Status::failure(ErrorCode::ResourceExhausted,
                           "GPU discovery request table overflow");
  if (count > capacity || count > limits.maximum_boxes)
    return core_internal::invalid_argument(
        "invalid GPU discovery request count");
  for (std::uint64_t i = 0; i < count; ++i) {
    if (limits.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    if (limits.consume_work) {
      auto charged = limits.consume_work(1);
      if (!charged.ok())
        return charged;
    }
    const auto* row = bytes + 16 + i * 32;
    GpuDiscoveryRecord record;
    record.input = word(row, 4);
    record.kind = word(row + 4, 4);
    record.tensor_slot = word(row + 8, 4);
    record.begin = word(row + 16, 8);
    record.end = word(row + 24, 8);
    if (word(row + 12, 4) || record.kind > 1 ||
        (record.kind == 0 && record.begin >= record.end) ||
        (record.kind == 1 &&
         (record.tensor_slot || record.begin || record.end)))
      return core_internal::invalid_argument("invalid GPU discovery record");
    auto status = visit(record);
    if (!status.ok())
      return status;
  }
  return limits.cancellation.cancelled() ? Status{ErrorCode::Cancelled, {}}
                                         : Status::success();
}
Result<ResourceVector<TileNeed>> decode_result_discovery(
    const CpuStorage& table, std::uint32_t capacity, std::uint32_t candidates,
    const ResultProgramQuery& query, const ResourceBudget& resources,
    FootprintLimits limits, std::uint64_t* metadata_entries) {
  using Answer = Result<ResourceVector<TileNeed>>;
  if (!metadata_entries)
    return Answer(core_internal::invalid_argument("missing discovery counter"));
  struct Group {
    ResourceVector<TileInterval> tiles;
    explicit Group(const ResourceBudget& root)
        : tiles(ResourceAllocator<TileInterval>(root)) {}
  };
  using Key = std::pair<std::uint32_t, std::uint32_t>;
  using Entry = std::pair<const Key, Group>;
  std::map<Key, Group, std::less<Key>, ResourceAllocator<Entry>> groups{
      std::less<Key>{}, ResourceAllocator<Entry>(resources)};
  auto visited = visit_discovery_records(
      table, capacity, candidates, limits, [&](const GpuDiscoveryRecord& row) {
        if (row.input >= query.inputs.size() ||
            !query.inputs[row.input].result_schema)
          return core_internal::invalid_argument(
              "invalid Result GPU discovery input");
        if (row.kind == 0) {
          auto grid =
              TileGrid::from_schema(*query.inputs[row.input].result_schema,
                                    query.tile_extent, row.tensor_slot);
          if (!grid.ok())
            return grid.status();
          if (row.end > grid.value().tile_count())
            return core_internal::invalid_argument(
                "GPU discovery tile outside compiled input grid");
        }
        auto found = groups.find({row.input, row.tensor_slot});
        if (found == groups.end())
          found =
              groups.emplace(Key{row.input, row.tensor_slot}, Group(resources))
                  .first;
        if (row.kind == 0)
          found->second.tiles.push_back({row.begin, row.end});
        return Status::success();
      });
  if (!visited.ok())
    return Answer(visited);
  ResourceVector<TileNeed> result{ResourceAllocator<TileNeed>(resources)};
  auto entries = *metadata_entries;
  for (auto& group : groups) {
    const auto count = group.second.tiles.size();
    if (limits.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    if (limits.consume_work) {
      std::uint64_t levels = 1;
      for (auto remaining = count; remaining > 1; remaining /= 2)
        ++levels;
      auto charged = limits.consume_work(1 + count * levels);
      if (!charged.ok())
        return Answer(charged);
    }
    auto tiles = TileSet::from_intervals(resources, group.second.tiles.data(),
                                         group.second.tiles.size());
    if (!tiles.ok())
      return Answer(tiles.status());
    const auto weight = 1 + tiles.value().intervals().size();
    if (!core_internal::can_add(entries, weight, limits.maximum_boxes))
      return Answer(
          Status{ErrorCode::ResourceExhausted, "GPU discovery metadata limit"});
    entries += weight;
    const bool descriptor = tiles.value().empty();
    result.push_back({group.first.first, group.first.second, descriptor,
                      tiles.take_value()});
  }
  if (limits.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  *metadata_entries = entries;
  return Answer(std::move(result));
}

}  // namespace ps::plugin_internal
