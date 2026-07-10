#include "kernel/services/compute-service/compute_task_submission.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <variant>
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
 * @brief Checks whether a candidate matches the planned task shape.
 *
 * @param impl Registered operation implementation candidate.
 * @param require_tiled Whether the planned task graph contains tile tasks for
 * this node.
 * @return True when the implementation shape is compatible.
 * @throws Nothing.
 * @note Node/monolithic planned tasks may run either monolithic callbacks or
 * full-node tiled callbacks through NodeExecutor::execute(). Materialized tile
 * tasks must run a TileOpFunc.
 */
bool implementation_shape_compatible(const OpImplementation& impl,
                                     bool require_tiled) {
  return !require_tiled || impl.is_tiled();
}

/**
 * @brief Chooses a shape-compatible per-device implementation for HP compute.
 *
 * @param node Graph node whose operation is being resolved.
 * @param available_devices Devices exposed by the active scheduler runtime.
 * @param require_tiled Whether task graph materialization requires TileOpFunc.
 * @return Selected operation variant, or nullopt when no compatible
 * per-device implementation is available.
 * @throws std::bad_alloc if registry candidate storage allocation fails.
 * @note The registry owns available-device filtering, HP device priority, and
 * cost_score tie-breaking. This helper contributes only the local task-shape
 * predicate required by the already materialized dispatch plan.
 */
std::optional<OpRegistry::OpVariant> select_device_aware_hp_op(
    const Node& node, const std::vector<Device>& available_devices,
    bool require_tiled) {
  const OpRegistry& registry = OpRegistry::instance();
  const OpImplementation* best = registry.select_best_implementation(
      node.type, node.subtype, available_devices,
      ComputeIntent::GlobalHighPrecision,
      [require_tiled](const OpImplementation& impl) {
        return implementation_shape_compatible(impl, require_tiled);
      });
  if (!best) {
    return std::nullopt;
  }
  return best->func;
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
                                       int node_id,
                                       std::vector<Device> available_devices)
    : graph_(graph),
      compute_plan_(
          ComputeDispatchPlanBuilder(traversal).build_high_precision_plan(
              graph, node_id)),
      execution_order_(compute_plan_.planned_nodes),
      available_devices_(std::move(available_devices)),
      dependency_state_(execution_order_, compute_plan_.task_graph) {
  resolve_operations();
  temp_results_.resize(execution_order_.size());
  task_handles_.resize(compute_plan_.task_graph.tasks.size());
}

void TaskSubmissionPlan::build_scheduler_tasks(
    NodeTaskRunner& runner, SchedulerTaskRuntime& task_runtime) {
  runner_ = &runner;
  task_runtime_ = &task_runtime;
  for (const auto& task : compute_plan_.task_graph.tasks) {
    if (task.task_id < 0 ||
        task.task_id >= static_cast<int>(task_handles_.size())) {
      continue;
    }
    task_handles_[task.task_id] = make_handle(task.task_id);
  }
}

std::vector<TaskHandle> TaskSubmissionPlan::take_initial_task_handles() {
  initial_task_handles_.clear();
  submitted_initial_task_ids_.clear();
  append_graph_ready_tasks();
  if (initial_task_handles_.empty()) {
    append_zero_dependency_tasks();
  }
  return std::move(initial_task_handles_);
}

void TaskSubmissionPlan::log_initial_assignments(
    SchedulerTaskRuntime& task_runtime) const {
  for (int task_id : submitted_initial_task_ids_) {
    if (task_id < 0 ||
        task_id >= static_cast<int>(compute_plan_.task_graph.tasks.size())) {
      continue;
    }
    const int node_id = compute_plan_.task_graph.tasks[task_id].node_id;
    if (std::find(execution_order_.begin(), execution_order_.end(), node_id) !=
        execution_order_.end()) {
      task_runtime.log_event(SchedulerTraceAction::AssignInitial, node_id);
    }
  }
}

void TaskSubmissionPlan::run_task(int task_id) {
  if (!runner_ || !task_runtime_) {
    throw GraphError(GraphErrc::ComputeError,
                     "TaskSubmissionPlan has no bound task runner.");
  }
  const PlannedTask& task = compute_plan_.task_graph.tasks.at(task_id);
  const SchedulerTraceAction execute_action =
      task.kind == PlannedTaskKind::Tile ? SchedulerTraceAction::ExecuteTile
                                         : SchedulerTraceAction::Execute;
  task_runtime_->log_event(execute_action, task.node_id);
  try {
    runner_->run_task(task_id);
    release_dependents(task.task_id, task.node_id, *task_runtime_);
  } catch (...) {
    task_runtime_->log_event(SchedulerTraceAction::RethrowException,
                             task.node_id);
    throw;
  }
  task_runtime_->dec_tasks_to_complete();
}

