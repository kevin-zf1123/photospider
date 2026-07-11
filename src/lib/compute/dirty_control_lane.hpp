#pragma once

#include <cstddef>
#include <opencv2/core.hpp>
#include <optional>

#include "kernel/services/compute-service/dirty_region_snapshot.hpp"

namespace ps {

class GraphModel;
class GraphTraversalService;
class RoiPropagationService;

namespace compute {

enum class DirtyControlEvent { Begin, Update, End };

struct DirtyControlLaneResult {
  DirtyRegionSnapshot snapshot;
  DirtyControlEvent event = DirtyControlEvent::Update;
  uint64_t generation = 0;
  size_t dirty_updating_count = 0;
  bool should_wake_dispatcher = false;
  bool cutoff_after_downstream = false;
};

class DirtyControlLane {
 public:
  DirtyControlLane(GraphTraversalService& traversal,
                   RoiPropagationService& roi_propagation);

  DirtyControlLaneResult begin_dirty_source(GraphModel& graph, int node_id,
                                            DirtyDomain domain,
                                            const cv::Rect& source_roi) const;
  DirtyControlLaneResult update_dirty_source(GraphModel& graph, int node_id,
                                             DirtyDomain domain,
                                             const cv::Rect& source_roi) const;
  DirtyControlLaneResult end_dirty_source(GraphModel& graph, int node_id,
                                          DirtyDomain domain) const;

 private:
  DirtyControlLaneResult build_result(const DirtyRegionSnapshot& snapshot,
                                      DirtyControlEvent event) const;

  GraphTraversalService& traversal_;
  RoiPropagationService& roi_propagation_;
};

}  // namespace compute
}  // namespace ps
