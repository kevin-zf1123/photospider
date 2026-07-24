#include "compute/compute_task_dispatcher.hpp"

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "compute/compute_dispatch_plan_builder.hpp"
#include "compute/compute_node_task_runner.hpp"
#include "compute/compute_result_committer.hpp"
#include "compute/compute_run.hpp"
#include "compute/compute_task_submission.hpp"
#include "compute/execution_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Rejects one full-plan boundary after accepted Run cancellation.
 * @param lease Retained lifecycle lease for the active dispatch.
 * @return Nothing while cancellation has not won.
 * @throws GraphError with ComputeError after explicit/deadline cancellation.
 * @throws std::system_error when Run-state synchronization fails.
 * @note The Run retains the stable cancellation reason for outer
 * ComputeService translation; this helper only stops later local work.
 */
void observe_dispatch_cancellation(const ComputeRunLease& lease) {
  if (lease.observe_cancellation().has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ComputeRun cancelled during full-plan dispatch.");
  }
}

/**
 * @brief Requires every planned full-HP node to have a prepared operation.
 * @param graph Candidate Graph used to describe a missing provider.
 * @param plan Complete Run-owned submission plan being prepared.
 * @return Nothing when every aligned operation slot is populated.
 * @throws std::logic_error when plan vectors violate their alignment invariant.
 * @throws GraphError with ComputeError for the first missing implementation.
 * @throws std::bad_alloc if construction of failure context cannot allocate.
 * @note This validation belongs to admission-aware production preparation.
 * Generic TaskSubmissionPlan construction preserves its historical ability to
 * represent an unresolved operation for storage and worker-failure tests.
 * Production parallel compute historically reports a missing prepared
 * operation through its ComputeError boundary after dispatch; eager validation
 * preserves that public category while moving failure before installation.
 */
void validate_prepared_operations(const GraphModel& graph,
                                  const TaskSubmissionPlan& plan) {
  const auto& execution_order = plan.execution_order();
  const auto& resolved_operations = plan.resolved_ops();
  if (execution_order.size() != resolved_operations.size()) {
    throw std::logic_error(
        "TaskSubmissionPlan operation slots do not match execution order.");
  }
  for (std::size_t i = 0; i < execution_order.size(); ++i) {
    if (resolved_operations[i].has_value()) {
      continue;
    }
    const Node& node = graph.node(execution_order[i]);
    throw GraphError(GraphErrc::ComputeError,
                     "No op for " + node.type + ":" + node.subtype);
  }
}

}  // namespace

/**
 * @brief Complete unpublished service-backed full-HP dispatch.
 *
 * @throws Nothing from destruction after successful preparation.
 * @note The lifecycle lease has a stable heap address before NodeTaskRunner
 * borrows it. Field order destroys the physical batch before that lease and
 * before any borrowed Graph/service pointer can be observed again.
 */
struct PreparedComputeDispatchState final {
  /**
   * @brief Captures stable dispatcher ownership before plan construction.
   * @param active_graph Request-local Graph snapshot.
   * @param cache_service Borrowed cache service.
   * @param dispatch_request Copied cache and telemetry controls.
   * @param active_run Candidate Run retaining the plan.
   * @param lease Candidate lifecycle lease copied for callbacks.
   * @param service Exact process execution owner.
   * @throws std::bad_alloc when request string copying allocates.
   */
  PreparedComputeDispatchState(
      GraphModel& active_graph, GraphCacheService& cache_service,
      const ComputeTaskDispatcher::ComputeDispatchRequest& dispatch_request,
      ComputeRun& active_run, const ComputeRunLease& lease,
      ExecutionService& service)
      : graph(&active_graph),
        cache(&cache_service),
        request(dispatch_request),
        run(&active_run),
        lifecycle_lease(lease),
        execution_service(&service) {}

  /** @brief Request-local Graph snapshot used for execution and commit. */
  GraphModel* graph = nullptr;
  /** @brief Borrowed cache service used by final result commit. */
  GraphCacheService* cache = nullptr;
  /** @brief Complete copied cache and telemetry controls. */
  ComputeTaskDispatcher::ComputeDispatchRequest request;
  /** @brief Candidate Run retaining plan, runner, and temporary results. */
  ComputeRun* run = nullptr;
  /** @brief Stable lease address borrowed by the Run-owned task runner. */
  ComputeRunLease lifecycle_lease;
  /** @brief Stable Run-owned plan prepared before lifecycle installation. */
  TaskSubmissionPlan* plan = nullptr;
  /** @brief Exact service that prepared and may publish the batch. */
  ExecutionService* execution_service = nullptr;
  /** @brief Complete unpublished ready/index/resource ownership. */
  PreparedExecutionRun physical_run;
};

