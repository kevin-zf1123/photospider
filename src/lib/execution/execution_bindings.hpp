#pragma once
#include <memory>
#include <vector>

#include "photospider/execution/execution.hpp"
namespace ps::data_internal {
class MemoryBudget;
}
namespace ps::execution_internal {
Status retain_managed_inputs(
    std::vector<ExecutionBinding>*,
    const std::shared_ptr<data_internal::MemoryBudget>&);
ErrorCode binding_stop(const ExecutionPlan&, const CancellationToken&) noexcept;
Result<std::vector<ExecutionBinding>> preflight_regional_bindings(
    const ExecutionPlan&, const ExecutionBindings&, const CancellationToken&,
    const ResourceBudget&);
}  // namespace ps::execution_internal
