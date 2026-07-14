#pragma once

/**
 * @file kernel.hpp
 * @brief Legacy source-tree Kernel facade for repository internals.
 *
 * This header lives under the private `src/lib/` include root and is not part
 * of the installable `include/photospider/` public interface. Repository
 * targets that still need Kernel must receive
 * `PHOTOSPIDER_PRIVATE_INCLUDE_DIRS`. External frontends should use
 * `photospider/host/host.hpp` and copied Host value snapshots instead of
 * depending on Kernel, GraphRuntime, or compute-service ownership.
 */

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <opencv2/opencv.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "compute/compute_service.hpp"
#include "compute/dirty_control_lane.hpp"
#include "graph/graph_cache_service.hpp"
#include "graph/graph_inspect_service.hpp"
#include "graph/graph_io_service.hpp"
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"
#include "plugin/plugin_manager.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps {

namespace testing {

/**
 * @brief Test-only bridge that may inspect Kernel internals.
 *
 * KernelTestAccess is declared here only so Kernel can friend it. The
 * definition lives under tests/support and is not part of the Host API.
 * Frontends use `ps::Host`; the embedded Host adapter alone may translate Host
 * requests to Kernel operations such as inspect_graph, scheduler_trace,
 * compute, and compute_async. Neither path exposes runtime ownership details.
 *
 * @note This forward declaration intentionally exposes no operations.
 */
class KernelTestAccess;

}  // namespace testing

/**
 * @brief Internal multi-graph coordinator for lifecycle, graph-state commands,
 * compute, scheduler access, and plugin management.
 *
 * Kernel owns one GraphRuntime per graph name and keeps its internal adapter
 * contract stable while delegating graph IO, traversal, inspection, cache, ROI,
 * dirty control, and compute work to narrower services. The embedded Host
 * adapter maps public Host values to these internal operations; frontend code
 * neither includes nor constructs Kernel types. Graph-state operations run
 * through each runtime's GraphStateExecutor so topology and runtime metadata
 * remain serialized with compute, including scheduler-backed parallel compute.
 * Schedulers receive ready task callbacks; they do not own graph-state
 * operation dispatch.
 *
 * @note Historically quiet inspection/cache methods retain bool or
 * std::optional failure handling. Required Host mutation/projection paths keep
 * exact GraphError categories through `with_required_graph_state()` so absence
 * is not conflated with content or parameter failures. Public Host methods
 * translate those results into copied status/value types. Compute and
 * ROI/dirty APIs also store per-graph LastError details when their service
 * boundary reports GraphError or std::exception.
 */
class Kernel {
 public:
  /**
   * @brief Last backend error recorded for one graph session.
   *
   * @note The value is best-effort diagnostic state. Missing graphs and helper
   * APIs that historically returned only false/nullopt do not always update it.
   */
  struct LastError {
    /** @brief Exact graph-domain category captured at the failure boundary. */
    GraphErrc code = GraphErrc::Unknown;

    /** @brief Owned diagnostic text captured by the same failed operation. */
    std::string message;
  };

  /**
   * @brief Immutable outcome produced by one asynchronous compute work item.
   *
   * @throws Nothing for destruction; constructing the owned diagnostic may
   *         throw std::bad_alloc inside the asynchronous work item.
   * @note A failed value owns the exact error captured by that work item. It is
   *       never reconstructed from the mutable per-session LastError map.
   */
  struct AsyncComputeResult {
    /** @brief True only when the associated compute completed successfully. */
    bool ok = false;

    /** @brief Exact failure, absent for a successful compute. */
    std::optional<LastError> error;
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
   * ExecutionOptions separates scheduler and graph-output quiet-mode choices
   * from cache and telemetry options. It is copied into async futures, so the
   * embedded adapter and backend callers should treat it as immutable once the
   * request is submitted.
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
   * @brief Internal compute request shared by sync, async, and image helpers.
   *
   * ComputeRequest is the adapter-to-Kernel compute value. The embedded Host
   * adapter creates it from a public `HostComputeRequest`; CLI/TUI/frontend
   * code constructs only the Host request and never this type. The structure
   * prevents long positional parameter lists from spreading into Kernel and
   * ComputeService. A missing intent selects the legacy high-precision path; a
   * populated intent enables HP/RT coordination and forwards dirty_roi to
   * ComputeService.
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

