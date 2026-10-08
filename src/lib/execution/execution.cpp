#include "photospider/execution/execution.hpp"

#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "execution/execution_bindings.hpp"
#include "execution/execution_context_state.hpp"
#include "execution/memory_budget.hpp"
#include "execution/result_run.hpp"

namespace ps {
using execution_internal::binding_stop;
using execution_internal::preflight_regional_bindings;

Result<ExecutionResult> ExecutionContext::execute(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const CancellationToken& cancellation, const ExecutionOptions& options) {
  return execute_regions(plan, std::move(bindings), cancellation, options);
}

Result<ExecutionResult> ExecutionContext::execute_atoms(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const DemandQuery& requested, const CancellationToken& cancellation,
    const ExecutionOptions& options) {
  auto root = resource_budget();
  if (!root.ok())
    return Result<ExecutionResult>(root.status());
  ResourceAllocationScope scope(root.value());
  return execute_regions(plan, std::move(bindings), cancellation, options, {},
                         true, &requested);
}
Result<ExecutionResult> ExecutionContext::execute_regions(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const CancellationToken& cancellation, const ExecutionOptions& options,
    const std::string& snapshot_identity, bool atom_outcomes,
    const DemandQuery* requested,
    std::shared_ptr<const ExecutionPlan> plan_owner) {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock() != impl_->operation_registry)
    return Result<ExecutionResult>(
        Status{ErrorCode::Stale, "Result plan is invalid, stale or foreign"});
  const auto stop = [&] { return binding_stop(plan, cancellation); };
  const auto failure = [&](Status status) {
    const auto code = stop();
    if (code != ErrorCode::Ok &&
        status.detail.origin != FailureOrigin::Protocol)
      status =
          Status{code,
                 {},
                 code == ErrorCode::Cancelled ? FailureReason::Cancelled
                                              : FailureReason::StaleVersion,
                 {FailureOrigin::Cancellation, FailureScope::Run}};
    return Result<ExecutionResult>(std::move(status));
  };
  if (stop() != ErrorCode::Ok)
    return failure(Status{stop(), {}});
  try {
    auto validated = preflight_regional_bindings(plan, bindings, cancellation,
                                                 *impl_->budget->resources());
    if (!validated.ok())
      return failure(validated.status());
    return execution_internal::run_result_plan(
        &impl_->cpu_pool, impl_->gpu_pool.get(), impl_->native_device,
        impl_->native_uploads, &impl_->waiting_admission, impl_->budget,
        impl_->operation_registry, plan, plan.current_check_,
        validated.take_value(), cancellation, options, requested,
        snapshot_identity, impl_->cache.get(), &impl_->shared_results,
        atom_outcomes, impl_->result_checkpoints.get(), std::move(plan_owner));
  } catch (const std::bad_alloc&) {
    return failure(Status{ErrorCode::ResourceExhausted, {}});
  } catch (const std::exception& error) {
    return failure(
        Status{ErrorCode::OperationFailed, error.what() ? error.what() : ""});
  } catch (...) {
    return failure(Status{ErrorCode::OperationFailed, {}});
  }
}

}  // namespace ps
