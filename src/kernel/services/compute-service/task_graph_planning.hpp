#pragma once

#include <optional>
#include <string>
#include <vector>

#include "kernel/services/compute-service/dirty_region_snapshot.hpp"
#include "ps_types.hpp"

namespace ps {
class GraphModel;
}

namespace ps::compute {

enum class PlannedTaskKind {
  Node,
  Tile,
  Monolithic,
};

struct PlannedDependency {
  int from_node_id = -1;
  int to_node_id = -1;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  std::string input_kind = "image";
  cv::Rect from_roi;
  cv::Rect to_roi;
  DirtyEdgeDirection direction = DirtyEdgeDirection::BackwardDemand;
};

struct PlannedTask {
  int task_id = -1;
  int node_id = -1;
  PlannedTaskKind kind = PlannedTaskKind::Node;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  cv::Rect output_roi;
  int tile_x = -1;
  int tile_y = -1;
  int tile_size = 0;
  bool whole_output = false;
  bool source_boundary_eligible = false;
  bool dirty_selected = false;
  uint64_t dirty_generation = 0;
  std::vector<int> dependency_task_ids;
};

struct PlannedNodeWork {
  int node_id = -1;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  cv::Rect represented_hp_roi;
  cv::Rect execution_roi;
  bool whole_output = false;
  bool reusable_cache_available = false;
  std::vector<cv::Rect> dirty_rois;
  std::vector<int> dependency_node_ids;
  std::vector<int> dependent_node_ids;
  std::vector<int> task_ids;
};

struct ComputeTaskGraph {
  std::vector<PlannedTask> tasks;
  std::vector<PlannedDependency> dependencies;
  std::vector<int> initial_task_ids;
};

struct DirtyUpdateWorkSet {
  uint64_t generation = 0;
  std::vector<int> dirty_source_task_ids;
  std::vector<int> downstream_task_ids;
};

struct ComputeRequest {
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  int target_node_id = -1;
  bool parallel = false;
  std::optional<cv::Rect> dirty_roi;
};

struct ComputePlan {
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  int target_node_id = -1;
  bool parallel = false;
  std::vector<int> execution_order;
  std::vector<int> planned_nodes;
  std::vector<PlannedNodeWork> planned_work;
  ComputeTaskGraph task_graph;
};

struct FullTaskGraph {
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  std::vector<int> expanded_node_ids;
  std::vector<PlannedNodeWork> expanded_work;
  ComputeTaskGraph task_graph;
};

class FullTaskGraphExpander {
 public:
  FullTaskGraph expand(const GraphModel& graph, ComputeIntent intent) const;
};

class NodeCacheTaskGraphPruner {
 public:
  ComputePlan prune(const FullTaskGraph& full_graph,
                    const ComputeRequest& request,
                    const std::vector<int>& execution_order,
                    const GraphModel& graph) const;
};

class DirtySnapshotTaskGraphPruner {
 public:
  ComputePlan prune(const ComputePlan& node_cache_plan,
                    const DirtyRegionSnapshot& snapshot) const;
  DirtyUpdateWorkSet materialize(const ComputePlan& plan,
                                 const DirtyRegionSnapshot& snapshot) const;
};

class TaskGraphReadyChecker {
 public:
  std::vector<int> initial_ready_task_ids(
      const ComputeTaskGraph& graph,
      const std::vector<int>* allowed_task_ids = nullptr) const;
};

}  // namespace ps::compute