  /**
   * @brief Creates and transactionally loads one internal graph runtime.
   *
   * @param name Unique graph/session name.
   * @param root_dir Root directory that owns the session folder.
   * @param yaml_path Optional source YAML copied into the session before load.
   * @param config_path Optional config file copied into the session.
   * @param cache_root_dir Optional external cache-root directory.
   * @return Loaded graph name, or nullopt for duplicate names and recoverable
   * graph-load failures.
   * @throws std::bad_alloc if path, runtime, scheduler, graph, or diagnostic
   *         allocation exhausts memory.
   * @throws std::exception for scheduler/runtime startup failures not
   *         classified as recoverable graph-load errors.
   * @note The return label is allocated before the runtime enters the owned
   *       graph map. After insertion, returning it uses only noexcept moves,
   *       so a propagated exception never leaves a newly published session.
   */
  std::optional<std::string> load_graph(const std::string& name,
                                        const std::string& root_dir,
                                        const std::string& yaml_path,
                                        const std::string& config_path = "",
                                        const std::string& cache_root_dir = "");

  /**
   * @brief Stops and removes a loaded graph runtime.
   *
   * @param name Graph session name to close.
   * @return true when a runtime existed and was removed; false when the session
   *         name is unknown.
   * @throws Any exception propagated while stopping the runtime; no exception
   *         is thrown when the graph name is unknown.
   * @note Runtime stop is submitted through the same GraphStateExecutor used by
   *       all serialized graph-state work, including compute, scheduler
   *       inspection/replacement, required save, node replacement, and ROI
   *       projection. Closing therefore waits prior serialized work. Embedded
   *       Host admission separately waits for complete admitted calls before
   *       invoking this method. The graph map entry and its mutex-protected
   *       LastError snapshot are erased only after stop succeeds; if stop
   *       throws, both remain owned by Kernel so callers can inspect or retry
   *       the session.
   */
  bool close_graph(const std::string& name);
  std::vector<std::string> list_graphs() const;

  /**
   * @brief Computes a graph node synchronously from a structured request.
   *
   * @param request Graph name, target node, cache, execution, telemetry, and
   * optional intent/dirty ROI controls.
   * @return true when compute completes; false when the graph is missing or
   * the compute boundary reports a handled failure.
   * @throws std::bad_alloc if compute execution or failure translation
   *         exhausts memory.
   * @note The request object is not retained after the call. benchmark_events
   * remains caller-owned for the duration of the call. GraphError, ordinary
   * std::exception, and unknown compute failures otherwise become false plus
   * best-effort last_error() state. Runtime start and compute both execute
   * inside the graph-state boundary shared with scheduler lifecycle methods.
   */
  bool compute(const ComputeRequest& request);

  /**
   * @brief Schedules a graph node compute from a structured request.
   *
   * @param request Graph name, target node, cache, execution, telemetry, and
   * optional intent/dirty ROI controls captured by value.
   * @return Future resolving to an owned exact outcome, or nullopt when the
   *         graph is missing.
   * @throws std::bad_alloc if request, task, queue, or future-state allocation
   *         fails while scheduling the graph-state work.
   * @throws std::system_error if runtime startup or graph-state asynchronous
   *         execution cannot launch.
   * @note The future owns the request copy, but benchmark_events remains
   * caller-owned and must outlive future completion. Calling get() on the
   * returned future may rethrow std::bad_alloc from compute execution or
   * failure-diagnostic allocation inside the async work item. Recoverable
   * failures are captured in AsyncComputeResult by that same work item. Runtime
   * start occurs inside the submitted graph-state closure rather than during
   * admission.
   */
  std::optional<std::future<AsyncComputeResult>> compute_async(
      ComputeRequest request);
  std::optional<TimingCollector> get_timing(const std::string& name);
  /**
   * @brief Projects one ROI forward between required graph nodes.
   *
   * @param name Required graph session name.
   * @param start_node_id Required source node id.
   * @param start_roi Source ROI in output coordinates.
   * @param target_node_id Required downstream target node id.
   * @return Projected ROI, or nullopt when existing endpoints produce no valid
   *         projection.
   * @throws GraphError with `GraphErrc::NotFound` when the graph or either
   *         endpoint is absent, or another exact propagation category raised
   *         by RoiPropagationService.
   * @throws std::bad_alloc if startup, submission, projection, or LastError
   *         recording exhausts memory.
   * @throws std::exception for other runtime, executor, or propagation
   *         failures.
   * @note Endpoint lookup and projection execute in one GraphStateExecutor work
   *       item. Existing-session diagnostics are mirrored to LastError, while
   *       Embedded Host retains a lifecycle admission against close.
   */
  std::optional<cv::Rect> project_roi_forward(const std::string& name,
                                              int start_node_id,
                                              const cv::Rect& start_roi,
                                              int target_node_id);