void TaskSubmissionPlan::resolve_operations() {
  resolved_ops_.resize(execution_order_.size());
  for (size_t i = 0; i < execution_order_.size(); ++i) {
    const auto& node = graph_.node(execution_order_[i]);
    const bool has_tile_task = std::any_of(
        compute_plan_.task_graph.tasks.begin(),
        compute_plan_.task_graph.tasks.end(), [&](const PlannedTask& task) {
          return task.node_id == node.id && task.kind == PlannedTaskKind::Tile;
        });
    if (has_tile_task) {
      if (auto device_op =
              select_device_aware_hp_op(node, available_devices_, true)) {
        resolved_ops_[i] = std::move(*device_op);
        continue;
      }
      const auto* impls =
          OpRegistry::instance().get_implementations(node.type, node.subtype);
      if (impls && impls->tiled_hp) {
        resolved_ops_[i] = OpRegistry::OpVariant{*impls->tiled_hp};
        continue;
      }
    }
    if (auto device_op =
            select_device_aware_hp_op(node, available_devices_, false)) {
      resolved_ops_[i] = std::move(*device_op);
      continue;
    }
    resolved_ops_[i] = OpRegistry::instance().resolve_for_intent(
        node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
  }
}

/**
 * @brief Releases and submits dependents made ready by one completed task.
 *
 * @param current_task_id Completed task whose dependency edges advance.
 * @param current_node_id Completed node used in scheduling diagnostics.
 * @param task_runtime Scheduler runtime receiving newly ready handles.
 * @return Nothing.
 * @throws std::bad_alloc if ready-id/handle collection or submission exhausts
 * memory.
 * @throws GraphError wrapping other dependency, range, or scheduler failures.
 * @note The release fence publishes completed output before dependent handles
 * become visible; resource exhaustion is never relabeled as scheduling error.
 */
void TaskSubmissionPlan::release_dependents(
    int current_task_id, int current_node_id,
    SchedulerTaskRuntime& task_runtime) {
  try {
    std::atomic_thread_fence(std::memory_order_release);
    std::vector<int> ready_task_ids =
        dependency_state_.release_dependents(current_task_id);
    std::vector<TaskHandle> ready_handles;
    ready_handles.reserve(ready_task_ids.size());
    for (int dependent_task_id : ready_task_ids) {
      ready_handles.push_back(task_handles_.at(dependent_task_id));
    }
    task_runtime.submit_ready_task_handles_from_worker(
        std::move(ready_handles));
  } catch (const std::bad_alloc&) {
    throw;
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

void TaskSubmissionPlan::append_initial_task_handle(int task_id) {
  if (!dependency_state_.ready_for_initial_submit(task_id)) {
    return;
  }
  if (submitted_initial_task_ids_.insert(task_id).second) {
    initial_task_handles_.push_back(task_handles_.at(task_id));
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
    append_initial_task_handle(task_id);
  }
}

void TaskSubmissionPlan::append_zero_dependency_tasks() {
  for (const auto& task : compute_plan_.task_graph.tasks) {
    append_initial_task_handle(task.task_id);
  }
}

TaskHandle TaskSubmissionPlan::make_handle(int task_id) const {
  if (task_id < 0 ||
      task_id >= static_cast<int>(compute_plan_.task_graph.tasks.size())) {
    return {};
  }
  const PlannedTask& task = compute_plan_.task_graph.tasks[task_id];
  return TaskHandle{const_cast<TaskSubmissionPlan*>(this), task_id,
                    task.node_id};
}

/**
 * @brief Submits one planned high-precision scheduler task graph.
 *
 * @param graph GraphModel used to validate target cache state for empty plans.
 * @param task_runtime Scheduler runtime receiving initial task handles.
 * @param node_id Request target node id used in empty-plan diagnostics.
 * @param plan Built task submission plan whose task handles are submitted.
 * @throws GraphError when no scheduler tasks were planned and the target has
 * no reusable HP output; otherwise rethrows scheduler submission or task
 * completion failures.
 * @note This function is the planned-dispatch boundary. It intentionally
 * avoids recursive ComputeService execution.
 */
void dispatch_planned_tasks(GraphModel& graph,
                            SchedulerTaskRuntime& task_runtime, int node_id,
                            TaskSubmissionPlan& plan) {
  if (plan.empty() && graph.has_node(node_id)) {
    if (!graph.node(node_id).cached_output_high_precision.has_value()) {
      throw GraphError(
          GraphErrc::ComputeError,
          "Planned dispatch produced no scheduler tasks for node " +
              std::to_string(node_id) +
              " and the target has no reusable high-precision output.");
    }
    return;
  }

  std::vector<TaskHandle> initial_task_handles =
      plan.take_initial_task_handles();
  task_runtime.submit_initial_task_handles(std::move(initial_task_handles),
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
