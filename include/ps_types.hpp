#pragma once
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <opencv2/opencv.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "image_buffer.hpp"

namespace ps {
namespace fs = std::filesystem;

using OutputValue = YAML::Node;

struct ImageInput {
  int from_node_id = -1;
  std::string from_output_name = "image";
};

struct ParameterInput {
  int from_node_id = -1;
  std::string from_output_name;
  std::string to_parameter_name;
};

struct OutputPort {
  int output_id = -1;
  std::string output_type;
  YAML::Node output_parameters;
};

struct CacheEntry {
  std::string cache_type;
  std::string location;
};

struct SpatialContext {
  std::array<double, 9> transform_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::array<double, 9> inverse_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::array<double, 9> local_inverse_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
  cv::Rect absolute_roi{0, 0, 0, 0};
  double global_scale_x{1.0};
  double global_scale_y{1.0};
};

struct DebugMeta {
  int computed_by_worker_id{-1};
  uint64_t timestamp_us{0};
  uint64_t execution_time_ms{0};
  double min_val{0.0};
  double max_val{0.0};
  bool has_nan{false};
  std::string compute_device{"UNKNOWN"};
};

struct NodeOutput {
  ps::ImageBuffer image_buffer;
  std::unordered_map<std::string, OutputValue> data;
  SpatialContext space;
  DebugMeta debug;
};

struct SpatialDependencyMap {
  int grid_size_x = 64;
  int grid_size_y = 64;
  int cols = 0;
  int rows = 0;
  cv::Size output_extent{};
  std::vector<cv::Rect> cell_to_upstream_roi;

  bool is_valid() const {
    return cols > 0 && rows > 0 && grid_size_x > 0 && grid_size_y > 0 &&
           output_extent.width > 0 && output_extent.height > 0 &&
           static_cast<int>(cell_to_upstream_roi.size()) == cols * rows;
  }

  bool is_valid_for(const cv::Size& extent) const {
    return is_valid() && output_extent == extent;
  }

  cv::Rect cell_bounds(int cx, int cy) const {
    if (cx < 0 || cy < 0 || cx >= cols || cy >= rows || grid_size_x <= 0 ||
        grid_size_y <= 0) {
      return cv::Rect();
    }
    int x0 = cx * grid_size_x;
    int y0 = cy * grid_size_y;
    int w = grid_size_x;
    int h = grid_size_y;
    if (output_extent.width > 0) {
      w = std::min(w, output_extent.width - x0);
    }
    if (output_extent.height > 0) {
      h = std::min(h, output_extent.height - y0);
    }
    if (w <= 0 || h <= 0)
      return cv::Rect();
    return cv::Rect(x0, y0, w, h);
  }

  static cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) {
    if (a.width <= 0 || a.height <= 0)
      return b;
    if (b.width <= 0 || b.height <= 0)
      return a;
    int x0 = std::min(a.x, b.x);
    int y0 = std::min(a.y, b.y);
    int x1 = std::max(a.x + a.width, b.x + b.width);
    int y1 = std::max(a.y + a.height, b.y + b.height);
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
  }

  cv::Rect lookup(const cv::Rect& downstream_roi) const {
    if (!is_valid() || downstream_roi.width <= 0 || downstream_roi.height <= 0)
      return cv::Rect();
    int start_c = std::clamp(downstream_roi.x / grid_size_x, 0, cols - 1);
    int start_r = std::clamp(downstream_roi.y / grid_size_y, 0, rows - 1);
    int end_c = std::clamp((downstream_roi.x + downstream_roi.width - 1) /
                               grid_size_x,
                           0, cols - 1);
    int end_r = std::clamp((downstream_roi.y + downstream_roi.height - 1) /
                               grid_size_y,
                           0, rows - 1);

    cv::Rect merged;
    for (int r = start_r; r <= end_r; ++r) {
      for (int c = start_c; c <= end_c; ++c) {
        int idx = r * cols + c;
        if (idx < 0 || idx >= static_cast<int>(cell_to_upstream_roi.size()))
          continue;
        merged = merge_rect(merged, cell_to_upstream_roi[idx]);
      }
    }
    return merged;
  }
};

#if defined(_WIN32)
#if defined(PHOTOSPIDER_LIB_BUILD)
#define PHOTOSPIDER_API __declspec(dllexport)
#else
#define PHOTOSPIDER_API __declspec(dllimport)
#endif
#else  // Non-Windows platforms
#if defined(PHOTOSPIDER_LIB_BUILD)
#define PHOTOSPIDER_API __attribute__((visibility("default")))
#else
#define PHOTOSPIDER_API
#endif
#endif

enum class GraphErrc {
  Unknown = 1,
  NotFound,
  Cycle,
  Io,
  InvalidYaml,
  MissingDependency,
  NoOperation,
  InvalidParameter,
  ComputeError,
};
struct PHOTOSPIDER_API GraphError : public std::runtime_error {
  explicit GraphError(const std::string& what)
      : std::runtime_error(what), code_(GraphErrc::Unknown) {}
  GraphError(GraphErrc code, const std::string& what)
      : std::runtime_error(what), code_(code) {}
  GraphErrc code() const noexcept { return code_; }

 private:
  GraphErrc code_;
};

// struct GraphError : public std::runtime_error {
//     explicit GraphError(const std::string& what)
//         : std::runtime_error(what), code_(GraphErrc::Unknown) {}
//     GraphError(GraphErrc code, const std::string& what)
//         : std::runtime_error(what), code_(code) {}
//     GraphErrc code() const noexcept { return code_; }
// private:
//     GraphErrc code_;
// };

class Node;
class GraphModel;

