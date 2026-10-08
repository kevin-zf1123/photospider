#include "plugin/dependency_discovery.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps::plugin_internal {
namespace {
Status invalid(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}
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
  const auto size = 16 + static_cast<std::uint64_t>(capacity) * 144;
  if (!visit || !capacity || capacity > 65536 || table.bytes().size() != size)
    return invalid("invalid GPU discovery allocation");
  const auto* bytes = table.bytes().data();
  const auto count = word(bytes, 4), overflow = word(bytes + 4, 4);
  if (word(bytes + 8, 8) || overflow > 1 || count > candidates)
    return invalid("invalid GPU discovery header");
  if (overflow)
    return Status::failure(ErrorCode::ResourceExhausted,
                           "GPU discovery request table overflow");
  if (count > capacity || count > limits.maximum_boxes)
    return invalid("invalid GPU discovery request count");
  for (std::uint64_t i = 0; i < count; ++i) {
    if (limits.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    const auto* row = bytes + 16 + i * 144;
    GpuDiscoveryRecord record;
    record.input = word(row, 4);
    record.roles = word(row + 4, 4);
    record.rank = word(row + 8, 4);
    record.slot = word(row + 12, 4);
    if (!record.roles || (record.roles & ~7U) || !record.rank ||
        record.rank > 8)
      return invalid("invalid GPU discovery record");
    for (unsigned axis = 0; axis < 8; ++axis) {
      record.offsets[axis] = word(row + 16 + axis * 8, 8);
      record.extents[axis] = word(row + 80 + axis * 8, 8);
      if (axis < record.rank ? !record.extents[axis]
                             : record.offsets[axis] || record.extents[axis])
        return invalid("invalid GPU discovery axis");
    }
    auto status = visit(record);
    if (!status.ok())
      return status;
  }
  return Status::success();
}
Result<ResourceVector<ResultTensorNeed>> decode_result_discovery(
    const CpuStorage& table, std::uint32_t capacity, std::uint32_t candidates,
    const ResultProgramQuery& query, const ResourceBudget& resources,
    FootprintLimits limits, std::uint64_t* metadata_entries) {
  using Answer = Result<ResourceVector<ResultTensorNeed>>;
  using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
  struct Box {
    ResourceLease lease;
    Region region;
  };
  using Boxes = ResourceVector<Box>;
  using Entry = std::pair<const Key, Boxes>;
  std::map<Key, Boxes, std::less<Key>, ResourceAllocator<Entry>> groups{
      std::less<Key>{}, ResourceAllocator<Entry>(resources)};
  auto visited = visit_discovery_records(
      table, capacity, candidates, limits, [&](const GpuDiscoveryRecord& row) {
        if (row.input >= query.inputs.size() ||
            !query.inputs[row.input].result_schema ||
            row.slot >= query.inputs[row.input].result_schema->tensors.size())
          return invalid("invalid Result GPU discovery input slot");
        const auto& spec =
            query.inputs[row.input].result_schema->tensors[row.slot];
        const auto rank = spec.batch_axes.size() + spec.descriptor.shape.size();
        if (row.rank != rank)
          return invalid("invalid Result GPU discovery rank");
        auto lease = resources.reserve(ResourceCapacity::host(
            sizeof(Region) +
                rank * (sizeof(RegionDimension) + sizeof(std::uint64_t)),
            sizeof(Region) +
                rank * (sizeof(RegionDimension) + sizeof(std::uint64_t))));
        if (!lease.ok())
          return lease.status();
        auto shape = spec.sample_shape();
        std::vector<RegionDimension> dimensions;
        dimensions.reserve(rank);
        for (unsigned axis = 0; axis < rank; ++axis) {
          if (row.offsets[axis] >= shape[axis] ||
              row.extents[axis] > shape[axis] - row.offsets[axis])
            return invalid("GPU discovery rectangle outside Result domain");
          dimensions.push_back({row.offsets[axis], row.extents[axis]});
        }
        auto raw = Footprint::from_regions(
            shape, {Region(std::move(dimensions))}, limits);
        if (!raw.ok())
          return raw.status();
        auto closed = spec.close_samples(raw.value(), limits);
        if (!closed.ok())
          return closed.status();
        if (closed.value() != raw.value())
          return invalid("GPU discovery omits Result tuple closure");
        const Key key{row.input, row.slot, row.roles};
        auto found = groups.find(key);
        if (found == groups.end()) {
          if (groups.size() >= limits.maximum_boxes)
            return Status{ErrorCode::ResourceExhausted,
                          "GPU discovery group limit"};
          found = groups.emplace(key, Boxes{ResourceAllocator<Box>(resources)})
                      .first;
        }
        found->second.push_back(
            {lease.take_value(), raw.value().boxes().front()});
        return Status::success();
      });
  if (!visited.ok())
    return Answer(visited);
  ResourceVector<ResultTensorNeed> result{
      ResourceAllocator<ResultTensorNeed>(resources)};
  for (auto& group : groups) {
    std::uint64_t roles = 0;
    for (std::uint32_t bit = 1; bit <= 4; bit <<= 1)
      roles += (std::get<2>(group.first) & bit) != 0;
    const auto& spec = query.inputs[std::get<0>(group.first)]
                           .result_schema->tensors[std::get<1>(group.first)];
    const auto rank = spec.batch_axes.size() + spec.descriptor.shape.size();
    const auto bytes =
        group.second.size() * (sizeof(Region) + rank * sizeof(RegionDimension));
    auto admission = resources.reserve(ResourceCapacity::host(bytes, bytes));
    if (!admission.ok())
      return Answer(admission.status());
    if (limits.consume_work) {
      auto charged = limits.consume_work(group.second.size() * (1 + 2 * rank));
      if (!charged.ok())
        return Answer(charged);
    }
    std::vector<Region> boxes;
    boxes.reserve(group.second.size());
    for (const auto& box : group.second)
      boxes.push_back(box.region);
    auto samples = Footprint::from_regions(spec.sample_shape(), boxes, limits);
    if (!samples.ok())
      return Answer(samples.status());
    const auto count = samples.value().boxes().size() + 1;
    if (*metadata_entries > limits.maximum_boxes ||
        count > (limits.maximum_boxes - *metadata_entries) / roles)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "GPU discovery metadata limit"});
    *metadata_entries += roles * count;
    result.push_back({std::get<0>(group.first), std::get<1>(group.first),
                      samples.take_value(), std::get<2>(group.first)});
  }
  return Answer(std::move(result));
}

}  // namespace ps::plugin_internal
