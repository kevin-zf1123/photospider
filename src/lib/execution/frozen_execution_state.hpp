#pragma once
#include <memory>
#include <string>

#include "photospider/execution/execution.hpp"
namespace ps {
/** @brief Shared backing for immutable frozen executions.
 * @details `allocate_shared` charges this state object and its shared control
 * block through `resources`. The state owns its plan, copied bindings,
 * operation registry, and identity; allocations inside the plan and standard
 * containers retain their existing allocator accounting.
 */
struct FrozenExecution::State {
  explicit State(const ResourceBudget& budget) : resources(budget) {}
  ResourceBudget resources;
  ExecutionPlan plan;
  ExecutionBindings bindings;
  std::shared_ptr<OperationRegistry> operations;
  std::string execution_identity;
};
}  // namespace ps