  /**
   * @brief Projects one ROI backward between required graph nodes.
   *
   * @param name Required graph session name.
   * @param target_node_id Required downstream target node id.
   * @param target_roi Target ROI in output coordinates.
   * @param source_node_id Required upstream source node id.
   * @return Projected source ROI, or nullopt when existing endpoints produce no
   *         valid projection.
   * @throws GraphError with `GraphErrc::NotFound` when the graph or either
   *         endpoint is absent, or another exact propagation category raised
   *         by RoiPropagationService.
   * @throws std::bad_alloc if startup, submission, projection, or LastError
   *         recording exhausts memory.
   * @throws std::exception for other runtime, executor, or propagation
   *         failures.
   * @note Endpoint lookup and projection execute in one GraphStateExecutor work
   *       item. Existing-session diagnostics are mirrored to LastError, while
   *       Embedded Host retains a lifecycle admission against close.
   */
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
   * @throws std::bad_alloc if reload execution or handled-failure LastError
   *         construction exhausts memory.
   * @note Missing graph sessions preserve the legacy quiet false result without
   * updating LastError. For existing sessions, GraphIOService error categories
   * such as GraphErrc::Io and GraphErrc::InvalidYaml are retained in
   * last_error(); other GraphError/std::exception failures become false.
   */
  bool reload_graph_yaml(const std::string& name, const std::string& yaml_path);
  /**
   * @brief Saves one required graph session through graph-state serialization.
   *
   * @param name Graph session name to save.
   * @param yaml_path Destination YAML file path.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::NotFound` when the session is absent,
   *         or `GraphErrc::Io` when destination access, node serialization, or
   *         YAML emission fails.
   * @throws std::bad_alloc if graph-state submission or serialization exhausts
   *         memory.
   * @throws std::exception for other graph-state submission or future failures.
   * @note The embedded Host holds a session admission across this call so
   *       concurrent close cannot invalidate the resolved runtime. The save
   *       itself executes in the session's GraphStateExecutor. Save-specific
   *       stream, serialization, and emitter exceptions are normalized to Io
   *       at this boundary.
   */
  void save_graph_yaml(const std::string& name, const std::string& yaml_path);
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
  /**
   * @brief Copies the latest best-effort diagnostic for one graph session.
   *
   * @param name Graph/session name whose diagnostic is observed.
   * @return Owned LastError snapshot, or nullopt when none is recorded.
   * @throws std::bad_alloc if copying the diagnostic text fails.
   * @throws std::system_error if the diagnostic mutex cannot be locked.
   * @note All map access is serialized by last_error_mutex_. Async compute
   * futures never derive their result from this mutable snapshot.
   */
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
  /**
   * @brief Drains one bounded page of compute events for a loaded graph.
   * @param name Loaded graph/session name.
   * @param limit Maximum events to remove.
   * @return Sequenced batch, or nullopt when the graph is missing.
   * @throws std::invalid_argument for an invalid limit without mutation.
   * @throws std::bad_alloc if bounded result allocation fails without
   *         mutation.
   * @note Removal and shared drop reset occur inside the graph event-service
   *       lock. Runtime exceptions propagate unchanged; only a missing graph
   *       is represented by `std::nullopt`.
   */
  std::optional<ComputeEventBatch> drain_compute_events(const std::string& name,
                                                        std::size_t limit);

