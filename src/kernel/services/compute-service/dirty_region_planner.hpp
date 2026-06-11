#pragma once

#include <string>
#include <unordered_map>

#include "graph_model.hpp"
#include "kernel/services/compute-service/dirty_region_snapshot.hpp"

namespace ps {
class GraphTraversalService;
}

namespace ps::compute {

struct HpPlanEntry {
  cv::Rect roi_hp;
  cv::Size hp_size;
  int halo_hp = 0;
};

struct RtPlanEntry {
  cv::Rect roi_hp;
  cv::Rect roi_rt;
  cv::Size hp_size;
  cv::Size rt_size;
  int halo_hp = 0;
  int halo_rt = 0;
};

struct HighPrecisionDirtyPlan {
  std::vector<int> execution_order;
  std::unordered_map<int, HpPlanEntry> entries;
  DirtyRegionSnapshot snapshot;
};

struct RealTimeDirtyPlan {
  std::vector<int> execution_order;
  std::unordered_map<int, RtPlanEntry> entries;
  DirtyRegionSnapshot snapshot;
};

class DirtyRegionPlanner {
 public:
  explicit DirtyRegionPlanner(GraphTraversalService& traversal);

  HighPrecisionDirtyPlan plan_high_precision(GraphModel& graph, int node_id,
                                             const cv::Rect& dirty_roi);
  RealTimeDirtyPlan plan_real_time(GraphModel& graph, int node_id,
                                   const cv::Rect& dirty_roi);

  static std::string describe_snapshot(const DirtyRegionSnapshot& snapshot);

 private:
  cv::Size infer_hp_size(GraphModel& graph, int node_id,
                         std::unordered_map<int, cv::Size>& cache) const;
  int infer_halo_hp(const Node& node) const;
  bool is_monolithic_boundary(const Node& node) const;
  void enumerate_tiles(DirtyRegionSnapshot& snapshot, int node_id,
                       DirtyDomain domain, DirtyTileLevel level,
                       const cv::Rect& roi, int tile_size) const;

  GraphTraversalService& traversal_;
  uint64_t generation_counter_ = 0;
};

}  // namespace ps::compute
