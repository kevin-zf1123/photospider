#pragma once

#include <opencv2/core.hpp>
#include <unordered_map>

#include "graph_model.hpp"

namespace ps {

class GraphExtentResolver {
 public:
  cv::Size resolve_output_extent(
      const GraphModel& graph, int node_id,
      std::unordered_map<int, cv::Size>& cache) const;
};

}  // namespace ps
