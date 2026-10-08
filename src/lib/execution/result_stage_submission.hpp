#pragma once

#include <functional>
#include <future>
#include <memory>

#include "execution/callback_pool.hpp"

namespace ps::execution_internal {
// Owns one admitted stage's work, promise and Root lease. A retirement owner
// clears work captures before delivering the future; rejected queue entries
// deliver Cancelled and retire their waiting capacity. The caller must drain
// this future before borrowed pool/resource/coordinator owners leave scope.
struct ResultStageCompletion {
  ResourceLease lease;
  std::promise<Result<int>> promise;
  std::future<Result<int>> future{promise.get_future()};
  Result<int> result{Status{ErrorCode::Internal, {}}};
  std::function<Result<int>()> work;
};
Result<std::shared_ptr<ResultStageCompletion>> submit_result_stage(
    ThreadPool*, WaitingAdmission*, std::function<Result<int>()>,
    const ResourceBudget* resources = nullptr);
}  // namespace ps::execution_internal
