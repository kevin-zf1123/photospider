#include "compute/dirty_execution_common.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "compute/compute_cache_policy.hpp"
#include "compute/compute_geometry.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {

/** @copydoc ensure_running_scheduler */
IScheduler& ensure_running_scheduler(GraphRuntime& runtime,
                                     ComputeIntent intent) {
  IScheduler* scheduler = runtime.get_scheduler(intent);
  if (!scheduler) {
    throw GraphError(GraphErrc::ComputeError,
                     "No scheduler registered for requested compute intent.");
  }
  if (!scheduler->is_running()) {
    scheduler->start();
  }
  return *scheduler;
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
  std::unordered_set<int> selected_task_ids(task_ids.begin(), task_ids.end());
  std::vector<int> node_ids;
  std::unordered_set<int> selected_node_ids;
  for (const auto& work : compute_plan.planned_work) {
    bool selected = false;
    for (int task_id : work.task_ids) {
      if (selected_task_ids.count(task_id)) {
        selected = true;
        break;
      }
    }
    if (selected && selected_node_ids.insert(work.node_id).second) {
      node_ids.push_back(work.node_id);
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
  runtime->log_event(GraphRuntime::SchedulerEvent::EXECUTE, node_id);
  runtime->log_event(
      dirty_source
          ? GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_SOURCE
          : GraphRuntime::SchedulerEvent::EXECUTE_DIRTY_DOWNSTREAM_NODE,
      node_id);
}

bool should_skip_stale_dirty_source(GraphRuntime* runtime, int node_id,
                                    uint64_t committed_generation,
                                    uint64_t dirty_generation) {
  if (committed_generation <= dirty_generation) {
    return false;
  }
  if (runtime) {
    runtime->log_event(GraphRuntime::SchedulerEvent::SKIP_STALE_GENERATION,
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
