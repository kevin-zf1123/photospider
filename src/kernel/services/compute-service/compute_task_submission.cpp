#include "kernel/services/compute-service/compute_task_submission.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "kernel/services/compute-service/compute_dispatch_plan_builder.hpp"
#include "kernel/services/graph_traversal_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Formats a graph node id with the node name when it is still present.
 *
 * @param graph GraphModel used for node lookup.
 * @param node_id Node id being reported.
 * @return Human-readable scheduler error context.
 * @throws std::bad_alloc if string construction fails.
 * @note Missing nodes are reported by id only so late scheduling failures still
 * produce usable diagnostics.
 */
std::string node_context(const GraphModel& graph, int node_id) {
  const Node* node = graph.find_node(node_id);
  if (!node) {
    return "node " + std::to_string(node_id);
  }
  return "node " + std::to_string(node_id) + " (" + node->name + ")";
}

/**
 * @brief Creates a scheduler-stage GraphError exception pointer.
 *
 * @param graph GraphModel used to enrich the node label.
 * @param node_id Node whose dependent release failed.
 * @param detail Original scheduling exception detail.
 * @return Exception pointer carrying GraphErrc::ComputeError.
 * @throws std::bad_alloc if the wrapped error string cannot be allocated.
 * @note This separates compute failures from dependency-release failures while
 * preserving SchedulerTaskRuntime cross-thread exception transport.
 */
std::exception_ptr scheduling_failure(const GraphModel& graph, int node_id,
                                      const std::string& detail) {
  return std::make_exception_ptr(
      GraphError(GraphErrc::ComputeError, "Scheduling stage after " +
                                              node_context(graph, node_id) +
                                              " failed: " + detail));
}

/**
 * @brief Submits one dirty task and waits for its completion.
 *
 * @param task_runtime Scheduler runtime receiving the wrapped task.
 * @param task Dirty task closure to execute.
 * @param priority Scheduler priority used for submission.
 * @param epoch Optional scheduler epoch forwarded to the runtime.
 * @throws Rethrows task exceptions through the completion future after also
 * recording them in task_runtime.
 * @note The wrapper keeps dirty update callers synchronous while still using
 * scheduler-owned task execution and exception capture.
 */
void submit_one_dirty_task(SchedulerTaskRuntime& task_runtime,
                           SchedulerTaskRuntime::Task&& task,
                           SchedulerTaskPriority priority,
                           std::optional<uint64_t> epoch) {
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> future = done->get_future();
  task_runtime.submit_ready_task_any_thread(
      [task = std::move(task), done, &task_runtime]() mutable {
        try {
          if (task) {
            task();
          }
          done->set_value();
        } catch (...) {
          auto error = std::current_exception();
          task_runtime.log_event(SchedulerTraceAction::RethrowException, -1);
          task_runtime.set_exception(error);
          done->set_exception(error);
        }
      },
      priority, epoch);
  future.get();
}

/**
 * @brief Submits a dirty task batch concurrently or in deterministic order.
 *
 * @param task_runtime Scheduler runtime receiving wrapped tasks.
 * @param tasks Dirty task closures to execute.
 * @param priority Scheduler priority shared by the batch.
 * @param epoch Optional scheduler epoch forwarded to each task.
 * @param preserve_order Whether to wait after each task to preserve vector
 * order.
 * @throws Rethrows task exceptions after recording them in task_runtime.
 * @note Source tasks use concurrent submission; downstream dirty work preserves
 * deterministic order for existing commit semantics.
 */
void submit_dirty_batch(SchedulerTaskRuntime& task_runtime,
                        std::vector<SchedulerTaskRuntime::Task>&& tasks,
                        SchedulerTaskPriority priority,
                        std::optional<uint64_t> epoch, bool preserve_order) {
  if (preserve_order) {
    for (auto& task : tasks) {
      submit_one_dirty_task(task_runtime, std::move(task), priority, epoch);
    }
    return;
  }

  std::vector<std::future<void>> futures;
  futures.reserve(tasks.size());
  for (auto& task : tasks) {
    auto done = std::make_shared<std::promise<void>>();
    futures.push_back(done->get_future());
    task_runtime.submit_ready_task_any_thread(
        [task = std::move(task), done, &task_runtime]() mutable {
          try {
            if (task) {
              task();
            }
            done->set_value();
          } catch (...) {
            auto error = std::current_exception();
            task_runtime.log_event(SchedulerTraceAction::RethrowException, -1);
            task_runtime.set_exception(error);
            done->set_exception(error);
          }
        },
        priority, epoch);
  }
  for (auto& future : futures) {
    future.get();
  }
}

}  // namespace

TaskSubmissionPlan::TaskSubmissionPlan(GraphModel& graph,
                                       GraphTraversalService& traversal,
                                       int node_id)
    : graph_(graph),
      compute_plan_(
          ComputeDispatchPlanBuilder(traversal).build_high_precision_plan(
              graph, node_id)),
      execution_order_(compute_plan_.planned_nodes),
      dependency_state_(execution_order_, compute_plan_.task_graph) {
  resolve_operations();
  temp_results_.resize(execution_order_.size());
  tasks_.resize(execution_order_.size());
}

