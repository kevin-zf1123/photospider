#pragma once

/**
 * @file interaction.hpp
 * @brief Legacy source-tree interaction facade between CLI code and Kernel.
 *
 * This header lives under the private `src/` include root and is not part of
 * the installable `include/photospider/` public interface. It includes
 * `kernel/kernel.hpp`, which depends on private implementation headers;
 * repository targets that still use InteractionService must receive the private
 * include roots. External frontends should use `photospider/host/host.hpp`
 * rather than linking `photospider_lib` and including this source-tree facade.
 */

#include <functional>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "kernel/kernel.hpp"
#include "kernel/plugin_result.hpp"

namespace ps {

/**
 * @brief Command-oriented facade that keeps frontends on Kernel value APIs.
 *
 * InteractionService owns no graph state. It translates CLI/host-style command
 * calls to Kernel facades and deliberately does not expose the underlying
 * Kernel reference, runtime map, or graph-state executor. Tests that must
 * inspect internals use the internal-only helper under tests/support; frontend
 * code should use the cmd_* value accessors here.
 *
 * @note The referenced Kernel must outlive the InteractionService instance.
 */
class InteractionService {
 public:
  /**
   * @brief Binds the interaction facade to an existing Kernel.
   *
   * @param kernel Kernel instance that owns graph runtimes and services.
   * @throws Nothing.
   * @note The service stores a borrowed reference and performs no ownership or
   * lifetime management.
   */
  explicit InteractionService(Kernel& kernel) : kernel_(kernel) {}

  // Graph lifecycle
  std::optional<std::string> cmd_load_graph(
      const std::string& name, const std::string& root_dir,
      const std::string& yaml_path, const std::string& config_path = "",
      const std::string& cache_root_dir = "") {
    return kernel_.load_graph(name, root_dir, yaml_path, config_path,
                              cache_root_dir);
  }
  bool cmd_close_graph(const std::string& name) {
    return kernel_.close_graph(name);
  }
  std::vector<std::string> cmd_list_graphs() const {
    return kernel_.list_graphs();
  }

  // [API修复] 新增方法以从 CLI 安全地获取末端节点
  std::optional<std::vector<int>> cmd_ending_nodes(const std::string& graph) {
    return kernel_.ending_nodes(graph);
  }

  /**
   * @brief Runs a synchronous compute command from a Kernel request object.
   *
   * @param request Graph, target node, cache, execution, telemetry, and
   * optional dirty/intent controls supplied by the frontend.
   * @return true when Kernel compute succeeds; false on missing graph or
   * handled compute failure.
   * @throws Nothing directly; Kernel records handled errors in last_error().
   * @note The request is forwarded without retaining borrowed benchmark sinks.
   */
  bool cmd_compute(const Kernel::ComputeRequest& request) {
    return kernel_.compute(request);
  }
  std::optional<TimingCollector> cmd_timing(const std::string& graph) {
    return kernel_.get_timing(graph);
  }

  // Plugins
  void cmd_plugins_load(const std::vector<std::string>& dirs) {
    kernel_.plugins().load_from_dirs(dirs);
  }
  PluginLoadResult cmd_plugins_load_report(
      const std::vector<std::string>& dirs) {
    return kernel_.plugins().load_from_dirs_report(dirs);
  }
  int cmd_plugins_unload_all() {
    return kernel_.plugins().unload_all_plugins();
  }
  void cmd_seed_builtin_ops() {
    kernel_.plugins().seed_builtins_from_registry();
  }

