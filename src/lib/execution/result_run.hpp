#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "photospider/execution/execution.hpp"
namespace ps::gpu_internal {
class Device;
}
namespace ps::execution_internal {
class MemoryBudget;
class ThreadPool;
class WaitingAdmission;
class NativeUploadRegistry;
class ResultCache;
class SharedResults;
class ResultCheckpoints;
// Captured binding/options owners survive callback retirement. Pools, admission
// and requested demands are borrowed for this synchronous call. A frozen plan
// owner enables transferring the structured coordinator to live shared peers.
Result<ExecutionResult> run_result_plan(
    ThreadPool* pool, ThreadPool* gpu_pool,
    const std::shared_ptr<gpu_internal::Device>& native_device,
    const std::shared_ptr<execution_internal::NativeUploadRegistry>&
        native_uploads,
    WaitingAdmission* admission, const std::shared_ptr<MemoryBudget>& budget,
    const std::shared_ptr<OperationRegistry>& operations,
    const ExecutionPlan& plan, std::function<bool()> current,
    std::vector<ExecutionBinding> bindings,
    const CancellationToken& caller_cancellation,
    const ExecutionOptions& options, const DemandQuery* requested,
    const std::string& snapshot_identity = {},
    execution_internal::ResultCache* dependency_cache = nullptr,
    execution_internal::SharedResults* shared_results = nullptr,
    bool atom_outcomes = false,
    execution_internal::ResultCheckpoints* result_checkpoints = nullptr,
    std::shared_ptr<const ExecutionPlan> plan_owner = {});
}  // namespace ps::execution_internal
