#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "graph_model.hpp"
#include "ps_types.hpp"

namespace ps::compute {

struct TiledExecutionConfig {
  int tile_size = 256;
  int halo_size = 16;
  std::optional<cv::Rect> output_roi;
  std::optional<cv::Size> output_size;
  std::optional<int> forced_halo;
  std::optional<OpMetadata> metadata;
  std::function<void(const cv::Rect&)> on_tile;
};

struct TiledInputContext {
  std::vector<NodeOutput> normalized_storage;
  std::vector<const NodeOutput*> inputs;
};

class NodeExecutor {
 public:
  static NodeOutput execute(GraphModel& graph, Node& node,
                            const OpRegistry::OpVariant& op,
                            const std::vector<const NodeOutput*>& inputs,
                            const TiledExecutionConfig& config = {});

  static TiledInputContext normalize_tiled_inputs(
      Node& node, const std::vector<const NodeOutput*>& inputs);

  static void execute_tiled_into(
      GraphModel& graph, Node& node, const TileOpFunc& tiled_op,
      const std::vector<const NodeOutput*>& inputs, ImageBuffer& output_buffer,
      const TiledExecutionConfig& config = {});

  static cv::Rect input_roi_for_tile(GraphModel& graph, const Node& node,
                                     const cv::Rect& output_roi,
                                     const ImageBuffer& input_buffer,
                                     const TiledExecutionConfig& config);

  static void execute_tile_task(const TileTask& task,
                                const TileOpFunc& tiled_op);
};

}  // namespace ps::compute