  // Ops overview (type:subtype -> source path or "built-in")
  std::map<std::string, std::string> cmd_ops_sources() const {
    return kernel_.plugins().op_sources();
  }
  // Combined ops: collapse monolithic/tiled HP/RT under a single op key;
  // frontends should use this
  std::vector<std::string> cmd_ops_combined_keys() const {
    return ps::OpRegistry::instance().get_combined_keys();
  }
  std::map<std::string, std::string> cmd_ops_combined_sources() const {
    std::map<std::string, std::string> out;
    auto keys = ps::OpRegistry::instance().get_combined_keys();
    const auto& sources = kernel_.plugins().op_sources();
    for (const auto& k : keys) {
      auto it = sources.find(k);
      if (it != sources.end()) {
        out[k] = it->second;
        continue;
      }
      // Fallback: if an alias "*_tiled" exists in sources, use its source
      auto pos = k.rfind(':');
      if (pos != std::string::npos) {
        std::string base = k.substr(0, pos + 1);
        std::string tiled = base + (k.substr(pos + 1) + std::string("_tiled"));
        auto it2 = sources.find(tiled);
        if (it2 != sources.end()) {
          out[k] = it2->second;
          continue;
        }
      }
      out[k] = "built-in";  // default when unknown
    }
    return out;
  }

  // IO / cache / traversal / printing
  bool cmd_reload_yaml(const std::string& graph, const std::string& yaml_path) {
    return kernel_.reload_graph_yaml(graph, yaml_path);
  }
  bool cmd_save_yaml(const std::string& graph, const std::string& yaml_path) {
    return kernel_.save_graph_yaml(graph, yaml_path);
  }
  bool cmd_clear_drive_cache(const std::string& graph) {
    return kernel_.clear_drive_cache(graph);
  }
  bool cmd_clear_memory_cache(const std::string& graph) {
    return kernel_.clear_memory_cache(graph);
  }
  bool cmd_clear_cache(const std::string& graph) {
    return kernel_.clear_cache(graph);
  }
  bool cmd_cache_all_nodes(const std::string& graph,
                           const std::string& precision) {
    return kernel_.cache_all_nodes(graph, precision);
  }
  // Structured stats APIs
  std::optional<GraphModel::DriveClearResult> cmd_clear_drive_cache_stats(
      const std::string& graph) {
    return kernel_.clear_drive_cache_stats(graph);
  }
  std::optional<GraphModel::MemoryClearResult> cmd_clear_memory_cache_stats(
      const std::string& graph) {
    return kernel_.clear_memory_cache_stats(graph);
  }
  std::optional<GraphModel::CacheSaveResult> cmd_cache_all_nodes_stats(
      const std::string& graph, const std::string& precision) {
    return kernel_.cache_all_nodes_stats(graph, precision);
  }
  std::optional<GraphModel::MemoryClearResult> cmd_free_transient_memory_stats(
      const std::string& graph) {
    return kernel_.free_transient_memory_stats(graph);
  }
  std::optional<GraphModel::DiskSyncResult> cmd_synchronize_disk_cache_stats(
      const std::string& graph, const std::string& precision) {
    return kernel_.synchronize_disk_cache_stats(graph, precision);
  }
  // Structured stats wrappers can be added when needed.
  bool cmd_free_transient_memory(const std::string& graph) {
    return kernel_.free_transient_memory(graph);
  }
  bool cmd_synchronize_disk_cache(const std::string& graph,
                                  const std::string& precision) {
    return kernel_.synchronize_disk_cache(graph, precision);
  }
  bool cmd_clear_graph(const std::string& graph) {
    return kernel_.clear_graph(graph);
  }

