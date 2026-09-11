#include "plugin/dependency_discovery.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
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
Result<std::vector<DependencyNeed>> decode_discovery(
    const CpuStorage& table, std::uint32_t capacity, std::uint32_t candidates,
    const DependencyQuery& query, const FootprintLimits& limits,
    std::uint64_t* normalization_work, std::uint64_t* metadata_entries) {
  using Answer = Result<std::vector<DependencyNeed>>;
  if (limits.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  const auto size = 16 + static_cast<std::uint64_t>(capacity) * 144;
  if (!normalization_work || !metadata_entries || !capacity ||
      capacity > 65536 || table.bytes().size() != size)
    return Answer(invalid("invalid GPU discovery allocation"));
  const auto* bytes = table.bytes().data();
  const auto count = word(bytes, 4), overflow = word(bytes + 4, 4);
  if (word(bytes + 8, 8) || overflow > 1 || count > candidates)
    return Answer(invalid("invalid GPU discovery header"));
  if (overflow)
    return Answer(Status::failure(ErrorCode::ResourceExhausted,
                                  "GPU discovery request table overflow"));
  if (count > capacity || count > limits.maximum_boxes)
    return Answer(invalid("invalid GPU discovery request count"));
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<Region>> groups;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (limits.cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    const auto* row = bytes + 16 + i * 144;
    const auto port = word(row, 4), roles = word(row + 4, 4),
               rank = word(row + 8, 4);
    if (port >= query.inputs.size() || !roles || (roles & ~UINT64_C(7)) ||
        word(row + 12, 4) || rank != query.inputs[port].descriptor.shape.size())
      return Answer(invalid("invalid GPU discovery record"));
    std::vector<RegionDimension> dimensions;
    for (unsigned a = 0; a < 8; ++a) {
      const auto offset = word(row + 16 + a * 8, 8),
                 extent = word(row + 80 + a * 8, 8);
      if (a < rank) {
        const auto domain = query.inputs[port].descriptor.shape[a];
        if (!extent || offset >= domain || extent > domain - offset)
          return Answer(invalid("GPU discovery rectangle outside domain"));
        dimensions.push_back({offset, extent});
      } else if (offset || extent) {
        return Answer(invalid("nonzero unused GPU discovery axis"));
      }
    }
    Region region(std::move(dimensions));
    const auto& input = query.inputs[port];
    if (!input_internal::complete_image_channels(input.descriptor, input.facets,
                                                 region))
      return Answer(invalid("GPU discovery omits image channel closure"));
    groups[{port, roles}].push_back(std::move(region));
  }
  std::vector<DependencyNeed> result;
  for (const auto& group : groups) {
    std::uint64_t roles = 0;
    for (std::uint32_t bit = 1; bit <= 4; bit <<= 1)
      roles += (group.first.second & bit) != 0;
    if (*metadata_entries > limits.maximum_boxes ||
        (limits.maximum_boxes - *metadata_entries) / roles <= 1)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "GPU discovery metadata limit"});
    auto grant = limits;
    grant.maximum_boxes =
        (limits.maximum_boxes - *metadata_entries) / roles - 1;
    grant.maximum_work = std::min(grant.maximum_work, *normalization_work);
    std::uint64_t used = 0;
    struct Charge {
      std::uint64_t* remaining;
      const std::uint64_t& used;
      ~Charge() { *remaining -= used; }
    } charge{normalization_work, used};
    auto set = Footprint::from_regions(
        query.inputs[group.first.first].descriptor.shape, group.second, grant,
        &used);
    if (!set.ok())
      return Answer(set.status());
    *metadata_entries += roles * (set.value().boxes().size() + 1);
    result.push_back(
        {group.first.first, group.first.second, set.take_value(), {}});
  }
  return Answer(std::move(result));
}
}  // namespace ps::plugin_internal