// --- 新增: 定义操作元数据 ---
// 用于描述一个操作对分块大小的偏好
enum class TileSizePreference {
  UNDEFINED,  // 未定义或不适用 (例如 Monolithic 操作)
  MICRO,      // 偏好小分块 (例如 16x16)，适用于交互式、低延迟任务
  MACRO       // 偏好大分块 (例如 256x256)，适用于吞吐量优先的批处理任务
};

// [核心修复] 移除此处的 Device 枚举定义，因为它已在 image_buffer.hpp 中定义

struct OpMetadata {
  TileSizePreference tile_preference = TileSizePreference::UNDEFINED;
  // [新增] 新增 device_preference 字段，默认为 CPU
  Device device_preference = Device::CPU;

  enum class InputAccessPattern {
    SpatialAligned,
    RandomAccess,
  };
  InputAccessPattern access_pattern = InputAccessPattern::SpatialAligned;
  bool data_dependent = false;
};

using MonolithicOpFunc = std::function<NodeOutput(
    const Node&, const std::vector<const NodeOutput*>&)>;
using TileOpFunc =
    std::function<void(const Node&, const Tile&, const std::vector<Tile>&)>;
using DirtyRoiPropFunc =
    std::function<cv::Rect(const Node&, const cv::Rect&, const GraphModel&)>;
using ForwardRoiPropFunc =
    std::function<cv::Rect(const Node&, const cv::Rect&, const GraphModel&,
                           const cv::Size& parent_size,
                           const cv::Size& child_size)>;
using DependencyLutBuilder = std::function<SpatialDependencyMap(
    const Node&, const GraphModel&, const cv::Size& upstream_extent,
    const cv::Size& downstream_extent)>;

// -----------------------------------------------------------------------------
// Compute intent for planner/scheduler (Phase 1: API foundation)
// -----------------------------------------------------------------------------
enum class ComputeIntent {
  GlobalHighPrecision,
  RealTimeUpdate,
};

class OpRegistry {
 public:
  static OpRegistry& instance();

  using OpVariant = std::variant<MonolithicOpFunc, TileOpFunc>;

  // [修改] 重载 register_op 以接收元数据
  void register_op(const std::string& type, const std::string& subtype,
                   MonolithicOpFunc fn, OpMetadata meta = {});
  void register_op(const std::string& type, const std::string& subtype,
                   TileOpFunc fn, OpMetadata meta);  // Tiled 操作必须提供元数据

  std::optional<OpVariant> find(const std::string& type,
                                const std::string& subtype) const;

  // [新增] 获取元数据
  std::optional<OpMetadata> get_metadata(const std::string& type,
                                         const std::string& subtype) const;

  std::vector<std::string> get_keys() const;
  // Combined keys: collapse multiple implementations into a single op key
  // (type:subtype)
  std::vector<std::string> get_combined_keys() const;
  bool unregister_op(const std::string& type, const std::string& subtype);
  bool unregister_key(const std::string& key);

  // Phase 1 scaffolding: multi-implementation registry (not wired yet in
  // executor)
  struct OpImplementations {
    std::optional<MonolithicOpFunc> monolithic_hp;  // optional
    std::optional<TileOpFunc> tiled_hp;             // preferred available
    std::optional<TileOpFunc> tiled_rt;             // optional
    std::optional<OpMetadata> meta_hp;
    std::optional<OpMetadata> meta_rt;
    std::optional<DirtyRoiPropFunc> dirty_propagator;
    std::optional<ForwardRoiPropFunc> forward_propagator;
    std::optional<DependencyLutBuilder> dependency_builder;
    bool data_dependent = false;
  };

  void register_op_hp_monolithic(const std::string& type,
                                 const std::string& subtype,
                                 MonolithicOpFunc fn, OpMetadata meta = {});
  void register_op_hp_tiled(const std::string& type, const std::string& subtype,
                            TileOpFunc fn, OpMetadata meta);
  void register_op_rt_tiled(const std::string& type, const std::string& subtype,
                            TileOpFunc fn, OpMetadata meta);
  void register_dirty_propagator(const std::string& type,
                                 const std::string& subtype,
                                 DirtyRoiPropFunc fn);
  void register_forward_propagator(const std::string& type,
                                   const std::string& subtype,
                                   ForwardRoiPropFunc fn);
  void register_dependency_builder(const std::string& type,
                                   const std::string& subtype,
                                   DependencyLutBuilder fn,
                                   bool mark_data_dependent = true);
  std::optional<OpVariant> resolve_for_intent(const std::string& type,
                                              const std::string& subtype,
                                              ComputeIntent intent) const;
  DirtyRoiPropFunc get_dirty_propagator(const std::string& type,
                                        const std::string& subtype) const;
  ForwardRoiPropFunc get_forward_propagator(const std::string& type,
                                            const std::string& subtype) const;
  std::optional<DependencyLutBuilder> get_dependency_builder(
      const std::string& type, const std::string& subtype) const;
  bool is_data_dependent(const std::string& type,
                         const std::string& subtype) const;
  const OpImplementations* get_implementations(
      const std::string& type, const std::string& subtype) const;

 private:
  std::unordered_map<std::string, OpVariant> table_;
  // [修改] 元数据表现在可以存储包含设备偏好的完整 OpMetadata
  std::unordered_map<std::string, OpMetadata> metadata_table_;
  // New: consolidate multiple implementations under a single op key
  std::unordered_map<std::string, OpImplementations> impl_table_;
};

inline std::string make_key(const std::string& type,
                            const std::string& subtype) {
  return type + ":" + subtype;
}

}  // namespace ps
