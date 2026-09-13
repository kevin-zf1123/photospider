#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
class SharedResults;
/** @brief Uses the existing context worker and its completion/drain boundary.
 */
using StructuredDispatch = std::function<Status(const std::function<Status()>&,
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
