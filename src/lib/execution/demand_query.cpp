#include "execution/demand_query.hpp"

#include <utility>

#include "core/checked_math.hpp"
#include "data/content_digest.hpp"
namespace ps::execution_internal {
Result<DemandKey> demand_key(const DemandQuery& query,
                             const ExecutionPlan& plan, std::uint64_t maximum) {
  content_internal::Sha256 hash;
  hash.text("photospider.demand-query.v1");
  hash.integer(query.size());
  std::uint64_t entries = 1;
  if (query.size() > maximum || !maximum)
    return Result<DemandKey>(Status{ErrorCode::ResourceExhausted, {}});
  for (const auto& item : query) {
    if (item.first.empty() || item.first.size() > 1024 ||
        !item.second.valid() || !plan.outputs().count(item.first))
      return Result<DemandKey>(Status{ErrorCode::InvalidArgument,
                                      "unknown or invalid demand query"});
    const auto weight = 1 + item.second.boxes().size();
    if (!core_internal::can_add(weight, entries, maximum))
      return Result<DemandKey>(
          Status{ErrorCode::ResourceExhausted, "demand query metadata limit"});
    entries += weight;
    hash.text(item.first);
    hash.integer(item.second.shape().size());
    for (const auto n : item.second.shape())
      hash.integer(n);
    hash.integer(item.second.boxes().size());
    for (const auto& box : item.second.boxes())
      for (const auto& d : box.dimensions()) {
        hash.integer(d.offset);
        hash.integer(d.extent);
      }
  }
  return Result<DemandKey>(DemandKey{hash.finish(), entries});
}
}  // namespace ps::execution_internal
