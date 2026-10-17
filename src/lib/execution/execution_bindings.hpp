#pragma once
#include <vector>

#include "photospider/execution/execution.hpp"
namespace ps::execution_internal {
Status validate_input_ownership(const std::vector<ExecutionBinding>&,
                                const ResourceBudget&);
ErrorCode binding_stop(const ExecutionPlan&, const CancellationToken&) noexcept;
Result<std::vector<ExecutionBinding>> preflight_regional_bindings(
    const ExecutionPlan&, const ExecutionBindings&, const CancellationToken&,
    const ResourceBudget&);
}  // namespace ps::execution_internal
