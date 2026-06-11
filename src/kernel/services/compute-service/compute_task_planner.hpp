#pragma once

#include <optional>
#include <vector>

#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "ps_types.hpp"

namespace ps::compute {

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
};

class ComputeTaskPlanner {
 public:
  ComputePlan plan(const ComputeRequest& request,
                   const std::vector<int>& execution_order,
                   const DirtyRegionSnapshot* snapshot = nullptr) const;
};

}  // namespace ps::compute
