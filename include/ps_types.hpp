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
    int end_c =
        std::clamp((downstream_roi.x + downstream_roi.width - 1) / grid_size_x,
                   0, cols - 1);
    int end_r =
        std::clamp((downstream_roi.y + downstream_roi.height - 1) / grid_size_y,
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

// [M3.1] 扩展 OpMetadata 以支持调度权重
struct OpMetadata {
  TileSizePreference tile_preference = TileSizePreference::UNDEFINED;
  // [新增] 设备偏好字段，默认为 CPU
  Device device_preference = Device::CPU;
  // [M3.1 新增] 启发式调度权重，用于算子选择
  // 值越小表示优先级越高（成本越低）
  int cost_score = 100;

  enum class InputAccessPattern {
    SpatialAligned,
    RandomAccess,
  };
  InputAccessPattern access_pattern = InputAccessPattern::SpatialAligned;
  bool data_dependent = false;
};

using MonolithicOpFunc = std::function<NodeOutput(
    const Node&, const std::vector<const NodeOutput*>&)>;

/**
 * @brief Tiled operator callback signature.
 *
 * Tiled callbacks receive the node being executed, one writable output tile,
 * and all read-only input tile views needed to produce that output ROI. The
 * callback owns no buffers; all tile views are borrowed from NodeExecutor for
 * the duration of the call.
 *
 * @note InputTile carries const ImageBuffer pointers so tiled operators cannot
 * replace or mutate upstream ImageBuffer metadata through the tile API. Pixel
 * data returned by adapter views must still be treated as read-only by
 * convention when sourced from InputTile.
 */
using TileOpFunc = std::function<void(const Node&, const OutputTile&,
                                      const std::vector<InputTile>&)>;
using DirtyRoiPropFunc =
    std::function<cv::Rect(const Node&, const cv::Rect&, const GraphModel&)>;
using ForwardRoiPropFunc = std::function<cv::Rect(
    const Node&, const cv::Rect&, const GraphModel&,
    const cv::Size& parent_size, const cv::Size& child_size)>;
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

enum class PropagationContractStatus {
  Explicit,
  LegacyIdentityFallback,
};

// [M3.1 新增] 单个算子实现的描述结构
// 用于支持同一算子在不同设备上的多个实现
struct OpImplementation {
  // 统一函数签名，支持 Monolithic 和 Tiled 两种形式
  std::variant<MonolithicOpFunc, TileOpFunc> func;
  // 该实现的元数据（包含设备、成本等信息）
  OpMetadata metadata;

  // 辅助方法：判断是否为 Monolithic 实现
  bool is_monolithic() const {
    return std::holds_alternative<MonolithicOpFunc>(func);
  }

  // 辅助方法：判断是否为 Tiled 实现
  bool is_tiled() const { return std::holds_alternative<TileOpFunc>(func); }
};

class OpRegistry {
 public:
  static OpRegistry& instance();

  using OpVariant = std::variant<MonolithicOpFunc, TileOpFunc>;

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

    // [M3.1 新增] 多设备实现列表
    // 同一算子可以有多个设备上的实现，按设备类型索引
    std::vector<OpImplementation> device_impls;
  };

  /**
   * @brief Previous registry state for one canonical operation key.
   *
   * The snapshot records all registry tables that may hold callbacks for a
   * key. Empty optionals mean the key had no entry in that table before a
   * plugin registration touched it.
   */
  struct RegistryEntrySnapshot {
    std::optional<OpVariant> legacy_op;
    std::optional<OpMetadata> metadata;
    std::optional<OpImplementations> implementations;
  };

  /**
   * @brief Captured key mutations made by one plugin registration call.
   *
   * `registered_keys` contains every canonical key touched through a
   * registration API, including replacements of existing keys. The snapshot
   * map stores each touched key's pre-registration state so plugin unload can
   * restore replaced operations before releasing the dynamic library.
   */
  struct RegistrationCapture {
    std::vector<std::string> registered_keys;
    std::unordered_map<std::string, RegistryEntrySnapshot> previous_entries;
  };

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
  /**
   * @brief Removes all registered implementations for one operation name.
   *
   * The method converts `type` and `subtype` into the canonical
   * `type:subtype` registry key, then removes legacy, metadata, and
   * multi-implementation entries for that key.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"gaussian_blur"`.
   * @return True when at least one registry entry was removed.
   * @throws Nothing under current container erase behavior.
   * @note Dynamic plugin unload relies on this method to remove callbacks
   * before releasing the plugin library handle.
   */
  bool unregister_op(const std::string& type, const std::string& subtype);

  /**
   * @brief Removes all registry state stored under one canonical operation key.
   *
   * This clears `table_`, `metadata_table_`, and `impl_table_` entries so both
   * legacy `register_op` callbacks and newer HP/RT/tiled/device implementations
   * are removed together.
   *
   * @param key Canonical operation key in `type:subtype` form.
   * @return True when at least one legacy, metadata, or multi-implementation
   * entry was erased.
   * @throws Nothing under current container erase behavior.
   * @note The method deliberately removes the entire multi-implementation group
   * for the key; partial per-device unload is not part of the current ABI.
   */
  bool unregister_key(const std::string& key);

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
  PropagationContractStatus dirty_propagation_contract_status(
      const std::string& type, const std::string& subtype) const;
  PropagationContractStatus forward_propagation_contract_status(
      const std::string& type, const std::string& subtype) const;
  std::optional<DependencyLutBuilder> get_dependency_builder(
      const std::string& type, const std::string& subtype) const;
  bool is_data_dependent(const std::string& type,
                         const std::string& subtype) const;
  const OpImplementations* get_implementations(
      const std::string& type, const std::string& subtype) const;

  // ==========================================================================
  // [M3.1 新增] 多设备实现注册与检索 API
  // ==========================================================================

  // 注册一个特定设备上的算子实现
  // @param type 算子类型（如 "image_process"）
  // @param subtype 算子子类型（如 "gaussian_blur"）
  // @param device 目标设备
  // @param fn 算子实现函数
  // @param meta 元数据（cost_score 用于调度优先级）
  void register_impl(const std::string& type, const std::string& subtype,
                     Device device, MonolithicOpFunc fn, OpMetadata meta = {});
  void register_impl(const std::string& type, const std::string& subtype,
                     Device device, TileOpFunc fn, OpMetadata meta);

  // 获取指定算子在特定设备上的所有实现
  // @return 该设备上的实现列表（可能为空）
  std::vector<const OpImplementation*> get_implementations_by_device(
      const std::string& type, const std::string& subtype, Device device) const;

  // 获取指定算子的所有实现（跨所有设备）
  // @return 所有实现的列表
  std::vector<const OpImplementation*> get_all_implementations(
      const std::string& type, const std::string& subtype) const;

  /**
   * @brief Selects the best registered implementation for an intent.
   *
   * The registry owns the intent-specific ordering policy. It first restricts
   * candidates to implementations whose `device_preference` is present in
   * `available_devices`, then orders the remaining candidates by
   * `ComputeIntent` device priority and `cost_score`.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"gaussian_blur"`.
   * @param available_devices Devices exposed by the active scheduler runtime.
   * @param intent Compute path selecting the HP or RT priority policy.
   * @return Pointer to the selected implementation, or nullptr when the key is
   * missing or no implementation can run on an available device.
   * @throws std::bad_alloc if temporary candidate storage allocation fails.
   * @note Returned pointers borrow storage owned by OpRegistry and remain valid
   * until the matching operation key is unregistered or the registry is mutated
   * in a way that reallocates that key's implementation vector.
   */
  const OpImplementation* select_best_implementation(
      const std::string& type, const std::string& subtype,
      const std::vector<Device>& available_devices, ComputeIntent intent) const;

  /**
   * @brief Selects the best registered implementation after caller filtering.
   *
   * This overload applies the same registry-owned device and intent ordering as
   * the four-argument overload, while allowing a caller to reject candidates
   * for local execution constraints such as task-shape compatibility. An empty
   * `candidate_filter` accepts every available-device candidate.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"gaussian_blur"`.
   * @param available_devices Devices exposed by the active scheduler runtime.
   * @param intent Compute path selecting the HP or RT priority policy.
   * @param candidate_filter Optional predicate; returning false removes the
   * candidate before registry ordering is applied.
   * @return Pointer to the selected implementation, or nullptr when the key is
   * missing or no available candidate survives filtering.
   * @throws std::bad_alloc if temporary candidate storage allocation fails.
   * @throws Any exception thrown by `candidate_filter`.
   * @note The filter must not mutate OpRegistry or retain references to
   * candidates beyond the call. Sorting policy remains centralized in
   * OpRegistry.
   */
  const OpImplementation* select_best_implementation(
      const std::string& type, const std::string& subtype,
      const std::vector<Device>& available_devices, ComputeIntent intent,
      const std::function<bool(const OpImplementation&)>& candidate_filter)
      const;

  /**
   * @brief Runs a registration callback while capturing touched keys.
   *
   * @param registration Callback that calls one or more OpRegistry register
   * APIs, usually a plugin's `register_photospider_ops` entry point.
   * @param capture Output capture receiving touched keys and prior table state.
   * @throws Any exception propagated by `registration`.
   * @note The capture is populated before each key mutation, so callers can
   * restore overwritten entries if registration fails or when a plugin unloads.
   */
  void capture_registration(const std::function<void()>& registration,
                            RegistrationCapture& capture);

  /**
   * @brief Restores registry entries saved by a registration capture.
   *
   * @param capture Capture whose previous entries should be restored.
   * @throws std::bad_alloc if restoring copied callbacks or metadata allocates.
   * @note Used by plugin load rollback and unload. Keys absent before the
   * capture are erased from all registry tables.
   */
  void restore_registration_capture(const RegistrationCapture& capture);

 private:
  /**
   * @brief Saves a key's previous state for the active registration capture.
   *
   * @param key Canonical operation key that is about to be mutated.
   * @throws std::bad_alloc if the capture needs to copy callbacks or grow.
   * @note No-op when registration capture is not active.
   */
  void capture_key_before_mutation(const std::string& key);

  /**
   * @brief Copies current registry table state for one key.
   *
   * @param key Canonical operation key to snapshot.
   * @return Snapshot of legacy, metadata, and multi-implementation tables.
   * @throws std::bad_alloc if copying callbacks or metadata allocates.
   */
  RegistryEntrySnapshot snapshot_entry(const std::string& key) const;

  /**
   * @brief Restores one key to a previous snapshot.
   *
   * @param key Canonical operation key to restore.
   * @param snapshot Previous table state for the key.
   * @throws std::bad_alloc if copying callbacks or metadata allocates.
   * @note Empty snapshot fields erase the corresponding table entries.
   */
  void restore_entry(const std::string& key,
                     const RegistryEntrySnapshot& snapshot);

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