  /**
   * @brief Reads one bounded non-destructive scheduler-trace page.
   * @param name Loaded graph/session name.
   * @param after_sequence Exclusive sequence cursor.
   * @param limit Maximum trace entries to copy.
   * @return Internal sequenced page, or nullopt when the graph is missing.
   * @throws std::invalid_argument for an invalid limit or future cursor.
   * @throws std::bad_alloc if bounded page allocation fails.
   * @note The runtime computes entries and metadata under one trace lock.
   *       Runtime exceptions propagate unchanged; only a missing graph is
   *       represented by `std::nullopt`.
   */
  std::optional<GraphRuntime::SchedulerEventPage> scheduler_trace(
      const std::string& name, uint64_t after_sequence, std::size_t limit);
  std::optional<std::string> dirty_region_snapshot_debug(
      const std::string& name);
  std::optional<compute::DirtyRegionSnapshot> dirty_region_snapshot(
      const std::string& name);

  /**
   * @brief Returns the latest compute planning summary for a graph.
   *
   * @param name Loaded graph/session name.
   * @return Latest backend planning summary, empty optional when the graph is
   *         missing or no planning summary has been recorded.
   * @throws std::bad_alloc if summary value copies allocate.
   * @note This internal facade preserves backend summary types; public Host
   *       APIs convert the result to value snapshots without exposing planner
   *       structures.
   */
  std::optional<compute::ComputePlanSummary> compute_planning_snapshot(
      const std::string& name);

  /**
   * @brief Returns bounded recent compute planning summaries for a graph.
   *
   * @param name Loaded graph/session name.
   * @return Summary history for the graph, or nullopt when the graph is
   *         missing.
   * @throws std::bad_alloc if vector or summary copies allocate.
   * @note The history is copied while graph-state serialization is held, then
   *       returned by value to callers.
   */
  std::optional<std::vector<compute::ComputePlanSummary>>
  recent_compute_planning_snapshots(const std::string& name);

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
   * @throws std::bad_alloc if compute/image execution or handled-failure
   *         LastError construction exhausts memory.
   * @note The image is cloned out of graph-owned storage before returning.
   *       Other compute and image-conversion exceptions preserve the historical
   *       nullopt preview/save contract.
   */
  std::optional<cv::Mat> compute_and_get_image(const ComputeRequest& request);

  std::optional<std::vector<int>> list_node_ids(const std::string& name);
  std::optional<std::string> get_node_yaml(const std::string& name,
                                           int node_id);
  /**
   * @brief Replaces one required node from YAML under graph-state
   *        serialization.
   *
   * @param name Required graph session name.
   * @param node_id Required existing node id whose identity is preserved.
   * @param yaml_text Candidate replacement YAML mapping.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::NotFound` when the session or node is
   *         absent, or `GraphErrc::InvalidYaml` when parsing or complete
   *         candidate-topology validation fails.
   * @throws std::bad_alloc if parsing, validation, graph-state submission, or
   *         replacement exhausts memory.
   * @throws std::exception for other graph-state executor failures.
   * @note Required-node lookup, parsing, validation, and replacement execute in
   *       one GraphStateExecutor work item. Embedded Host retains a session
   *       admission across the call so concurrent close cannot erase the
   *       runtime.
   */
  void set_node_yaml(const std::string& name, int node_id,
                     const std::string& yaml_text);

  std::optional<std::vector<int>> trees_containing_node(const std::string& name,
                                                        int node_id);

  /**
   * @brief Returns the process-global operation plugin owner.
   *
   * @return Shared manager used by every Kernel and embedded Host instance.
   * @throws std::bad_alloc if first-use process-manager allocation fails.
   * @note Kernel destruction does not unload operation plugins. Explicit Host
   *       or internal unload affects every Host consistently.
   */
  PluginManager& plugins() { return PluginManager::process_instance(); }
  std::optional<double> get_last_io_time(const std::string& name);

  // [新增] 暴露 Metal 设备访问器
  id get_metal_device(const std::string& name);

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

