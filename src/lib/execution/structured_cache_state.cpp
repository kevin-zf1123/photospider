#include "execution/structured_cache_state.hpp"

#include <memory>

namespace ps::execution_internal {
Status StructuredCacheState::charge(std::uint64_t units) {
  if (units > remaining_)
    return Status{ErrorCode::ResourceExhausted,
                  "Result checkpoint cache work exhausted",
                  FailureReason::WorkLimit,
                  {FailureOrigin::Resource, FailureScope::Run}};
  auto status = root_.consume({units});
  if (!status.ok())
    return status;
  remaining_ -= units;
  return Status::success();
}
std::shared_ptr<ResultCheckpointScope> StructuredCacheState::scope(
    const ResourceString& key, ResultCheckpoints* table,
    std::uint64_t maximum) {
  auto found = scopes_.find(key);
  if (found != scopes_.end())
    return found->second;
  auto scope = table ? table->acquire(key, root_, maximum)
                     : std::allocate_shared<ResultCheckpointScope>(
                           ResourceAllocator<ResultCheckpointScope>(root_),
                           root_, maximum);
  if (scope)
    scopes_.emplace(key, scope);
  return scope;
}
}  // namespace ps::execution_internal
