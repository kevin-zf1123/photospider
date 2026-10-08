#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "execution/demand_query.hpp"
#include "execution/execution_bindings.hpp"
#include "execution/execution_context_state.hpp"
#include "execution/frozen_execution_state.hpp"
#include "execution/memory_budget.hpp"
#include "execution/result_run.hpp"
#include "photospider/execution/execution.hpp"
namespace ps {
using execution_internal::binding_stop;
using execution_internal::demand_key;
using execution_internal::preflight_regional_bindings;
namespace {
Result<std::string> frozen_identity() {
  static std::atomic<std::uint64_t> sequence{1};
  auto next = sequence.load();
  do {
    if (next == UINT64_MAX)
      return Result<std::string>(Status{ErrorCode::ResourceExhausted, {}});
  } while (!sequence.compare_exchange_weak(next, next + 1));
  return Result<std::string>("frozen-" + std::to_string(next));
}
}  // namespace
/** @brief Returns the plan pinned by this frozen state.
 * @return A borrowed plan reference, or a static empty plan for a default
 * object. The reference remains valid while this object retains its state.
 */
const ExecutionPlan& FrozenExecution::plan() const noexcept {
  static const ExecutionPlan empty;
  return state_ ? state_->plan : empty;
}
Result<FrozenExecution> FrozenExecution::for_region(
    const std::string& output, const Region& region) const {
  if (!valid())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "invalid frozen execution"));
  auto tile = state_->plan.tile_plan(output, region);
  if (!tile.ok())
    return Result<FrozenExecution>(tile.status());
  auto state = std::allocate_shared<State>(
      ResourceAllocator<State>(state_->resources), state_->resources);
  state->plan = tile.take_value();
  state->bindings = state_->bindings;
  state->operations = state_->operations;
  state->execution_identity = state_->execution_identity;
  FrozenExecution result;
  result.state_ = std::move(state);
  return Result<FrozenExecution>(std::move(result));
}

Result<FrozenExecution> ExecutionContext::freeze(
    const ExecutionPlan& plan, ExecutionBindings bindings) const {
  if (!impl_ || !plan.current() ||
      plan.operation_registry_.lock().get() != impl_->operation_registry.get())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "invalid stale or foreign plan"));
  if (!impl_->budget->resources())
    return Result<FrozenExecution>(
        Status{ErrorCode::InvalidArgument,
               "Result capture requires managed_resources"});
  auto validated = preflight_regional_bindings(plan, bindings, {},
                                               *impl_->budget->resources());
  if (!validated.ok())
    return Result<FrozenExecution>(validated.status());
  auto identity = frozen_identity();
  if (!identity.ok())
    return Result<FrozenExecution>(identity.status());
  const auto& root = *impl_->budget->resources();
  auto state = std::allocate_shared<FrozenExecution::State>(
      ResourceAllocator<FrozenExecution::State>(root), root);
  state->execution_identity = identity.take_value();
  state->plan = plan;
  state->bindings = std::move(bindings);
  state->operations = impl_->operation_registry;
  // The explicit owner keeps registry and input lifetimes independent of the
  // editable graph. Ordinary plan predicates are never changed in place.
  state->plan.current_check_ = [] { return true; };
  if (!plan.current())
    return Result<FrozenExecution>(
        Status::failure(ErrorCode::Stale, "graph changed during freeze"));
  FrozenExecution frozen;
  frozen.state_ = std::move(state);
  return Result<FrozenExecution>(std::move(frozen));
}
Result<DemandResult> ExecutionContext::execute_fragments(
    const FrozenExecution& frozen, const DemandQuery& query,
    const CancellationToken& cancellation, const ExecutionOptions& options) {
  const auto captured = frozen;
  if (!impl_ || !captured.valid() || !captured.state_->plan.current() ||
      captured.state_->operations != impl_->operation_registry)
    return Result<DemandResult>(
        Status{ErrorCode::Stale, "invalid or foreign frozen demand"});
  const auto stop = [&] {
    if (options.dependencies.sets.cancellation.cancelled())
      return ErrorCode::Cancelled;
    return binding_stop(captured.state_->plan, cancellation);
  };
  const auto failure = [&](Status status) {
    const auto code = stop();
    if (code != ErrorCode::Ok &&
        status.detail.origin != FailureOrigin::Protocol)
      status = Status{code, {}};
    return Result<DemandResult>(std::move(status));
  };
  try {
    if (stop() != ErrorCode::Ok)
      return failure(Status{stop(), {}});
    auto combined = CancellationToken::combine(
        {cancellation, options.dependencies.sets.cancellation});
    if (!combined.ok())
      return failure(combined.status());
    auto key = demand_key(query, captured.state_->plan,
                          options.dependencies.sets.maximum_boxes);
    if (!key.ok())
      return failure(key.status());
    auto validated = preflight_regional_bindings(
        captured.state_->plan, captured.state_->bindings, cancellation,
        *impl_->budget->resources());
    if (!validated.ok())
      return failure(validated.status());
    DemandResult result;
    auto run = execution_internal::run_result_plan(
        &impl_->cpu_pool, impl_->gpu_pool.get(), impl_->native_device,
        impl_->native_uploads, &impl_->waiting_admission, impl_->budget,
        impl_->operation_registry, captured.state_->plan,
        captured.state_->plan.current_check_, validated.take_value(),
        combined.value(), options, &query, captured.state_->execution_identity,
        impl_->cache.get(), &impl_->shared_results, false,
        impl_->result_checkpoints.get(),
        std::shared_ptr<const ExecutionPlan>(captured.state_,
                                             &captured.state_->plan));
    if (!run.ok())
      return failure(run.status());
    auto completed = run.take_value();
    result.diagnostics = std::move(completed.diagnostics);
    result.dependencies = std::move(completed.dependencies);
    result.results = std::move(completed.results);
    if (stop() != ErrorCode::Ok)
      return failure(Status{stop(), {}});
    return Result<DemandResult>(std::move(result));
  } catch (const std::bad_alloc&) {
    return failure(Status{ErrorCode::ResourceExhausted,
                          {},
                          FailureReason::CapacityLimit,
                          {FailureOrigin::Resource, FailureScope::Run}});
  } catch (const std::exception& error) {
    return failure(Status{ErrorCode::OperationFailed,
                          error.what() ? error.what() : "",
                          FailureReason::HostException});
  } catch (...) {
    return failure(
        Status{ErrorCode::OperationFailed, {}, FailureReason::HostException});
  }
}
Result<ExecutionResult> ExecutionContext::execute(
    const FrozenExecution& frozen, const CancellationToken& cancellation,
    const ExecutionOptions& options) {
  const auto captured = frozen;
  if (!captured.valid())
    return Result<ExecutionResult>(Status{ErrorCode::Stale, {}});
  return execute_regions(captured.state_->plan, captured.state_->bindings,
                         cancellation, options,
                         captured.state_->execution_identity, false, nullptr,
                         std::shared_ptr<const ExecutionPlan>(
                             captured.state_, &captured.state_->plan));
}
}  // namespace ps
