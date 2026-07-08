// Photospider kernel: multi-graph Kernel facade
#pragma once

#include <future>
#include <map>
#include <memory>
#include <opencv2/opencv.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "kernel/graph_runtime.hpp"
#include "kernel/plugin_manager.hpp"
#include "kernel/services/compute-service/dirty_control_lane.hpp"
#include "kernel/services/compute_service.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_inspect_service.hpp"
#include "kernel/services/graph_io_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "kernel/services/roi_propagation_service.hpp"

namespace ps {

/**
 * @brief Multi-graph facade for graph lifecycle, graph-state commands, compute,
 * scheduler access, and plugin management.
 *
 * Kernel owns one GraphRuntime per graph name and keeps frontend-facing APIs
 * stable while delegating graph IO, traversal, inspection, cache, ROI, dirty
 * control, and compute work to narrower services. Graph-state operations run
 * through each runtime's GraphStateExecutor so topology and runtime metadata
 * remain serialized with compute, including scheduler-backed parallel compute.
 * Schedulers receive ready task callbacks; they do not own graph-state
 * operation dispatch.
 *
 * @note Public methods return bool or std::optional for frontend-friendly
 * failure handling. Compute and ROI/dirty APIs additionally store per-graph
 * LastError details when their service boundary reports GraphError or
 * std::exception.
 */
class Kernel {
 public:
  /**
   * @brief Last frontend-visible error recorded for one graph.
   *
   * @note The value is best-effort diagnostic state. Missing graphs and helper
   * APIs that historically returned only false/nullopt do not always update it.
   */
  struct LastError {
    GraphErrc code = GraphErrc::Unknown;
    std::string message;
  };

  /**
   * @brief Cache and persistence controls for one Kernel compute request.
   *
   * CacheOptions groups precision, recache, disk-cache, and save behavior so
   * callers cannot accidentally swap unrelated boolean flags. These settings
   * are applied for one compute request and then the graph skip-save flag is
   * restored according to the historical compute facade rules.
   *
   * @note disable_disk_cache affects reads from disk cache. nosave controls
   * cache save side effects through GraphModel::set_skip_save_cache().
   */
  struct ComputeCacheOptions {
    /** @brief Precision label passed to cache read/write paths. */
    std::string precision;

    /** @brief Whether selected in-memory caches should be recomputed. */
    bool force_recache = false;

    /** @brief Whether disk-cache reads are disabled for this request. */
    bool disable_disk_cache = false;

    /** @brief Whether this request should skip saving cache outputs. */
    bool nosave = false;
  };

  /**
   * @brief Execution-mode controls for one Kernel compute request.
   *
   * ExecutionOptions separates scheduler and frontend quiet-mode choices from
   * cache and telemetry options. It is copied into async futures, so callers
   * should treat it as immutable once the request is submitted.
   *
   * @note parallel selects scheduler-backed compute where supported. quiet is
   * applied to the graph model around the compute call.
   */
  struct ComputeExecutionOptions {
    /** @brief Whether the request should use scheduler-backed execution. */
    bool parallel = false;

    /** @brief Whether graph output should be quiet during this request. */
    bool quiet = false;
  };

  /**
   * @brief Timing and benchmark controls for one Kernel compute request.
   *
   * TelemetryOptions owns no storage. benchmark_events is a borrowed optional
   * sink and must outlive any asynchronous future that receives the request.
   *
   * @note enable_timing resets and fills graph timing state for the target
   * graph when the service path supports timing collection.
   */
  struct ComputeTelemetryOptions {
    /** @brief Whether graph timing data should be collected. */
    bool enable_timing = false;

    /** @brief Optional caller-owned per-node benchmark event sink. */
    std::vector<BenchmarkEvent>* benchmark_events = nullptr;
  };

  /**
   * @brief Public compute request shared by sync, async, and image facades.
   *
   * ComputeRequest is the public Kernel compute contract. It preserves CLI and
   * frontend behavior while preventing long positional parameter lists from
   * spreading into Kernel and ComputeService. A missing intent selects the
   * legacy high-precision path; a populated intent enables HP/RT coordination
   * and forwards dirty_roi to ComputeService.
   *
   * @note name identifies the graph runtime. benchmark_events is caller-owned
   * and must remain valid for the full synchronous call or async future.
   */
  struct ComputeRequest {
    /** @brief Graph name resolved by Kernel before dispatch. */
    std::string name;

    /** @brief Target graph node id to compute. */
    int node_id = 0;