void TaskSubmissionPlan::build_scheduler_tasks(
    NodeTaskRunner& runner, SchedulerTaskRuntime& task_runtime) {
  for (size_t i = 0; i < execution_order_.size(); ++i) {
    const int current_node_id = execution_order_[i];
    const int current_node_idx = static_cast<int>(i);
    tasks_[i] = [this, &runner, &task_runtime, current_node_id,
                 current_node_idx]() {
      task_runtime.log_event(SchedulerTraceAction::Execute, current_node_id);
      try {
        runner.run_node(current_node_idx);
        release_dependents(current_node_idx, current_node_id, task_runtime);
      } catch (...) {
        task_runtime.log_event(SchedulerTraceAction::RethrowException,
                               current_node_id);
        throw;
      }
      task_runtime.dec_tasks_to_complete();
    };
  }
}

std::vector<SchedulerTaskRuntime::Task>
TaskSubmissionPlan::take_initial_tasks() {
  initial_tasks_.clear();
  submitted_initial_indices_.clear();
  append_graph_ready_tasks();
  if (initial_tasks_.empty()) {
    append_zero_dependency_tasks();
  }
  return std::move(initial_tasks_);
}

void TaskSubmissionPlan::log_initial_assignments(
    SchedulerTaskRuntime& task_runtime) const {
  for (size_t i = 0; i < execution_order_.size(); ++i) {
    if (submitted_initial_indices_.count(static_cast<int>(i))) {
      task_runtime.log_event(SchedulerTraceAction::AssignInitial,
                             execution_order_[i]);
    }
  }
}

void TaskSubmissionPlan::resolve_operations() {
  resolved_ops_.resize(execution_order_.size());
  for (size_t i = 0; i < execution_order_.size(); ++i) {
    const auto& node = graph_.node(execution_order_[i]);
    resolved_ops_[i] = OpRegistry::instance().resolve_for_intent(
        node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
  }
}

void TaskSubmissionPlan::release_dependents(
    int current_node_idx, int current_node_id,
    SchedulerTaskRuntime& task_runtime) {
  try {
    std::atomic_thread_fence(std::memory_order_release);
    dependency_state_.release_dependents(
        current_node_idx, [&](int dependent_idx) {
          task_runtime.submit_ready_task_from_worker(
              std::move(tasks_.at(dependent_idx)));
        });
  } catch (const std::out_of_range& e) {
    std::rethrow_exception(scheduling_failure(
        graph_, current_node_id, "out_of_range: " + std::string(e.what())));
  } catch (const std::exception& e) {
    std::rethrow_exception(
        scheduling_failure(graph_, current_node_id, e.what()));
  } catch (...) {
    std::rethrow_exception(
        scheduling_failure(graph_, current_node_id, "unknown exception"));
  }
}

void TaskSubmissionPlan::append_initial_task_for_node(int node_idx) {
  if (!dependency_state_.ready_for_initial_submit(node_idx)) {
    return;
  }
  if (submitted_initial_indices_.insert(node_idx).second) {
    initial_tasks_.push_back(std::move(tasks_.at(node_idx)));
  }
}

void TaskSubmissionPlan::append_graph_ready_tasks() {
  TaskGraphReadyChecker ready_checker;
  const std::vector<int> initial_ready_task_ids =
      ready_checker.initial_ready_task_ids(compute_plan_.task_graph);
  for (int task_id : initial_ready_task_ids) {
    if (task_id < 0 ||
        task_id >= static_cast<int>(compute_plan_.task_graph.tasks.size())) {
      continue;
    }
    const int planned_node_id = compute_plan_.task_graph.tasks[task_id].node_id;
    auto idx_it = dependency_state_.id_to_idx().find(planned_node_id);
    if (idx_it == dependency_state_.id_to_idx().end()) {
      continue;
    }
    append_initial_task_for_node(idx_it->second);
  }
}

void TaskSubmissionPlan::append_zero_dependency_tasks() {
  for (size_t i = 0; i < execution_order_.size(); ++i) {
    append_initial_task_for_node(static_cast<int>(i));
  }
}

void dispatch_or_run_fallback(
    GraphModel& graph, SchedulerTaskRuntime& task_runtime, int node_id,
    bool disable_disk_cache, TaskSubmissionPlan& plan,
    ComputeTaskDispatcher::SequentialFallback sequential_fallback) {
  if (plan.empty() && graph.has_node(node_id)) {
    if (!graph.node(node_id).cached_output_high_precision.has_value()) {
      sequential_fallback(graph, node_id, !disable_disk_cache);
    }
    return;
  }

  std::vector<SchedulerTaskRuntime::Task> initial_tasks =
      plan.take_initial_tasks();
  task_runtime.submit_initial_tasks(std::move(initial_tasks),
                                    static_cast<int>(plan.size()));
  plan.log_initial_assignments(task_runtime);
  task_runtime.wait_for_completion();
}

void submit_dirty_ready_tasks_source_first(
    SchedulerTaskRuntime& task_runtime,
    std::vector<SchedulerTaskRuntime::Task>&& source_tasks,
    std::vector<SchedulerTaskRuntime::Task>&& downstream_tasks,
    std::optional<uint64_t> epoch, std::function<void()> before_downstream) {
  submit_dirty_batch(task_runtime, std::move(source_tasks),
                     SchedulerTaskPriority::High, epoch, false);
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
  submit_dirty_batch(task_runtime, std::move(downstream_tasks),
                     SchedulerTaskPriority::Normal, epoch, true);
}

}  // namespace ps::compute
