#include "compute/dirty_execution_common.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "compute/compute_geometry.hpp"
#include "compute/resource_demand_estimator.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {

/** @copydoc DirtyNodeSynchronization::DirtyNodeSynchronization */
DirtyNodeSynchronization::DirtyNodeSynchronization(
    const std::vector<int>& node_ids) {
  node_mutexes_.reserve(node_ids.size());
  for (int node_id : node_ids) {
    if (node_mutexes_.find(node_id) == node_mutexes_.end()) {
      node_mutexes_.emplace(node_id, std::make_unique<std::mutex>());
    }
  }
}

/** @copydoc DirtyNodeSynchronization::mutex_for */
std::mutex& DirtyNodeSynchronization::mutex_for(int node_id) const {
  return *node_mutexes_.at(node_id);
}

/** @copydoc DirtyNodeSynchronization::retained_memory_bytes */
std::uint64_t DirtyNodeSynchronization::retained_memory_bytes() const {
  RetainedMemoryEstimator estimate("DirtyNodeSynchronization");
  const std::uint64_t bucket_count =
      static_cast<std::uint64_t>(node_mutexes_.bucket_count());
  const std::uint64_t node_count =
      static_cast<std::uint64_t>(node_mutexes_.size());
  estimate.add_objects<DirtyNodeSynchronization>();
  estimate.add_shared_control_block();
  estimate.add_objects<void*>(bucket_count);
  estimate.add_objects<decltype(node_mutexes_)::value_type>(node_count);
  estimate.add_objects<void*>(node_count);
  estimate.add_objects<void*>(node_count);
  estimate.add_objects<std::mutex>(node_count);
  return estimate.bytes();
}

/** @copydoc DirtyReadyTaskContext::DirtyReadyTaskContext */
DirtyReadyTaskContext::DirtyReadyTaskContext(
    const ComputePlan& compute_plan, const DirtyTaskSelectionOverlay* selection,
    const std::vector<int>& active_task_ids,
    const std::vector<Device>& task_devices, std::function<void(int)> run_task,
    std::uint64_t run_task_retained_memory_bytes, ComputeRunLease lease,
    bool release_dependents, ExecutionTaskPriority priority)
    : compute_plan_(compute_plan),
      selection_(selection
                     ? std::optional<DirtyTaskSelectionOverlay>(*selection)
                     : std::nullopt),
      active_task_ids_(active_task_ids),
      task_devices_(task_devices),
      active_task_id_set_(active_task_ids.begin(), active_task_ids.end()),
      run_task_(std::move(run_task)),
      run_task_retained_memory_bytes_(run_task_retained_memory_bytes),
      lease_(std::move(lease)),
      release_dependents_(release_dependents),
      priority_(priority) {
  if (!run_task_) {
    throw std::invalid_argument(
        "DirtyReadyTaskContext requires an owned task callable.");
  }
  if (task_devices_.size() != compute_plan_.task_graph.tasks.size()) {
    throw std::invalid_argument(
        "DirtyReadyTaskContext requires one device per planned task.");
  }
  if (selection_) {
    dependency_state_ = std::make_unique<TaskDependencyState>(
        compute_plan_.execution_order, compute_plan_.task_graph,
        active_task_ids_, selection_->dependency_task_ids);
  } else {
    dependency_state_ = std::make_unique<TaskDependencyState>(
        compute_plan_.execution_order, compute_plan_.task_graph,
        active_task_ids_);
  }
}