/** @copydoc PreparedComputeDispatch::PreparedComputeDispatch */
PreparedComputeDispatch::PreparedComputeDispatch(
    std::unique_ptr<PreparedComputeDispatchState> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc PreparedComputeDispatch::PreparedComputeDispatch */
PreparedComputeDispatch::PreparedComputeDispatch(
    PreparedComputeDispatch&& other) noexcept = default;  // NOLINT

/** @copydoc PreparedComputeDispatch::operator= */
PreparedComputeDispatch& PreparedComputeDispatch::operator=(
    PreparedComputeDispatch&& other) noexcept {
  if (this != &other) {
    if (state_) {
      std::terminate();
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc PreparedComputeDispatch::~PreparedComputeDispatch */
PreparedComputeDispatch::~PreparedComputeDispatch() noexcept = default;

/**
 * @brief Constructs the dispatcher with borrowed compute services.
 *
 * @param traversal Traversal service used by the plan-building collaborator.
 * @param cache Cache service used by workers and result commit.
 * @param events Event service used by worker node execution.
 * @throws Nothing directly.
 * @note Implementation stores references only; ownership remains with
 * ComputeService.
 */
ComputeTaskDispatcher::ComputeTaskDispatcher(GraphTraversalService& traversal,
                                             GraphCacheService& cache,
                                             GraphEventService& events)
    : traversal_(traversal), cache_(cache), events_(events) {}

/**
 * @brief Clears timing entries for a new timed dispatch.
 *
 * @param graph Graph whose TimingCollector is reset.
 * @throws Nothing directly.
 * @note This does not reset graph.total_io_time_ms; execute() handles that
 * field alongside this helper.
 */
void ComputeTaskDispatcher::clear_timing_results(GraphModel& graph) {
  std::lock_guard<std::mutex> lk(graph.timing_mutex_);
  graph.timing_results.node_timings.clear();
  graph.timing_results.total_ms = 0.0;
}

/**
 * @brief Submits dirty task handles through dispatcher-owned source-first
 * ordering.
 *
 * @param task_runtime Execution runtime receiving the dirty task batches.
 * @param source_handles Dirty source handles submitted and drained first.
 * @param source_task_count Total source tasks tracked by runtime completion.
 * @param initial_downstream_handles First downstream handles released after
 * the source boundary.
 * @param downstream_task_count Total downstream tasks tracked by runtime
 * completion.
 * @param before_downstream Optional boundary validation callback.
 * @return Nothing.
 * @throws Rethrows task_runtime, task, or before_downstream exceptions.
 * @throws std::bad_alloc unchanged when handle submission, dependency, or
 * callback storage exhausts memory.
 * @note The helper centralizes production dirty source-first submission while
 * dirty executors retain their request-local ExecutionTaskExecutor ownership.
 */
void ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
    ExecutionTaskRuntime& task_runtime,
    std::vector<ExecutionTaskHandle>&& source_handles, int source_task_count,
    std::vector<ExecutionTaskHandle>&& initial_downstream_handles,
    int downstream_task_count, std::function<void()> before_downstream) {
  task_runtime.submit_initial_task_handles(std::move(source_handles),
                                           source_task_count,
                                           ExecutionTaskPriority::High);
  task_runtime.wait_for_completion();

  if (before_downstream) {
    try {
      before_downstream();
    } catch (...) {
      auto error = std::current_exception();
      try {
        task_runtime.log_event(ExecutionTraceAction::RethrowException, -1);
      } catch (...) {
      }
      try {
        task_runtime.set_exception(error);
      } catch (...) {
      }
      std::rethrow_exception(error);
    }
  }

  task_runtime.submit_initial_task_handles(
      std::move(initial_downstream_handles), downstream_task_count,
      ExecutionTaskPriority::Normal);
  task_runtime.wait_for_completion();
}

/**
 * @brief Executes one high-precision dispatch through the execution runtime.
 *
 * @param graph GraphModel whose target output is computed.
 * @param task_runtime Execution runtime used for this dispatch.
 * @param request Per-call dispatch options.
 * @param run Request observer that mints a dispatcher lease retaining plan,
 * runner, callback, exception, and output state through commit.
 * @param lifecycle_lease Retained request lease observed and copied into
 * runtime callback ownership.
 * @return Mutable high-precision output stored on the target graph node.
 * @throws GraphError for missing targets, missing final output, compute
 * failures, or scheduling failures; may also propagate operation/cache
 * exceptions with added context.
 * @throws std::bad_alloc unchanged when plan, task, operation, cache,
 * telemetry, or result storage exhausts memory.
 * @note The function binds the Run-owned worker runner before submission,
 * waits for owned callbacks and their dependent releases, then commits
 * Run-owned temp outputs under graph_mutex_. Every full-HP callback reaches the
 * runner through a composite identity and a matching lease; the execution
 * runtime remains borrowed through the current synchronous wait. Cooperative
 * observations bracket planning, dispatch, phase transitions, and result
 * commit; cancellation that wins before commit leaves temp outputs unpublished.
 */
NodeOutput& ComputeTaskDispatcher::execute(
    GraphModel& graph, ExecutionTaskRuntime& task_runtime,
    const ComputeDispatchRequest& request, ComputeRun& run,
    const ComputeRunLease& lifecycle_lease) {
  return execute_impl(graph, task_runtime, nullptr, nullptr, nullptr, request,
                      run, lifecycle_lease);
}

/**
 * @brief Executes one full-HP dispatch through the injected CPU service.
 *
 * @param graph Graph whose target is computed and committed.
 * @param execution_service Process-owned ready-only CPU runtime.
 * @param host Active Graph trace observation context.
 * @param request Per-call dispatch options.
 * @param run Request owner retaining leased dispatcher state.
 * @param lifecycle_lease Retained request lease observed and copied into
 * service submission ownership.
 * @return Mutable committed target output.
 * @throws GraphError or standard exceptions from shared planning, service
 * execution, cache, telemetry, and commit.
 * @note Accepted cancellation purges only this Run's queued service entries
 * and waits executing callbacks to drain. Cancellation observed before the
 * corresponding boundaries rejects dependent publication and suppresses final
 * Graph cache commit.
 */
NodeOutput& ComputeTaskDispatcher::execute(
    GraphModel& graph, ExecutionService& execution_service,
    ExecutionHostContext& host, const std::string& execution_type,
    const ComputeDispatchRequest& request, ComputeRun& run,
    const ComputeRunLease& lifecycle_lease) {
  return execute_prepared(prepare(graph, execution_service, host,
                                  execution_type, request, run,
                                  lifecycle_lease));
}

/** @copydoc ComputeTaskDispatcher::prepare */
PreparedComputeDispatch ComputeTaskDispatcher::prepare(
    GraphModel& graph, ExecutionService& execution_service,
    ExecutionHostContext& host, const std::string& execution_type,
    const ComputeDispatchRequest& request, ComputeRun& run,
    const ComputeRunLease& lifecycle_lease) {
  observe_dispatch_cancellation(lifecycle_lease);
  const int node_id = request.node_id;
  if (!graph.has_node(node_id)) {
    throw GraphError(
        GraphErrc::NotFound,
        "Cannot compute: node " + std::to_string(node_id) + " not found.");
  }

  auto state = std::make_unique<PreparedComputeDispatchState>(
      graph, cache_, request, run, lifecycle_lease, execution_service);

  observe_dispatch_cancellation(state->lifecycle_lease);
  std::vector<Device> available_devices =
      execution_service.available_devices(host, execution_type);
  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal_, node_id, std::move(available_devices), false);
  state->plan = &plan;
  validate_prepared_operations(graph, plan);

  plan.emplace_task_runner(NodeTaskRunnerContext{
      graph,
      cache_,
      events_,
      execution_service,
      graph.timing_results,
      graph.timing_mutex_,
      plan.execution_order(),
      plan.id_to_idx(),
      plan.temp_results(),
      plan.resolved_ops(),
      plan.compute_plan().task_graph,
      request.force_recache,
      request.enable_timing,
      request.disable_disk_cache,
      request.benchmark_events,
      &state->lifecycle_lease,
  });
  observe_dispatch_cancellation(state->lifecycle_lease);

  if (plan.empty()) {
    if (request.force_recache ||
        !graph.node(node_id).cached_output_high_precision.has_value()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Planned dispatch produced no executable tasks for node " +
              std::to_string(node_id) +
              " and the target has no reusable high-precision output.");
    }
    return PreparedComputeDispatch(std::move(state));
  }

  std::vector<ReadyTaskSubmission> initial_submissions =
      plan.make_initial_ready_submissions(state->lifecycle_lease);
  const CpuRunResourceDemand resource_demand{
      state->lifecycle_lease.retained_memory_bytes(),
      ReadyTaskSubmission::default_resource_demand()};
  state->physical_run = execution_service.prepare_run(
      host, execution_type, std::move(initial_submissions),
      static_cast<int>(plan.size()), resource_demand);
  observe_dispatch_cancellation(state->lifecycle_lease);
  return PreparedComputeDispatch(std::move(state));
}

/** @copydoc ComputeTaskDispatcher::execute_prepared */
NodeOutput& ComputeTaskDispatcher::execute_prepared(
    PreparedComputeDispatch prepared) {
  if (!prepared.state_) {
    throw std::invalid_argument(
        "ComputeTaskDispatcher requires an active prepared dispatch.");
  }
  std::unique_ptr<PreparedComputeDispatchState> state =
      std::move(prepared.state_);
  if (state->graph == nullptr || state->cache == nullptr ||
      state->run == nullptr || state->plan == nullptr ||
      state->execution_service == nullptr) {
    throw std::invalid_argument(
        "ComputeTaskDispatcher prepared dispatch is incomplete.");
  }

  observe_dispatch_cancellation(state->lifecycle_lease);
  if (!state->run->advance_to(ComputeRunPhase::Queued)) {
    observe_dispatch_cancellation(state->lifecycle_lease);
  }
  if (!state->run->advance_to(ComputeRunPhase::Running)) {
    observe_dispatch_cancellation(state->lifecycle_lease);
  }
  if (state->request.enable_timing) {
    clear_timing_results(*state->graph);
    state->graph->total_io_time_ms = 0.0;
  }
  if (state->request.force_recache) {
    state->graph->clear_full_task_graph_cache();
    clear_planned_high_precision_caches(*state->graph,
                                        state->graph->graph_mutex_,
                                        state->plan->execution_order());
  }
  ComputeDispatchPlanBuilder::publish_plan_inspection(
      *state->graph, state->plan->compute_plan());
  observe_dispatch_cancellation(state->lifecycle_lease);
  if (!state->plan->empty()) {
    if (!state->physical_run.active()) {
      throw std::logic_error(
          "ComputeTaskDispatcher nonempty plan has no physical preparation.");
    }
    state->execution_service->execute_prepared_run(
        std::move(state->physical_run));
  }

  observe_dispatch_cancellation(state->lifecycle_lease);
  if (!state->run->advance_to(ComputeRunPhase::CommitPending)) {
    observe_dispatch_cancellation(state->lifecycle_lease);
  }
  observe_dispatch_cancellation(state->lifecycle_lease);

  ComputeResultCommitter committer(*state->cache, state->graph->graph_mutex_,
                                   state->request.cache_precision);
  if (state->request.enable_timing) {
    committer.finalize_timing(state->graph->timing_results,
                              state->graph->timing_mutex_);
  }
  observe_dispatch_cancellation(state->lifecycle_lease);
  committer.commit(*state->graph, state->plan->execution_order(),
                   state->plan->temp_results());
  observe_dispatch_cancellation(state->lifecycle_lease);

  const int node_id = state->request.node_id;
  if (!state->graph->node(node_id).cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "Parallel computation finished but target node has no "
                     "output. An upstream error likely occurred.");
  }
  return *state->graph->mutable_node(node_id).cached_output_high_precision;
}

