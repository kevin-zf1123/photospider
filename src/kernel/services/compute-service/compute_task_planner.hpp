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
  std::vector<int> dependency_task_ids;
};

struct PlannedNodeWork {
  int node_id = -1;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  cv::Rect represented_hp_roi;
  cv::Rect execution_roi;
  bool whole_output = false;
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

class ComputeTaskPlanner {
 public:
  ComputePlan plan(const ComputeRequest& request,
                   const std::vector<int>& execution_order,
                   const DirtyRegionSnapshot* snapshot = nullptr,
                   const GraphModel* graph = nullptr) const;
};

}  // namespace ps::compute