  /**
   * @brief Replaces one session scheduler through graph-state serialization.
   * @param name Graph session name.
   * @param intent Compute intent whose scheduler is replaced.
   * @param type Registered scheduler type name.
   * @return True when the graph and scheduler type exist and replacement
   *         succeeds; false for a handled lookup or lifecycle failure.
   * @throws std::bad_alloc if scheduler creation or graph-state submission
   *         exhausts memory.
   * @note The graph-state boundary is held until active compute has released
   *       every scheduler reference, so replacement cannot destroy a retained
   *       scheduler object.
   */
  bool replace_scheduler(const std::string& name, ComputeIntent intent,
                         const std::string& type);

  /**
   * @brief Copies one coherent scheduler information snapshot.
   * @param name Graph session name.
   * @param intent Compute intent whose scheduler is inspected.
   * @return Owned scheduler name/statistics, or nullopt when unavailable.
   * @throws std::bad_alloc if graph-state submission or copied text allocation
   *         fails.
   * @note Inspection uses the same graph-state serialization boundary as
   *       compute and replacement; no scheduler pointer escapes the callback.
   */
  std::optional<std::pair<std::string, std::string>> get_scheduler_info(
      const std::string& name, ComputeIntent intent);

 private:
  friend class testing::KernelTestAccess;

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
   * @brief Builds backend traversal metadata for Host adapter conversion.
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
   * paths. Frontends receive a copied Host snapshot, not this Kernel type.
   */
  TraversalNodeInfo build_traversal_node_info(const GraphModel& graph,
                                              int node_id) const;

  /**
   * @brief Builds traversal metadata for one ending node.
   *
   * @param graph Graph whose traversal order should be inspected.
   * @param end_node_id Ending node used as the traversal root.
   * @return Node metadata in topological postorder, or nullopt when this
   * traversal branch cannot be inspected after a recoverable failure.
   * @throws std::bad_alloc if traversal or metadata collection exhausts
   * memory.
   * @note The caller still receives details for other ending nodes when one
   * branch fails.
   */
  std::optional<std::vector<TraversalNodeInfo>> traversal_details_for_end(
      GraphModel& graph, int end_node_id) const;

  /**
   * @brief Borrowed graph-name parameter used by private runtime helpers.
   *
   * @note This private alias names the existing `const std::string&` calling
   * convention; it does not take ownership of, copy, or extend the graph-name
   * lifetime.
   */
  using GraphName = const std::string&;

  /**
   * @brief Result wrapper for const graph-runtime facade callbacks.
   *
   * @tparam Fn Callable type invoked with `const GraphRuntime&`.
   * @note This alias keeps the const `with_runtime` signature stable across
   * the clang-format versions used by local development and CI without changing
   * the private helper's exception or lifetime behavior.
   */
  template <typename Fn>
  using ConstRuntimeResult = std::optional<
      std::decay_t<std::invoke_result_t<Fn, const GraphRuntime&>>>;

  /**
   * @brief Executes a graph-runtime facade operation with missing-graph and
   * exception-to-nullopt handling.
   *
   * @param name Graph name to resolve.
   * @param op Callable invoked as op(GraphRuntime&).
   * @return Optional operation result, or nullopt when the graph is missing or
   * the operation throws a recoverable failure.
   * @throws std::bad_alloc if the runtime operation exhausts memory.
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
    } catch (const std::bad_alloc&) {
      throw;
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
   * the operation throws a recoverable failure.
   * @throws std::bad_alloc if the const runtime operation exhausts memory.
   * @note Used by const inspection APIs such as scheduler metadata queries.
   */
  template <typename Fn>
  auto with_runtime(GraphName name, Fn&& op) const -> ConstRuntimeResult<Fn> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      return std::nullopt;
    }
    try {
      return std::forward<Fn>(op)(*it->second);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (...) {
      return std::nullopt;
    }
  }

