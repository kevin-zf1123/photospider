// Photospider kernel: multi-graph Kernel facade
#pragma once

#include <map>
#include <memory>
#include <opencv2/opencv.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "kernel/graph_runtime.hpp"
#include "kernel/plugin_manager.hpp"
#include "kernel/services/compute_service.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_inspect_service.hpp"
#include "kernel/services/graph_io_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "kernel/services/roi_propagation_service.hpp"

namespace ps {

class Kernel {
 public:
  struct LastError {
    GraphErrc code = GraphErrc::Unknown;
    std::string message;
  };

  std::optional<std::string> load_graph(const std::string& name,
                                        const std::string& root_dir,
                                        const std::string& yaml_path,
                                        const std::string& config_path = "");

  bool close_graph(const std::string& name);
  std::vector<std::string> list_graphs() const;

  template <typename Fn>
  auto post(const std::string& name, Fn&& fn)
      -> std::future<decltype(fn(std::declval<GraphModel&>()))> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      throw std::runtime_error("Graph not found: " + name);
    }
    return it->second->post(std::forward<Fn>(fn));
  }

  bool compute(const std::string& name, int node_id,
               const std::string& cache_precision, bool force_recache,
               bool enable_timing, bool parallel, bool quiet,
               bool disable_disk_cache, bool nosave,
               std::vector<BenchmarkEvent>* benchmark_events = nullptr);
  std::optional<std::future<bool>> compute_async(
      const std::string& name, int node_id, const std::string& cache_precision,
      bool force_recache, bool enable_timing, bool parallel, bool quiet,
      bool disable_disk_cache, bool nosave,
      std::vector<BenchmarkEvent>* benchmark_events = nullptr);
  std::optional<TimingCollector> get_timing(const std::string& name);
  std::optional<std::future<bool>> compute_async(
      const std::string& name, int node_id, const std::string& cache_precision,
      bool force_recache, bool enable_timing, bool parallel, bool quiet,
      bool disable_disk_cache, bool nosave,
      std::vector<BenchmarkEvent>* benchmark_events,
      ComputeIntent intent,              // 新增
      std::optional<cv::Rect> dirty_roi  // 新增
  );
  std::optional<cv::Rect> project_roi_forward(const std::string& name,
                                              int start_node_id,
                                              const cv::Rect& start_roi,
                                              int target_node_id);
  std::optional<cv::Rect> project_roi_backward(const std::string& name,
                                               int target_node_id,
                                               const cv::Rect& target_roi,
                                               int source_node_id);
  bool reload_graph_yaml(const std::string& name, const std::string& yaml_path);
  bool save_graph_yaml(const std::string& name, const std::string& yaml_path);
  bool clear_drive_cache(const std::string& name);
  bool clear_memory_cache(const std::string& name);
  bool clear_cache(const std::string& name);
  bool cache_all_nodes(const std::string& name,
                       const std::string& cache_precision);
  bool free_transient_memory(const std::string& name);
  bool synchronize_disk_cache(const std::string& name,
                              const std::string& cache_precision);
  bool clear_graph(const std::string& name);

  std::optional<GraphModel::DriveClearResult> clear_drive_cache_stats(
      const std::string& name);
  std::optional<GraphModel::MemoryClearResult> clear_memory_cache_stats(
      const std::string& name);
  std::optional<GraphModel::CacheSaveResult> cache_all_nodes_stats(
      const std::string& name, const std::string& cache_precision);
  std::optional<GraphModel::MemoryClearResult> free_transient_memory_stats(
      const std::string& name);
  std::optional<GraphModel::DiskSyncResult> synchronize_disk_cache_stats(
      const std::string& name, const std::string& cache_precision);

  std::optional<DependencyTree> dependency_tree(const std::string& name,
                                                std::optional<int> node_id,
                                                bool include_metadata = false);
  std::optional<GraphNodeInspectInfo> inspect_node(const std::string& name,
                                                   int node_id);
  std::optional<GraphInspectionSnapshot> inspect_graph(const std::string& name);
  std::optional<LastError> last_error(const std::string& name) const;
  std::optional<std::vector<int>> ending_nodes(const std::string& name);
  std::optional<std::vector<int>> topo_postorder_from(const std::string& name,
                                                      int end_node_id);
  std::optional<std::map<int, std::vector<int>>> traversal_orders(
      const std::string& name);
  struct TraversalNodeInfo {
    int id;
    std::string name;
    bool has_memory_cache;
    bool has_disk_cache;
  };
  std::optional<std::map<int, std::vector<TraversalNodeInfo>>>
  traversal_details(const std::string& name);
  std::optional<std::vector<GraphEventService::ComputeEvent>>
  drain_compute_events(const std::string& name);
  std::optional<std::vector<GraphRuntime::SchedulerEvent>> scheduler_trace(
      const std::string& name);
  std::optional<std::string> dirty_region_snapshot_debug(
      const std::string& name);
  std::optional<compute::DirtyRegionSnapshot> dirty_region_snapshot(
      const std::string& name);
  std::optional<cv::Mat> compute_and_get_image(
      const std::string& name, int node_id, const std::string& cache_precision,
      bool force_recache, bool enable_timing, bool parallel,
      bool disable_disk_cache = false,
      std::vector<BenchmarkEvent>* benchmark_events = nullptr);

  std::optional<std::vector<int>> list_node_ids(const std::string& name);
  std::optional<std::string> get_node_yaml(const std::string& name,
                                           int node_id);
  bool set_node_yaml(const std::string& name, int node_id,
                     const std::string& yaml_text);

  std::optional<std::vector<int>> trees_containing_node(const std::string& name,
                                                        int node_id);

  PluginManager& plugins() { return plugin_mgr_; }
  std::optional<double> get_last_io_time(const std::string& name);

  // [新增] 暴露 Metal 设备访问器
  id get_metal_device(const std::string& name);

  GraphRuntime& runtime(const std::string& name) {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      throw std::runtime_error("Graph not found: " + name);
    }
    return *it->second;
  }

  // =========================================================================
  // [M3.4 新增] 调度器配置与管理 API
  // =========================================================================

  /// @brief 调度器配置结构
  struct SchedulerConfig {
    std::string hp_type = "cpu_work_stealing";  // HP 调度器类型
    std::string rt_type = "cpu_work_stealing";  // RT 调度器类型
    unsigned int worker_count = 0;              // 工作线程数（0=自动）
  };

  /// @brief 设置全局调度器配置（影响后续加载的图）
  /// @param config 调度器配置
  void set_scheduler_config(const SchedulerConfig& config);

  /// @brief 获取当前调度器配置
  const SchedulerConfig& get_scheduler_config() const;

  /// @brief 为指定图替换调度器
  /// @param name 图名称
  /// @param intent 计算意图 (HP/RT)
  /// @param type 调度器类型名称
  /// @return true 如果替换成功
  bool replace_scheduler(const std::string& name, ComputeIntent intent,
                         const std::string& type);

  /// @brief 获取指定图的调度器信息
  /// @param name 图名称
  /// @param intent 计算意图 (HP/RT)
  /// @return 调度器名称和统计信息，如果图不存在则返回 nullopt
  std::optional<std::pair<std::string, std::string>> get_scheduler_info(
      const std::string& name, ComputeIntent intent) const;

 private:
  void setup_schedulers_for_runtime(GraphRuntime& runtime);

  std::map<std::string, std::unique_ptr<GraphRuntime>> graphs_;
  PluginManager plugin_mgr_;
  std::map<std::string, LastError> last_error_;
  GraphTraversalService traversal_service_;
  GraphInspectService inspect_service_;
  GraphCacheService cache_service_;
  GraphIOService io_service_;
  RoiPropagationService roi_propagation_service_;

  // [M3.4] 调度器配置
  SchedulerConfig scheduler_config_;
};

}  // namespace ps
