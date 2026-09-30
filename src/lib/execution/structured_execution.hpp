#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
class SharedResults;
/** @brief Uses the existing context worker and its completion/drain boundary.
 */
struct StructuredServices final {
  const ps_cpu_parallel_service_v1* cpu_parallel = nullptr;
  const ps_cpu_tiles_service_v1* cpu_tiles = nullptr;
  const ps_gpu_service_v11* gpu = nullptr;
  std::function<Status()> gpu_status;
  BufferAllocator allocator;
  // NOLINTBEGIN(readability/casting)
  std::function<std::uint64_t(std::uint64_t)> allocation_capacity;
  // NOLINTEND
  std::function<void(ExecutionDiagnostics&)> observe;
  std::function<Result<std::pair<Value, std::uint64_t>>(const Value&)>
      native_input;
};
using StructuredDispatch = std::function<Status(
    Backend, bool, bool, const CancellationToken&,
    const std::function<Status(const StructuredServices&)>&,
    const std::function<void()>&)>;
Result<ExecutionResult> execute_structured(
    const ExecutionPlan& plan, std::vector<ExecutionBinding> bindings,
    std::shared_ptr<OperationRegistry> operations, ResourceBudget resources,
    const ExecutionOptions& options, const ExecutionSink* sink,
    const CancellationToken& cancellation,
    const std::function<ErrorCode()>& stop, const StructuredDispatch& dispatch,
    const DemandQuery* requested = nullptr,
    std::map<std::string, ValueFragments>* fragment_outputs = nullptr,
    const std::string& snapshot_identity = {},
    SharedResults* shared_results = nullptr);
}  // namespace ps::execution_internal
