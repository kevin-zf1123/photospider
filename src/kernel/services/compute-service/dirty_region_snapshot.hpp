#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ps_types.hpp"

namespace ps::compute {

enum class DirtyDomain {
  HighPrecision,
  RealTime,
};

enum class DirtyTileLevel {
  Micro,
  Macro,
};

enum class DirtyEdgeDirection {
  ForwardAffected,
  BackwardDemand,
};

enum class DirtySourceLifecycleState {
  Idle,
  Updating,
  Settled,
};

struct DirtySourceRoiRecord {
  int node_id = -1;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  cv::Rect source_roi;
  uint64_t generation = 0;
};

struct DirtySourceNodeState {
  int node_id = -1;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  DirtySourceLifecycleState lifecycle = DirtySourceLifecycleState::Idle;
  uint64_t generation = 0;
  std::vector<cv::Rect> source_rois;
};

struct DirtyTileKey {
  int node_id = -1;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  DirtyTileLevel level = DirtyTileLevel::Micro;
  int tile_x = 0;
  int tile_y = 0;
  int tile_size = 0;
  cv::Rect pixel_roi;
};

struct DirtyMonolithicRegion {
  int node_id = -1;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  cv::Rect pixel_roi;
  bool whole_output = true;
};

struct DirtyEdgeMapping {
  int from_node_id = -1;
  int to_node_id = -1;
  DirtyDomain domain = DirtyDomain::HighPrecision;
  cv::Rect from_roi;
  cv::Rect to_roi;
  DirtyEdgeDirection direction = DirtyEdgeDirection::BackwardDemand;
};

struct DirtyRegionSnapshot {
  uint64_t graph_generation = 0;
  std::vector<int> dirty_source_nodes;
  std::unordered_map<int, DirtySourceNodeState> dirty_source_state;
  std::unordered_map<int, std::vector<DirtySourceRoiRecord>> source_roi_records;
  size_t dirty_updating_count = 0;
  std::vector<DirtyTileKey> dirty_tiles;
  std::vector<DirtyMonolithicRegion> dirty_monolithic_nodes;
  std::unordered_map<int, std::vector<cv::Rect>> per_node_dirty_rois;
  std::unordered_map<int, std::vector<cv::Rect>> actual_dirty_rois;
  std::vector<DirtyEdgeMapping> edge_mappings;

  bool empty() const;
};

}  // namespace ps::compute
