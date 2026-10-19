#include "photospider/execution/execution.hpp"

#include <exception>
#include <new>
#include <utility>

#include "execution/demand_query.hpp"
#include "execution/execution_bindings.hpp"
#include "execution/execution_context_state.hpp"
#include "execution/result_run.hpp"

namespace ps {
using execution_internal::binding_stop;
using execution_internal::preflight_regional_bindings;

Result<ExecutionResult> ExecutionContext::execute(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const CancellationToken& cancellation, const ExecutionOptions& options) {
  return execute_regions(plan, std::move(bindings), cancellation, options);
}

Result<DemandResult> ExecutionContext::execute_fragments(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const DemandQuery& query, const CancellationToken& cancellation,
    const ExecutionOptions& options) {
  auto executed =
      execute_regions(plan, std::move(bindings), cancellation, options, &query);
  if (!executed.ok())
    return Result<DemandResult>(executed.status());
  auto completed = executed.take_value();
  DemandResult result;
  result.results = std::move(completed.results);
  result.diagnostics = std::move(completed.diagnostics);
  result.dependencies = std::move(completed.dependencies);
  return Result<DemandResult>(std::move(result));
}

Result<ExecutionResult> ExecutionContext::execute_regions(
    const ExecutionPlan& plan, ExecutionBindings bindings,
    const CancellationToken& cancellation, const ExecutionOptions& options,
    const DemandQuery* requested) {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock() != impl_->operation_registry)
    return Result<ExecutionResult>(
        Status{ErrorCode::Stale, "Result plan is invalid, stale or foreign"});
  const auto stop = [&] {
    if (options.dependencies.sets.cancellation.cancelled())
      return ErrorCode::Cancelled;
    return binding_stop(plan, cancellation);
  };
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
    if (requested) {
      auto key = execution_internal::demand_key(
          *requested, plan, options.dependencies.sets.maximum_boxes,
          *impl_->budget);
      if (!key.ok())
        return failure(key.status());
    }
    auto validated = preflight_regional_bindings(plan, bindings, cancellation,
                                                 *impl_->budget);
    if (!validated.ok())
      return failure(validated.status());
    return execution_internal::run_result_plan(
        &impl_->cpu_pool, impl_->gpu_pool.get(), impl_->native_device,
        impl_->native_uploads, &impl_->waiting_admission, impl_->budget,
        impl_->operation_registry, plan, plan.current_check_,
        validated.take_value(), cancellation, options, requested,
        impl_->cache.get(), impl_->result_checkpoints.get());
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
