#include "execution/execution_bindings.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "plugin/port_validation.hpp"
namespace ps::execution_internal {
Status validate_input_ownership(const std::vector<ExecutionBinding>& bindings,
                                const ResourceBudget& budget) {
  for (const auto& binding : bindings)
    if (!binding.result.owned_by(budget))
      return Status{ErrorCode::InvalidArgument,
                    "Result binding belongs to a different resource root"};
  return Status::success();
}

/** @brief Allocation-free stop selection after a successful plan entry check.
 */
ErrorCode binding_stop(const ExecutionPlan& plan,
                       const CancellationToken& cancellation) noexcept {
  if (cancellation.cancelled())
    return ErrorCode::Cancelled;
  return plan.current() ? ErrorCode::Ok : ErrorCode::Stale;
}

/** @brief Validates/copies complete regional binding metadata before any source
 * callback. */
Result<std::vector<ExecutionBinding>> preflight_regional_bindings(
    const ExecutionPlan& plan, const ExecutionBindings& bindings,
    const CancellationToken& cancellation, const ResourceBudget& root) {
  if (bindings.inputs.size() > 4096)
    return Result<std::vector<ExecutionBinding>>(
        Status::failure(ErrorCode::InvalidArgument, "too many bindings"));
  std::map<std::string, std::vector<const ExecutionBinding*>> named;
  for (const auto& binding : bindings.inputs)
    named[binding.name].push_back(&binding);
  for (const auto& entry : named)
    if (!input_internal::valid_input_name(entry.first))
      return Result<std::vector<ExecutionBinding>>(Status::failure(
          ErrorCode::InvalidArgument, "malformed binding name"));
  for (const auto& entry : named)
    if (entry.second.size() != 1)
      return Result<std::vector<ExecutionBinding>>(Status::failure(
          ErrorCode::InvalidArgument, "duplicate binding name"));
  std::set<std::string> declared;
  for (const auto& declaration : plan.input_declarations())
    declared.insert(declaration.name);
  for (const auto& entry : named)
    if (declared.count(entry.first) == 0)
      return Result<std::vector<ExecutionBinding>>(
          Status::failure(ErrorCode::InvalidArgument, "extra binding name"));
  for (const auto& name : declared)
    if (named.count(name) == 0)
      return Result<std::vector<ExecutionBinding>>(
          Status::failure(ErrorCode::InvalidArgument, "missing binding name"));
  std::vector<ExecutionBinding> result;
  for (const auto& declaration : plan.input_declarations()) {
    auto binding = *named.at(declaration.name)[0];
    if (!binding.result.valid())
      return Result<std::vector<ExecutionBinding>>(Status{
          ErrorCode::InvalidArgument, "binding requires an immutable Result"});
    if (!binding.result.owned_by(root))
      return Result<std::vector<ExecutionBinding>>(
          Status{ErrorCode::InvalidArgument,
                 "Result binding belongs to a different Root"});
    if (!declaration.result_schema ||
        !binding.result.schema().same_schema(*declaration.result_schema) ||
        !binding.result.descriptor().ok())
      return Result<std::vector<ExecutionBinding>>(
          Status{ErrorCode::TypeMismatch,
                 "Result binding schema or finality differs"});
    result.push_back(std::move(binding));
  }
  const auto stopped = binding_stop(plan, cancellation);
  if (stopped != ErrorCode::Ok)
    return Result<std::vector<ExecutionBinding>>(Status{stopped, {}});
  return Result<std::vector<ExecutionBinding>>(std::move(result));
}

}  // namespace ps::execution_internal
