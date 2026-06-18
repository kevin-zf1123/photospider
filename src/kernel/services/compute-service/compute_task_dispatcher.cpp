#include "kernel/services/compute-service/compute_task_dispatcher.hpp"

#include <exception>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "kernel/services/compute-service/compute_node_task_runner.hpp"
#include "kernel/services/compute-service/compute_result_committer.hpp"
#include "kernel/services/compute-service/compute_task_submission.hpp"

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
 * @brief Delegates source-first dirty task submission to the submission unit.
 *
 * @param task_runtime Scheduler runtime that executes and records task errors.
 * @param source_tasks Dirty source tasks to run with high priority.
 * @param downstream_tasks Dependent tasks to run with normal priority after
 * source completion and optional validation.
 * @param epoch Optional scheduler epoch passed through to task submission.
 * @param before_downstream Optional boundary validation callback.
 * @throws Rethrows any task or callback exception after recording it in
 * task_runtime.
 * @note The public dispatcher API remains stable while the implementation
 * lives in compute_task_submission.cpp with other scheduler-submit helpers.
 */
void ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
    SchedulerTaskRuntime& task_runtime,
    std::vector<SchedulerTaskRuntime::Task>&& source_tasks,
    std::vector<SchedulerTaskRuntime::Task>&& downstream_tasks,
    std::optional<uint64_t> epoch, std::function<void()> before_downstream) {
  ps::compute::submit_dirty_ready_tasks_source_first(
      task_runtime, std::move(source_tasks), std::move(downstream_tasks), epoch,
      std::move(before_downstream));
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
 * @throws Rethrows task_runtime, task, or before_downstream exceptions.
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
      task_runtime.log_event(SchedulerTraceAction::RethrowException, -1);
      task_runtime.set_exception(error);
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
 * @return Mutable high-precision output stored on the target graph node.
 * @throws GraphError for missing targets, missing final output, compute
 * failures, or scheduling failures; may also propagate operation/cache
 * exceptions with added context.
 * @note The function builds all worker closures before submission, waits for
 * completion, then commits temp outputs under graph_mutex_.
 */
NodeOutput& ComputeTaskDispatcher::execute(
    GraphModel& graph, SchedulerTaskRuntime& task_runtime,
    const ComputeDispatchRequest& request) {
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

  TaskSubmissionPlan plan(graph, traversal_, node_id);
  if (request.force_recache) {
    clear_planned_high_precision_caches(graph, graph_mutex,
                                        plan.execution_order());
  }

  NodeTaskRunner runner(NodeTaskRunnerContext{
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
  plan.build_scheduler_tasks(runner, task_runtime);
  dispatch_planned_tasks(graph, task_runtime, node_id, plan);

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