/**
 * @brief Executes shared full-HP semantics through one physical route.
 *
 * @param graph GraphModel whose target output is computed.
 * @param task_runtime Runtime used for task execution traces and completion.
 * @param execution_service Optional migrated CPU service selector.
 * @param host Optional Graph observation target for service execution.
 * @param request Per-call dispatch options.
 * @param run Request observer that mints all retained leases.
 * @param lifecycle_lease Retained request lease observed and copied into the
 * selected physical route.
 * @return Mutable high-precision output stored on the target graph node.
 * @throws GraphError or standard exceptions from the selected route and shared
 * semantic stages.
 * @note Only dispatch selection differs; plan, runner, temporary results, and
 * commit remain shared and Run/dispatcher-owned. Cancellation observations
 * surround every semantic stage; tiled providers observe per tile while a
 * monolithic provider already entered remains non-preemptible.
 */
NodeOutput& ComputeTaskDispatcher::execute_impl(
    GraphModel& graph, ExecutionTaskRuntime& task_runtime,
    ExecutionService* execution_service, ExecutionHostContext* host,
    const std::string* execution_type, const ComputeDispatchRequest& request,
    ComputeRun& run, const ComputeRunLease& lifecycle_lease) {
  observe_dispatch_cancellation(lifecycle_lease);
  const int node_id = request.node_id;
  auto& timing_results = graph.timing_results;
  auto& timing_mutex = graph.timing_mutex_;
  auto& graph_mutex = graph.graph_mutex_;
  if (!graph.has_node(node_id)) {
    throw GraphError(
        GraphErrc::NotFound,
        "Cannot compute: node " + std::to_string(node_id) + " not found.");
  }

  if (request.enable_timing) {
    clear_timing_results(graph);
    graph.total_io_time_ms = 0.0;
  }

  if (request.force_recache) {
    graph.clear_full_task_graph_cache();
  }

  ComputeRunLease dispatcher_lease = lifecycle_lease;
  observe_dispatch_cancellation(dispatcher_lease);
  std::vector<Device> available_devices =
      execution_service != nullptr && host != nullptr &&
              execution_type != nullptr
          ? execution_service->available_devices(*host, *execution_type)
          : task_runtime.available_devices();
  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal_, node_id, std::move(available_devices));
  if (request.force_recache) {
    clear_planned_high_precision_caches(graph, graph_mutex,
                                        plan.execution_order());
  }

  plan.emplace_task_runner(NodeTaskRunnerContext{
      graph,
      cache_,
      events_,
      task_runtime,
      timing_results,
      timing_mutex,
      plan.execution_order(),
      plan.id_to_idx(),
      plan.temp_results(),
      plan.resolved_ops(),
      plan.compute_plan().task_graph,
      request.force_recache,
      request.enable_timing,
      request.disable_disk_cache,
      request.benchmark_events,
      &dispatcher_lease,
  });
  observe_dispatch_cancellation(dispatcher_lease);
  if (!run.advance_to(ComputeRunPhase::Queued)) {
    observe_dispatch_cancellation(dispatcher_lease);
  }
  if (!run.advance_to(ComputeRunPhase::Running)) {
    observe_dispatch_cancellation(dispatcher_lease);
  }
  if (execution_service != nullptr) {
    if (host == nullptr || execution_type == nullptr) {
      throw std::logic_error(
          "ExecutionService dispatch requires host and route context.");
    }
    dispatch_planned_tasks(graph, *execution_service, *host, *execution_type,
                           node_id, plan, dispatcher_lease);
  } else {
    dispatch_planned_tasks(graph, task_runtime, node_id, plan,
                           dispatcher_lease);
  }
  observe_dispatch_cancellation(dispatcher_lease);
  if (!run.advance_to(ComputeRunPhase::CommitPending)) {
    observe_dispatch_cancellation(dispatcher_lease);
  }

  observe_dispatch_cancellation(dispatcher_lease);
  ComputeResultCommitter committer(cache_, graph_mutex,
                                   request.cache_precision);
  if (request.enable_timing) {
    committer.finalize_timing(timing_results, timing_mutex);
  }
  observe_dispatch_cancellation(dispatcher_lease);
  committer.commit(graph, plan.execution_order(), plan.temp_results());
  observe_dispatch_cancellation(dispatcher_lease);

  if (!graph.node(node_id).cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "Parallel computation finished but target node has no "
                     "output. An upstream error likely occurred.");
  }
  return *graph.mutable_node(node_id).cached_output_high_precision;
}

}  // namespace ps::compute