  std::optional<DependencyTree> cmd_dependency_tree(
      const std::string& graph, std::optional<int> node_id,
      bool include_metadata = false) {
    return kernel_.dependency_tree(graph, node_id, include_metadata);
  }
  std::optional<GraphNodeInspectInfo> cmd_inspect_node(const std::string& graph,
                                                       int node_id) {
    return kernel_.inspect_node(graph, node_id);
  }
  std::optional<GraphInspectionSnapshot> cmd_inspect_graph(
      const std::string& graph) {
    return kernel_.inspect_graph(graph);
  }
  std::optional<Kernel::LastError> cmd_last_error(
      const std::string& graph) const {
    return kernel_.last_error(graph);
  }
  std::optional<std::map<int, std::vector<int>>> cmd_traversal_orders(
      const std::string& graph) {
    return kernel_.traversal_orders(graph);
  }
  std::optional<std::map<int, std::vector<Kernel::TraversalNodeInfo>>>
  cmd_traversal_details(const std::string& graph) {
    return kernel_.traversal_details(graph);
  }
  std::optional<std::vector<GraphEventService::ComputeEvent>>
  cmd_drain_compute_events(const std::string& graph) {
    return kernel_.drain_compute_events(graph);
  }
  std::optional<std::vector<GraphRuntime::SchedulerEvent>> cmd_scheduler_trace(
      const std::string& graph) {
    return kernel_.scheduler_trace(graph);
  }
  std::optional<std::string> cmd_dirty_region_snapshot_debug(
      const std::string& graph) {
    return kernel_.dirty_region_snapshot_debug(graph);
  }
  std::optional<compute::DirtyRegionSnapshot> cmd_dirty_region_snapshot(
      const std::string& graph) {
    return kernel_.dirty_region_snapshot(graph);
  }

  /**
   * @brief Reads the latest backend planning summary through Kernel.
   *
   * @param graph Graph/session name.
   * @return Latest summary, empty optional if unavailable.
   * @throws std::bad_alloc if summary copies allocate.
   * @note Public Host adapters convert this internal summary into stable value
   *       snapshots before exposing it to frontends.
   */
  std::optional<compute::ComputePlanSummary> cmd_compute_planning_snapshot(
      const std::string& graph) {
    return kernel_.compute_planning_snapshot(graph);
  }

  /**
   * @brief Reads bounded backend planning summary history through Kernel.
   *
   * @param graph Graph/session name.
   * @return Summary history, or nullopt when graph lookup fails.
   * @throws std::bad_alloc if vector or summary copies allocate.
   * @note Empty history is valid for loaded graphs before compute.
   */
  std::optional<std::vector<compute::ComputePlanSummary>>
  cmd_recent_compute_planning_snapshots(const std::string& graph) {
    return kernel_.recent_compute_planning_snapshots(graph);
  }

  std::optional<compute::DirtyRegionSnapshot> cmd_begin_dirty_source(
      const std::string& graph, int node_id, compute::DirtyDomain domain,
      const cv::Rect& source_roi) {
    return kernel_.begin_dirty_source(graph, node_id, domain, source_roi);
  }
  std::optional<compute::DirtyControlLaneResult> cmd_begin_dirty_source_control(
      const std::string& graph, int node_id, compute::DirtyDomain domain,
      const cv::Rect& source_roi) {
    return kernel_.begin_dirty_source_control(graph, node_id, domain,
                                              source_roi);
  }
  std::optional<compute::DirtyRegionSnapshot> cmd_update_dirty_source(
      const std::string& graph, int node_id, compute::DirtyDomain domain,
      const cv::Rect& source_roi) {
    return kernel_.update_dirty_source(graph, node_id, domain, source_roi);
  }
  std::optional<compute::DirtyControlLaneResult>
  cmd_update_dirty_source_control(const std::string& graph, int node_id,
                                  compute::DirtyDomain domain,
                                  const cv::Rect& source_roi) {
    return kernel_.update_dirty_source_control(graph, node_id, domain,
                                               source_roi);
  }
  std::optional<compute::DirtyRegionSnapshot> cmd_end_dirty_source(
      const std::string& graph, int node_id, compute::DirtyDomain domain) {
    return kernel_.end_dirty_source(graph, node_id, domain);
  }
  std::optional<compute::DirtyControlLaneResult> cmd_end_dirty_source_control(
      const std::string& graph, int node_id, compute::DirtyDomain domain) {
    return kernel_.end_dirty_source_control(graph, node_id, domain);
  }
  /**
   * @brief Computes a node and returns an image from a Kernel request object.
   *
   * @param request Graph, target node, cache, execution, telemetry, and
   * optional dirty/intent controls supplied by the frontend.
   * @return Cloned image, or nullopt when compute or image extraction fails.
   * @throws Nothing directly; Kernel keeps the preview/save nullopt contract.
   * @note The request is not retained after image extraction completes.
   */
  std::optional<cv::Mat> cmd_compute_and_get_image(
      const Kernel::ComputeRequest& request) {
    return kernel_.compute_and_get_image(request);
  }
  std::optional<std::vector<int>> cmd_trees_containing_node(
      const std::string& graph, int node_id) {
    return kernel_.trees_containing_node(graph, node_id);
  }