  /**
   * @brief Executes one required serialized GraphModel operation without
   *        collapsing its exact failure.
   *
   * @tparam Fn Callable accepted by GraphStateExecutor as `op(GraphModel&)`.
   * @param name Required graph session name.
   * @param op Callable submitted to the session's graph-state executor.
   * @return Exact callable result.
   * @throws GraphError with `GraphErrc::NotFound` when no session map entry
   *         exists, or any GraphError raised by the operation unchanged.
   * @throws std::bad_alloc if lookup diagnostics, submission, or operation
   *         execution exhausts memory.
   * @throws std::exception for other submission, future, or operation
   *         failures.
   * @note Callers that can race Kernel::close_graph() must retain an equivalent
   *       runtime-lifetime admission across this helper. Embedded Host checked
   *       operations use its lifecycle admission gate. Model validation and
   *       mutation belong in the single submitted callable, avoiding a
   *       check-then-act gap between graph-state work items.
   */
  template <typename Fn>
  auto with_required_graph_state(const std::string& name, Fn&& op)
      -> std::decay_t<std::invoke_result_t<Fn, GraphModel&>> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      throw GraphError(GraphErrc::NotFound, "Graph session not found: " + name);
    }
    return it->second->graph_state().submit(std::forward<Fn>(op)).get();
  }

  /**
   * @brief Executes one serialized GraphModel operation through the graph-state
   * executor.
   *
   * @tparam Fn Callable accepted by GraphStateExecutor as `op(GraphModel&)`.
   * @param name Graph name to resolve.
   * @param op Callable submitted as op(GraphModel&).
   * @return Optional result from op, or nullopt when the graph is missing or a
   * recoverable submit/get/op failure occurs.
   * @throws std::bad_alloc if submission, future consumption, or the operation
   *         exhausts memory.
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
    } catch (const std::bad_alloc&) {
      throw;
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
   * @throws std::bad_alloc if runtime startup, submission, the operation, or
   *         handled-failure LastError construction exhausts memory.
   * @note Use this helper for facade APIs whose existing contract exposes
   * best-effort LastError details. Historically quiet accessors should keep
   * using with_graph_state so they continue to hide diagnostic state.
   * GraphError and other std::exception failures otherwise become nullopt.
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
      clear_last_error(name);
      return result;
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const GraphError& ge) {
      store_last_error(name, LastError{ge.code(), ge.what()});
      return std::nullopt;
    } catch (const std::exception& e) {
      store_last_error(name,
                       LastError{GraphErrc::Unknown,
                                 std::string(exception_prefix) + e.what()});
      return std::nullopt;
    }
  }

  /**
   * @brief Executes one required serialized graph-state operation while
   *        preserving its exact exception and LastError mirror.
   *
   * @tparam Fn Callable accepted by GraphStateExecutor as `op(GraphModel&)`.
   * @param name Required graph session name.
   * @param exception_prefix Prefix for the best-effort ordinary-exception
   *        diagnostic.
   * @param op Callable submitted to the session graph-state executor.
   * @param start_runtime Whether to start the owning runtime before submission.
   * @return Exact callable result, including an inner optional when the
   *         operation uses empty success/failure semantics.
   * @throws GraphError with `GraphErrc::NotFound` when the session map entry is
   *         absent, or any GraphError raised inside the work item unchanged.
   * @throws std::bad_alloc if startup, submission, operation, or diagnostic
   *         recording exhausts memory.
   * @throws std::exception for other runtime, executor, or operation failures.
   * @note Missing sessions do not create LastError state. Existing-session
   *       success clears stale state; GraphError and ordinary exceptions are
   *       recorded before rethrow. Callers that can race close must retain an
   *       equivalent runtime-lifetime admission across this helper.
   */
  template <typename Fn>
  auto with_required_graph_state_last_error(const std::string& name,
                                            const char* exception_prefix,
                                            Fn&& op, bool start_runtime = false)
      -> std::decay_t<std::invoke_result_t<Fn, GraphModel&>> {
    auto it = graphs_.find(name);
    if (it == graphs_.end()) {
      throw GraphError(GraphErrc::NotFound, "Graph session not found: " + name);
    }
    try {
      if (start_runtime && !it->second->running()) {
        it->second->start();
      }
      auto result =
          it->second->graph_state().submit(std::forward<Fn>(op)).get();
      clear_last_error(name);
      return result;
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const GraphError& error) {
      store_last_error(name, LastError{error.code(), error.what()});
      throw;
    } catch (const std::exception& error) {
      store_last_error(name,
                       LastError{GraphErrc::Unknown,
                                 std::string(exception_prefix) + error.what()});
      throw;
    }
  }

  /**
   * @brief Removes one graph's best-effort diagnostic snapshot.
   *
   * @param name Graph/session name whose diagnostic is cleared.
   * @return Nothing.
   * @throws std::system_error if locking the diagnostic mutex fails.
   * @note Every access to `last_error_`, including accesses from independent
   *       graph-state executors, uses the same mutex through these helpers.
   */
  void clear_last_error(const std::string& name);

  /**
   * @brief Publishes one owned best-effort diagnostic snapshot.
   *
   * @param name Graph/session name whose diagnostic is replaced.
   * @param error Fully constructed diagnostic moved into the shared map.
   * @return Nothing.
   * @throws std::bad_alloc if the graph key or map node cannot be allocated.
   * @throws std::system_error if locking the diagnostic mutex fails.
   * @note Async callers retain a separate owned result; this mirror exists only
   *       for the legacy `last_error()` observation surface.
   */
  void store_last_error(const std::string& name, LastError error);

  /**
   * @brief Copies one graph's best-effort diagnostic snapshot.
   *
   * @param name Graph/session name to inspect.
   * @return Copied diagnostic, or nullopt when no failure is recorded.
   * @throws std::bad_alloc if copying the diagnostic text fails.
   * @throws std::system_error if locking the diagnostic mutex fails.
   * @note The copy is completed while holding `last_error_mutex_`; no map
   *       iterator or borrowed string escapes the protected region.
   */
  std::optional<LastError> copy_last_error(const std::string& name) const;

  /**
   * @brief Runs internal synchronous compute from an adapter request object.
   *
   * @param request Internal Kernel request translated from Host values by the
   * embedded adapter, or constructed by an internal test/backend caller.
   * @return true when ComputeService completes successfully; false when the
   * graph is missing or compute reports a handled failure.
   * @throws std::bad_alloc if compute execution or construction of handled-
   *         failure LastError state exhausts memory.
   * @note quiet and skip-save are applied only for the active request and are
   * restored on every success or failure exit path. GraphError, ordinary
   * std::exception, and unknown failures otherwise map to false plus LastError.
   */
  bool compute_request(const ComputeRequest& request);

  /**
   * @brief Schedules internal async compute from an adapter request object.
   *
   * @param request Internal async Kernel request captured by value.
   * @return Future that resolves to the work item's owned exact result, or
   *         nullopt when the graph is missing.
   * @throws std::bad_alloc if request, task, queue, or future-state allocation
   *         fails while submitting graph-state work.
   * @throws std::system_error if GraphStateExecutor cannot launch the async
   * graph-state work.
   * @note The returned future owns the request copy. benchmark_events remains
   * caller-owned and must outlive future completion. Future get() may rethrow
   * std::bad_alloc from compute execution or exact diagnostic construction.
   */
  std::optional<std::future<AsyncComputeResult>> compute_async_request(
      ComputeRequest request);

  /**
   * @brief Runs compute and returns the target output as an OpenCV image.
   *
   * @param request Internal compute request with image-returning arguments.
   * @return Cloned cv::Mat target image, or nullopt on missing graph, compute
   * failure, or empty output.
   * @throws std::bad_alloc if compute/image execution or handled-failure
   *         LastError construction exhausts memory.
   * @note Missing graphs return nullopt before LastError state is touched.
   * Successful compute paths clear stale LastError state, including the
   * no-image-output case. Other compute/image exceptions become nullopt.
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
   * @throws GraphError if ComputeService rejects the graph request.
   * @throws std::bad_alloc if request translation or ComputeService exhausts
   *         memory.
   * @throws std::exception for other failures propagated by ComputeService.
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

  /**
   * @brief Serializes all reads, writes, and erases of `last_error_`.
   *
   * @note The mutex is independent of per-graph GraphStateExecutor instances
   *       because asynchronous computes for different sessions may publish
   *       diagnostics concurrently. It is never held while graph-state or
   *       scheduler work executes.
   */
  mutable std::mutex last_error_mutex_;

  /**
   * @brief Best-effort per-session diagnostic mirror for legacy inspection.
   *
   * @note Access is permitted only through clear_last_error(),
   *       store_last_error(), and copy_last_error() while
   *       `last_error_mutex_` is held. Async operation results never derive
   *       their final status from this map.
   */
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