    /** @brief Cache precision, recache, disk-cache, and save controls. */
    ComputeCacheOptions cache;

    /** @brief Scheduler and quiet-mode execution controls. */
    ComputeExecutionOptions execution;

    /** @brief Timing and benchmark recording controls. */
    ComputeTelemetryOptions telemetry;

    /** @brief Optional compute intent; nullopt selects legacy HP behavior. */
    std::optional<ComputeIntent> intent;

    /** @brief Optional HP-space dirty ROI for dirty HP or RT updates. */
    std::optional<cv::Rect> dirty_roi;
  };

  std::optional<std::string> load_graph(const std::string& name,
                                        const std::string& root_dir,
                                        const std::string& yaml_path,
                                        const std::string& config_path = "",
                                        const std::string& cache_root_dir = "");

  bool close_graph(const std::string& name);
  std::vector<std::string> list_graphs() const;

  template <typename Fn>
  auto post(const std::string& name, Fn&& fn)
      -> std::future<decltype(fn(std::declval<GraphModel&>()))> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      throw std::runtime_error("Graph not found: " + name);
    }
    return it->second->graph_state().submit(std::forward<Fn>(fn));
  }

  /**
   * @brief Computes a graph node synchronously from a structured request.
   *
   * @param request Graph name, target node, cache, execution, telemetry, and
   * optional intent/dirty ROI controls.
   * @return true when compute completes; false when the graph is missing or
   * the compute boundary reports a handled failure.
   * @throws Nothing; handled failures are recorded in last_error().
   * @note The request object is not retained after the call. benchmark_events
   * remains caller-owned for the duration of the call.
   */
  bool compute(const ComputeRequest& request);

  /**
   * @brief Schedules a graph node compute from a structured request.
   *
   * @param request Graph name, target node, cache, execution, telemetry, and
   * optional intent/dirty ROI controls captured by value.
   * @return Future resolving to success, or nullopt when the graph is missing.
   * @throws std::system_error if std::async cannot launch the parallel branch.
   * @note The future owns the request copy, but benchmark_events remains
   * caller-owned and must outlive future completion.
   */
  std::optional<std::future<bool>> compute_async(ComputeRequest request);
  std::optional<TimingCollector> get_timing(const std::string& name);
  std::optional<cv::Rect> project_roi_forward(const std::string& name,
                                              int start_node_id,
                                              const cv::Rect& start_roi,
                                              int target_node_id);
  std::optional<cv::Rect> project_roi_backward(const std::string& name,
                                               int target_node_id,
                                               const cv::Rect& target_roi,
                                               int source_node_id);

  /**
   * @brief Reloads an existing graph session from YAML through graph-state
   * serialization.
   *
   * @param name Graph session name to reload.
   * @param yaml_path Source YAML file path.
   * @return true when reload succeeds; false when the graph is missing or the
   * reload fails with a handled IO/YAML error.
   * @throws Nothing; GraphError/std::exception from the backend load path are
   * converted to last_error() and false.
   * @note Missing graph sessions preserve the legacy quiet false result without
   * updating LastError. For existing sessions, GraphIOService error categories
   * such as GraphErrc::Io and GraphErrc::InvalidYaml are retained in
   * last_error().
   */
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
  /**
   * @brief Cache and display metadata for one node in a traversal tree.
   *
   * @note Disk-cache presence is derived from cache file metadata at inspection
   * time and is not a durable graph-state field.
   */
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
  std::optional<compute::DirtyRegionSnapshot> begin_dirty_source(
      const std::string& name, int node_id, compute::DirtyDomain domain,
      const cv::Rect& source_roi);
  std::optional<compute::DirtyControlLaneResult> begin_dirty_source_control(
      const std::string& name, int node_id, compute::DirtyDomain domain,
      const cv::Rect& source_roi);
  std::optional<compute::DirtyRegionSnapshot> update_dirty_source(
      const std::string& name, int node_id, compute::DirtyDomain domain,
      const cv::Rect& source_roi);
  std::optional<compute::DirtyControlLaneResult> update_dirty_source_control(
      const std::string& name, int node_id, compute::DirtyDomain domain,
      const cv::Rect& source_roi);
  std::optional<compute::DirtyRegionSnapshot> end_dirty_source(
      const std::string& name, int node_id, compute::DirtyDomain domain);
  std::optional<compute::DirtyControlLaneResult> end_dirty_source_control(
      const std::string& name, int node_id, compute::DirtyDomain domain);
  /**
   * @brief Computes a node and returns its output image from a request object.
   *
   * @param request Graph name, target node, cache, execution, telemetry, and
   * optional intent/dirty ROI controls.
   * @return Cloned output image, or nullopt when graph lookup, compute, or
   * image conversion fails.
   * @throws Nothing; this facade preserves the historical quiet failure
   * contract for preview/save helpers.
   * @note The image is cloned out of graph-owned storage before returning.
   */
  std::optional<cv::Mat> compute_and_get_image(const ComputeRequest& request);

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
  /**
   * @brief Checks whether a node has a materialized disk-cache payload or
   * metadata file.
   *
   * @param graph Graph that owns the node and cache-root configuration.
   * @param node Node whose configured cache entries should be inspected.
   * @return true when any cache payload or matching .yml metadata file exists.
   * @throws std::filesystem::filesystem_error if the platform cannot inspect a
   * configured cache path.
   * @note This helper performs point-in-time filesystem inspection only; it
   * does not mutate graph cache state or trigger cache synchronization.
   */
  bool has_node_disk_cache(const GraphModel& graph, const Node& node) const;

  /**
   * @brief Builds frontend traversal metadata for one node id.
   *
   * @param graph Graph containing the requested node.
   * @param node_id Node identifier to inspect.
   * @return TraversalNodeInfo with node identity plus memory and disk cache
   * visibility.
   * @throws GraphError/std::exception when node lookup or filesystem inspection
   * fails; traversal_details_for_end converts those failures to a skipped
   * traversal branch to preserve the historical facade behavior.
   * @note Memory-cache visibility reflects either high-precision or real-time
   * cached output; disk-cache visibility is derived from configured cache
   * paths.
   */
  TraversalNodeInfo build_traversal_node_info(const GraphModel& graph,
                                              int node_id) const;

  /**
   * @brief Builds traversal metadata for one ending node.
   *
   * @param graph Graph whose traversal order should be inspected.
   * @param end_node_id Ending node used as the traversal root.
   * @return Node metadata in topological postorder, or nullopt when this
   * traversal branch cannot be inspected.
   * @throws Nothing; all branch-local failures are swallowed to preserve the
   * previous traversal_details contract.
   * @note The caller still receives details for other ending nodes when one
   * branch fails.
   */
  std::optional<std::vector<TraversalNodeInfo>> traversal_details_for_end(
      GraphModel& graph, int end_node_id) const;

  /**
   * @brief Executes a graph-runtime facade operation with missing-graph and
   * exception-to-nullopt handling.
   *
   * @param name Graph name to resolve.
   * @param op Callable invoked as op(GraphRuntime&).
   * @return Optional operation result, or nullopt when the graph is missing or
   * the operation throws.
   * @throws Nothing; all exceptions from op are converted to nullopt to match
   * existing thin facade APIs.
   * @note This helper is for historically quiet runtime accessors. APIs that
   * must update LastError keep their explicit GraphError/std::exception
   * handling.
   */
  template <typename Fn>
  auto with_runtime(const std::string& name, Fn&& op)
      -> std::optional<std::decay_t<std::invoke_result_t<Fn, GraphRuntime&>>> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      return std::nullopt;
    }
    try {
      return std::forward<Fn>(op)(*it->second);
    } catch (...) {
      return std::nullopt;
    }
  }

  /**
   * @brief Const overload for runtime facade accessors.
   *
   * @param name Graph name to resolve.
   * @param op Callable invoked as op(const GraphRuntime&).
   * @return Optional operation result, or nullopt when the graph is missing or
   * the operation throws.
   * @throws Nothing; all exceptions from op are converted to nullopt.
   * @note Used by const inspection APIs such as scheduler metadata queries.
   */
  template <typename Fn>
  auto with_runtime(const std::string& name, Fn&& op) const -> std::optional<
      std::decay_t<std::invoke_result_t<Fn, const GraphRuntime&>>> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      return std::nullopt;
    }
    try {
      return std::forward<Fn>(op)(*it->second);
    } catch (...) {
      return std::nullopt;
    }
  }

  /**
   * @brief Executes one serialized GraphModel operation through the graph-state
   * executor.
   *
   * @param name Graph name to resolve.
   * @param op Callable submitted as op(GraphModel&).
   * @return Optional result from op, or nullopt when the graph is missing,
   * submit/get fails, or op throws.
   * @throws Nothing; submit, future get, and op exceptions are converted to
   * nullopt.
   * @note This helper preserves the facade contract for graph-state commands:
   * they remain serialized by GraphStateExecutor and are not routed through
   * scheduler task runtimes.
   */
  template <typename Fn>
  auto with_graph_state(const std::string& name, Fn&& op)
      -> std::optional<std::decay_t<std::invoke_result_t<Fn, GraphModel&>>> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      return std::nullopt;
    }
    try {
      return it->second->graph_state().submit(std::forward<Fn>(op)).get();
    } catch (...) {
      return std::nullopt;
    }
  }

  /**
   * @brief Executes one serialized graph-state operation and records LastError
   * details on handled failures.
   *
   * @param name Graph name to resolve.
   * @param exception_prefix Prefix used for std::exception diagnostic messages.
   * @param op Callable submitted as op(GraphModel&).
   * @param start_runtime Whether to start the owning runtime before submit.
   * @return Optional result from op, or nullopt when the graph is missing or
   * the operation reports a handled failure.
   * @throws Nothing; GraphError and std::exception are converted to LastError
   * and nullopt.
   * @note Use this helper for facade APIs whose existing contract exposes
   * best-effort LastError details. Historically quiet accessors should keep
   * using with_graph_state so they continue to hide diagnostic state.
   */
  template <typename Fn>
  auto with_graph_state_last_error(const std::string& name,
                                   const char* exception_prefix, Fn&& op,
                                   bool start_runtime = false)
      -> std::optional<std::decay_t<std::invoke_result_t<Fn, GraphModel&>>> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      return std::nullopt;
    }
    try {
      if (start_runtime && !it->second->running()) {
        it->second->start();
      }
      auto result =
          it->second->graph_state().submit(std::forward<Fn>(op)).get();
      last_error_.erase(name);
      return result;
    } catch (const GraphError& ge) {
      last_error_[name] = {ge.code(), ge.what()};
      return std::nullopt;
    } catch (const std::exception& e) {
      last_error_[name] = {GraphErrc::Unknown,
                           std::string(exception_prefix) + e.what()};
      return std::nullopt;
    }
  }

  /**
   * @brief Runs the public synchronous compute facade from a request object.
   *
   * @param request Public compute request with graph, cache, execution, and
   * telemetry options.
   * @return true when ComputeService completes successfully; false when the
   * graph is missing or compute reports a handled failure.
   * @throws Nothing; GraphError/std::exception/unknown exceptions are mapped to
   * the existing LastError behavior and false.
   * @note quiet and skip-save are applied only for the active request and are
   * restored on every success or failure exit path.
   */
  bool compute_request(const ComputeRequest& request);

  /**
   * @brief Schedules the async compute facade from a request object.
   *
   * @param request Public async compute request captured by value.
   * @return Future that resolves to the compute status, or nullopt when the
   * graph is missing.
   * @throws std::system_error if GraphStateExecutor cannot launch the async
   * graph-state work.
   * @note The returned future owns the request copy. benchmark_events remains
   * caller-owned and must outlive future completion.
   */
  std::optional<std::future<bool>> compute_async_request(
      ComputeRequest request);

  /**
   * @brief Runs compute and returns the target output as an OpenCV image.
   *
   * @param request Compute request with image-returning facade arguments.
   * @return Cloned cv::Mat target image, or nullopt on missing graph, compute
   * failure, or empty output.
   * @throws Nothing; failures are converted to nullopt to preserve the previous
   * save/preview facade contract.
   * @note This path intentionally does not modify LastError, matching the prior
   * compute_and_get_image behavior.
   */
  std::optional<cv::Mat> compute_and_get_image_request(
      const ComputeRequest& request);

  /**
   * @brief Dispatches one request through the correct ComputeService path.
   *
   * @param compute_service Request-scoped ComputeService collaborator.
   * @param runtime Runtime that owns scheduler/event services for the graph.
   * @param graph Visible graph model to compute against.
   * @param request Kernel compute request to translate into service options.
   * @return Mutable output owned by the graph node cache.
   * @throws GraphError or std::exception from ComputeService.
   * @note intent=nullopt selects legacy HP-only overloads; otherwise intent and
   * dirty_roi are forwarded to the intent-aware HP/RT path. Callers should
   * invoke this only from a GraphStateExecutor work item when the graph is a
   * visible runtime model.
   */
  NodeOutput& run_compute_request(ComputeService& compute_service,
                                  GraphRuntime& runtime, GraphModel& graph,
                                  const ComputeRequest& request);

  void setup_schedulers_for_runtime(const std::string& name,
                                    GraphRuntime& runtime);

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