/** @copydoc DirtyReadyTaskContext::retained_memory_bytes */
std::uint64_t DirtyReadyTaskContext::retained_memory_bytes() const {
  RetainedMemoryEstimator estimate("DirtyReadyTaskContext");
  estimate.add_objects<DirtyReadyTaskContext>();
  estimate.add_shared_control_block();
  estimate.add_bytes(compute_plan_dynamic_retained_memory_bytes(compute_plan_));
  if (selection_.has_value()) {
    estimate.add_bytes(
        dirty_selection_dynamic_retained_memory_bytes(*selection_));
  }
  estimate.add_objects<int>(
      static_cast<std::uint64_t>(active_task_ids_.capacity()));
  estimate.add_objects<Device>(
      static_cast<std::uint64_t>(task_devices_.capacity()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(active_task_id_set_.bucket_count()));
  estimate.add_objects<decltype(active_task_id_set_)::value_type>(
      static_cast<std::uint64_t>(active_task_id_set_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(active_task_id_set_.size()));
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(active_task_id_set_.size()));
  if (dependency_state_) {
    estimate.add_objects<TaskDependencyState>();
    estimate.add_bytes(dependency_state_->dynamic_retained_memory_bytes());
  }
  estimate.add_bytes(run_task_retained_memory_bytes_);
  return estimate.bytes();
}

/** @copydoc DirtyReadyTaskContext::run_resource_demand */
CpuRunResourceDemand DirtyReadyTaskContext::run_resource_demand(
    std::uint64_t additional_shared_retained_memory_bytes) const {
  RetainedMemoryEstimator shared("dirty service phase");
  shared.add_bytes(retained_memory_bytes());
  shared.add_bytes(lease_.retained_memory_bytes());
  shared.add_bytes(additional_shared_retained_memory_bytes);
  return CpuRunResourceDemand{
      shared.bytes(), owned_callback_resource_demand(static_cast<std::uint64_t>(
                          sizeof(std::shared_ptr<DirtyReadyTaskContext>)))};
}

/** @copydoc DirtyReadyTaskContext::make_submissions */
std::vector<ReadyTaskSubmission> DirtyReadyTaskContext::make_submissions(
    const std::vector<int>& task_ids, bool initial_ready) {
  std::vector<ReadyTaskSubmission> submissions;
  submissions.reserve(task_ids.size());
  const std::shared_ptr<DirtyReadyTaskContext> self = shared_from_this();
  for (int task_id : task_ids) {
    if (task_id < 0 ||
        task_id >= static_cast<int>(compute_plan_.task_graph.tasks.size()) ||
        active_task_id_set_.count(task_id) == 0U) {
      throw std::invalid_argument(
          "Dirty ready submission names an inactive task.");
    }
    const PlannedTask& task = compute_plan_.task_graph.tasks.at(task_id);
    ComputeRunLease submission_lease = lease_;
    const ComputeRunTaskIdentity identity =
        submission_lease.task_identity(static_cast<uint64_t>(task_id));
    submissions.emplace_back(
        std::move(submission_lease), identity, task.node_id, initial_ready,
        [self](ComputeRunLease& lease,
               const ComputeRunTaskIdentity& accepted_identity,
               ExecutionTaskRuntime& task_runtime) {
          self->execute(lease, accepted_identity, task_runtime);
        },
        priority_,
        owned_callback_resource_demand(
            static_cast<std::uint64_t>(sizeof(self))),
        task_devices_.at(static_cast<std::size_t>(task_id)));
  }
  return submissions;
}

/** @copydoc DirtyReadyTaskContext::execute */
void DirtyReadyTaskContext::execute(ComputeRunLease& lease,
                                    const ComputeRunTaskIdentity& identity,
                                    ExecutionTaskRuntime& task_runtime) {
  if (identity.run_id() != lease.descriptor().id() ||
      identity.run_id() != lease_.descriptor().id()) {
    throw std::invalid_argument(
        "Dirty ready task identity does not match its Run lease.");
  }
  const uint64_t local_value = identity.local_task_id().value();
  if (local_value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("Dirty ready task id exceeds int range.");
  }
  const int task_id = static_cast<int>(local_value);
  if (task_id < 0 ||
      task_id >= static_cast<int>(compute_plan_.task_graph.tasks.size()) ||
      active_task_id_set_.count(task_id) == 0U) {
    throw std::invalid_argument(
        "Dirty ready task identity is not active in this Run phase.");
  }

  const PlannedTask& task = compute_plan_.task_graph.tasks.at(task_id);
  try {
    if (lease.observe_cancellation().has_value()) {
      task_runtime.dec_tasks_to_complete();
      return;
    }
    run_task_(task_id);
    if (lease.observe_cancellation().has_value()) {
      task_runtime.dec_tasks_to_complete();
      return;
    }
    if (release_dependents_) {
      const std::vector<int> ready_ids =
          dependency_state_->release_dependents(task_id);
      std::vector<ReadyTaskSubmission> ready_submissions =
          make_submissions(ready_ids, false);
      auto* ready_runtime =
          dynamic_cast<ReadyTaskSubmissionRuntime*>(&task_runtime);
      if (ready_runtime == nullptr) {
        throw std::logic_error(
            "Dirty owned context requires a ready-submission runtime.");
      }
      for (ReadyTaskSubmission& submission : ready_submissions) {
        if (lease.observe_cancellation().has_value()) {
          break;
        }
        ready_runtime->submit_ready_submission(std::move(submission));
      }
    }
    task_runtime.dec_tasks_to_complete();
  } catch (...) {
    try {
      task_runtime.log_event(ExecutionTraceAction::RethrowException,
                             task.node_id);
    } catch (...) {
    }
    throw;
  }
}

void remember_dirty_snapshot(GraphModel& graph,
                             const DirtyRegionSnapshot& snapshot) {
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  if (graph.recent_dirty_region_snapshots.size() > 16) {
    graph.recent_dirty_region_snapshots.erase(
        graph.recent_dirty_region_snapshots.begin());
  }
}

void remember_compute_plan(GraphModel& graph, const ComputePlan& compute_plan,
                           const DirtyTaskSelectionOverlay* selection) {
  graph.last_compute_plan = compute_plan;
  graph.last_compute_plan_summary =
      summarize_compute_plan(graph, compute_plan, selection);
  graph.recent_compute_plan_summaries.push_back(
      *graph.last_compute_plan_summary);
  if (graph.recent_compute_plan_summaries.size() > 16) {
    graph.recent_compute_plan_summaries.erase(
        graph.recent_compute_plan_summaries.begin());
  }
}

ComputePlan prune_node_cache_task_graph(
    GraphModel& graph, const ComputeRequest& request,
    const std::vector<int>& execution_order) {
  NodeCacheTaskGraphPruner node_cache_pruner;
  const std::shared_ptr<const FullTaskGraph> full_graph =
      get_or_expand_full_task_graph(graph, request.intent);
  return node_cache_pruner.prune(*full_graph, request, execution_order, graph);
}

ComputePlan prune_dirty_snapshot_task_graph(const ComputePlan& node_cache_plan,
                                            const DirtyRegionSnapshot& snapshot,
                                            const GraphModel& graph) {
  DirtySnapshotTaskGraphPruner dirty_snapshot_pruner;
  return dirty_snapshot_pruner.prune(node_cache_plan, snapshot, graph);
}

std::vector<int> planned_nodes_for_task_ids(const ComputePlan& compute_plan,
                                            const std::vector<int>& task_ids) {
  std::vector<int> node_ids;
  std::unordered_set<int> selected_node_ids;
  node_ids.reserve(task_ids.size());
  selected_node_ids.reserve(task_ids.size());
  for (int task_id : task_ids) {
    if (task_id < 0 ||
        task_id >= static_cast<int>(compute_plan.task_graph.tasks.size())) {
      throw std::out_of_range("Dirty phase task id is outside ComputePlan.");
    }
    const int node_id = compute_plan.task_graph.tasks.at(task_id).node_id;
    if (selected_node_ids.insert(node_id).second) {
      node_ids.push_back(node_id);
    }
  }
  return node_ids;
}

void validate_dirty_source_boundaries_ready(const GraphModel& graph,
                                            const DirtyRegionSnapshot& snapshot,
                                            DirtyDomain domain) {
  for (int source_node_id : snapshot.dirty_source_nodes) {
    const Node* source = graph.find_node(source_node_id);
    if (!source) {
      throw GraphError(GraphErrc::NotFound, "Dirty source node " +
                                                std::to_string(source_node_id) +
                                                " not found.");
    }
    (void)domain;
    const NodeOutput* output = ComputeCachePolicy::reusable_output(*source);
    if (!output) {
      throw GraphError(GraphErrc::MissingDependency,
                       "Dirty source boundary output is not ready for node " +
                           std::to_string(source_node_id) + ".");
    }
  }
}

bool is_dirty_source_node(const DirtyRegionSnapshot& snapshot, int node_id) {
  return std::find(snapshot.dirty_source_nodes.begin(),
                   snapshot.dirty_source_nodes.end(),
                   node_id) != snapshot.dirty_source_nodes.end();
}

void log_dirty_node_execution(GraphRuntime* runtime, int node_id,
                              bool dirty_source) {
  if (!runtime) {
    return;
  }
  runtime->log_event(GraphRuntime::ExecutionEvent::EXECUTE, node_id);
  runtime->log_event(
      dirty_source
          ? GraphRuntime::ExecutionEvent::EXECUTE_DIRTY_SOURCE
          : GraphRuntime::ExecutionEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE,
      node_id);
}

bool should_skip_stale_dirty_source(GraphRuntime* runtime, int node_id,
                                    uint64_t committed_generation,
                                    uint64_t dirty_generation) {
  if (committed_generation <= dirty_generation) {
    return false;
  }
  if (runtime) {
    runtime->log_event(GraphRuntime::ExecutionEvent::SKIP_STALE_GENERATION,
                       node_id);
  }
  return true;
}

/**
 * @brief Resolves the output format for a dirty-domain staging buffer.
 * @param preferred Existing staged output preferred when it has a valid image.
 * @param image_inputs Destination-indexed image inputs, including null slots.
 * @param fallback Optional committed output used after staged/input candidates.
 * @return Channel count and data type from the first usable candidate, or the
 * single-channel FLOAT32 default.
 * @throws Nothing.
 * @note Disconnected input placeholders are skipped only for format inference;
 * their slot identity remains intact for operation execution.
 */
std::pair<int, DataType> infer_output_spec(
    const std::optional<NodeOutput>& preferred,
    const std::vector<const NodeOutput*>& image_inputs,
    const std::optional<NodeOutput>* fallback) {
  if (preferred) {
    const auto& buffer = preferred->image_buffer;
    if (buffer.width > 0 && buffer.height > 0 && buffer.channels > 0) {
      return {buffer.channels, buffer.type};
    }
  }
  for (const auto* input : image_inputs) {
    if (!input) {
      continue;
    }
    const auto& buffer = input->image_buffer;
    if (buffer.width > 0 && buffer.height > 0 && buffer.channels > 0) {
      return {buffer.channels, buffer.type};
    }
  }
  if (fallback && *fallback) {
    const auto& buffer = (*fallback)->image_buffer;
    if (buffer.width > 0 && buffer.height > 0 && buffer.channels > 0) {
      return {buffer.channels, buffer.type};
    }
  }
  return {1, DataType::FLOAT32};
}

void apply_planned_work_rois(std::unordered_map<int, HpPlanEntry>& entries,
                             const DirtyTaskSelectionOverlay& selection) {
  for (const auto& [node_id, node_selection] : selection.node_selections) {
    auto entry_it = entries.find(node_id);
    if (entry_it == entries.end()) {
      continue;
    }
    if (!is_rect_empty(node_selection.represented_hp_roi)) {
      entry_it->second.roi_hp = clip_rect(node_selection.represented_hp_roi,
                                          entry_it->second.hp_size);
    }
  }
}

void apply_planned_work_rois(std::unordered_map<int, RtPlanEntry>& entries,
                             const DirtyTaskSelectionOverlay& selection) {
  for (const auto& [node_id, node_selection] : selection.node_selections) {
    auto entry_it = entries.find(node_id);
    if (entry_it == entries.end()) {
      continue;
    }
    if (!is_rect_empty(node_selection.represented_hp_roi)) {
      entry_it->second.roi_hp = clip_rect(node_selection.represented_hp_roi,
                                          entry_it->second.hp_size);
    }
    if (!is_rect_empty(node_selection.execution_roi)) {
      entry_it->second.roi_rt =
          clip_rect(node_selection.execution_roi, entry_it->second.rt_size);
    }
  }
}

}  // namespace ps::compute