  // Nodes
  std::optional<std::vector<int>> cmd_list_node_ids(const std::string& graph) {
    return kernel_.list_node_ids(graph);
  }
  std::optional<std::string> cmd_get_node_yaml(const std::string& graph,
                                               int node_id) {
    return kernel_.get_node_yaml(graph, node_id);
  }
  bool cmd_set_node_yaml(const std::string& graph, int node_id,
                         const std::string& yaml_text) {
    return kernel_.set_node_yaml(graph, node_id, yaml_text);
  }
  /**
   * @brief Schedules an asynchronous compute command from a request object.
   *
   * @param request Graph, target node, cache, execution, telemetry, and
   * optional dirty/intent controls captured by value.
   * @return Future resolving to success, or nullopt when the graph is missing.
   * @throws std::system_error if Kernel cannot launch the async parallel path.
   * @note benchmark_events is still caller-owned and must outlive the future.
   */
  std::optional<std::future<bool>> cmd_compute_async(
      Kernel::ComputeRequest request) {
    return kernel_.compute_async(std::move(request));
  }
  std::optional<cv::Rect> cmd_project_roi(const std::string& graph,
                                          int start_node_id,
                                          const cv::Rect& start_roi,
                                          int target_node_id) {
    return kernel_.project_roi_forward(graph, start_node_id, start_roi,
                                       target_node_id);
  }
  std::optional<cv::Rect> cmd_project_roi_backward(const std::string& graph,
                                                   int target_node_id,
                                                   const cv::Rect& target_roi,
                                                   int source_node_id) {
    return kernel_.project_roi_backward(graph, target_node_id, target_roi,
                                        source_node_id);
  }
  std::optional<double> cmd_get_last_io_time(const std::string& graph) {
    return kernel_.get_last_io_time(graph);
  }

  // Benchmark orchestration helpers
  BenchmarkResult cmd_run_benchmark(const std::string& benchmark_dir,
                                    const BenchmarkSessionConfig& session,
                                    int runs = 10);
  std::vector<BenchmarkResult> cmd_run_all_benchmarks(
      const std::string& benchmark_dir);
  std::vector<BenchmarkSessionConfig> cmd_load_benchmark_configs(
      const std::string& benchmark_dir);
  void cmd_cleanup_benchmark_artifacts(const std::string& benchmark_dir);

  // [新增] GPU 上下文访问器
  id cmd_get_metal_device(const std::string& graph) {
    return kernel_.get_metal_device(graph);
  }

  // [M3.5] Scheduler information
  // Get all available scheduler types (built-in + plugins)
  std::vector<std::string> cmd_scheduler_available_types() const;

  // Get description for a scheduler type
  std::string cmd_scheduler_description(const std::string& type_name) const;

  // Scan and load scheduler plugins from directories
  size_t cmd_scheduler_scan(const std::vector<std::string>& dirs);

  // Load a single scheduler plugin
  bool cmd_scheduler_load(const std::string& path);

  // Get list of loaded scheduler plugins
  std::vector<std::string> cmd_scheduler_loaded_plugins() const;

 private:
  Kernel& kernel_;
};

}  // namespace ps
