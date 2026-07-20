#include "compute/compute_task_dispatcher.hpp"

#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "compute/compute_node_task_runner.hpp"
#include "compute/compute_result_committer.hpp"
#include "compute/compute_run.hpp"
#include "compute/compute_task_submission.hpp"
#include "compute/execution_service.hpp"

namespace ps::compute {

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
 * @param task_runtime Scheduler runtime receiving the dirty task batches.
 * @param source_handles Dirty source handles submitted and drained first.
 * @param source_task_count Total source tasks tracked by scheduler completion.
 * @param initial_downstream_handles First downstream handles released after
 * the source boundary.
 * @param downstream_task_count Total downstream tasks tracked by scheduler
 * completion.
 * @param before_downstream Optional boundary validation callback.
 * @return Nothing.
 * @throws Rethrows task_runtime, task, or before_downstream exceptions.
 * @throws std::bad_alloc unchanged when handle submission, dependency, or
 * callback storage exhausts memory.
 * @note The helper centralizes production dirty source-first submission while
 * dirty executors retain their request-local TaskExecutor ownership.
 */
void ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
    SchedulerTaskRuntime& task_runtime,
    std::vector<TaskHandle>&& source_handles, int source_task_count,
    std::vector<TaskHandle>&& initial_downstream_handles,
    int downstream_task_count, std::function<void()> before_downstream) {
  task_runtime.submit_initial_task_handles(std::move(source_handles),
                                           source_task_count,
                                           SchedulerTaskPriority::High);
  task_runtime.wait_for_completion();

  if (before_downstream) {
    try {
      before_downstream();
    } catch (...) {
      auto error = std::current_exception();
      try {
        task_runtime.log_event(SchedulerTraceAction::RethrowException, -1);
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
      SchedulerTaskPriority::Normal);
  task_runtime.wait_for_completion();
}

/**
 * @brief Executes one high-precision dispatch through the scheduler runtime.
 *
 * @param graph GraphModel whose target output is computed.
 * @param task_runtime Scheduler runtime used for this dispatch.
 * @param request Per-call dispatch options.
 * @param run Request observer that mints a dispatcher lease retaining plan,
 * runner, callback, exception, and output state through commit.
 * @return Mutable high-precision output stored on the target graph node.
 * @throws GraphError for missing targets, missing final output, compute
 * failures, or scheduling failures; may also propagate operation/cache
 * exceptions with added context.
 * @throws std::bad_alloc unchanged when plan, task, operation, cache,
 * telemetry, or result storage exhausts memory.
 * @note The function binds the Run-owned worker runner before submission,
 * waits for owned callbacks and their dependent releases, then commits
 * Run-owned temp outputs under graph_mutex_. Every full-HP callback reaches the
 * runner through a composite identity and a matching lease; the scheduler
 * runtime remains borrowed through the current synchronous wait.
 */
NodeOutput& ComputeTaskDispatcher::execute(
    GraphModel& graph, SchedulerTaskRuntime& task_runtime,
    const ComputeDispatchRequest& request, ComputeRun& run) {
  return execute_impl(graph, task_runtime, nullptr, nullptr, request, run);
}

/**
 * @brief Executes one full-HP dispatch through the injected CPU service.
 *
 * @param graph Graph whose target is computed and committed.
 * @param execution_service Process-owned ready-only CPU runtime.
 * @param host Active Graph trace observation context.
 * @param request Per-call dispatch options.
 * @param run Request owner retaining leased dispatcher state.
 * @return Mutable committed target output.
 * @throws GraphError or standard exceptions from shared planning, service
 * execution, cache, telemetry, and commit.
 */
NodeOutput& ComputeTaskDispatcher::execute(
    GraphModel& graph, ExecutionService& execution_service,
    SchedulerHostContext& host, const ComputeDispatchRequest& request,
    ComputeRun& run) {
  return execute_impl(graph, execution_service, &execution_service, &host,
                      request, run);
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
 * @return Mutable high-precision output stored on the target graph node.
 * @throws GraphError or standard exceptions from the selected route and shared
 * semantic stages.
 * @note Only dispatch selection differs; plan, runner, temporary results, and
 * commit remain shared and Run/dispatcher-owned.
 */
NodeOutput& ComputeTaskDispatcher::execute_impl(
    GraphModel& graph, SchedulerTaskRuntime& task_runtime,
    ExecutionService* execution_service, SchedulerHostContext* host,
    const ComputeDispatchRequest& request, ComputeRun& run) {
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

  ComputeRunLease dispatcher_lease = run.acquire_lease();
  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal_, node_id, task_runtime.available_devices());
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
  });
  run.advance_to(ComputeRunPhase::Queued);
  run.advance_to(ComputeRunPhase::Running);
  if (execution_service != nullptr) {
    if (host == nullptr) {
      throw std::logic_error(
          "ExecutionService dispatch requires a host context.");
    }
    dispatch_planned_tasks(graph, *execution_service, *host, node_id, plan,
                           dispatcher_lease);
  } else {
    dispatch_planned_tasks(graph, task_runtime, node_id, plan,
                           dispatcher_lease);
  }
  run.advance_to(ComputeRunPhase::CommitPending);

  ComputeResultCommitter committer(cache_, graph_mutex,
                                   request.cache_precision);
  if (request.enable_timing) {
    committer.finalize_timing(timing_results, timing_mutex);
  }
  committer.commit(graph, plan.execution_order(), plan.temp_results());

  if (!graph.node(node_id).cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "Parallel computation finished but target node has no "
                     "output. An upstream error likely occurred.");
  }
  return *graph.mutable_node(node_id).cached_output_high_precision;
}

}  // namespace ps::compute
