#pragma once

/**
 * @file interaction.hpp
 * @brief Private interaction facade between the embedded Host adapter and
 * Kernel.
 *
 * This header lives under the private `src/lib/` include root and is not part
 * of the installable `include/photospider/` public interface. It includes
 * `kernel/kernel.hpp`, which depends on private implementation headers;
 * repository targets that use InteractionService must receive the private
 * include roots. CLI/TUI and external frontends use
 * `photospider/host/host.hpp`; they do not include this source-tree facade
 * through the static `photospider` product.
 */

#include <yaml-cpp/yaml.h>

#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "plugin/plugin_result.hpp"
#include "runtime/kernel.hpp"

namespace ps {

/**
 * @brief Command-oriented facade that keeps backend adapters on Kernel value
 * APIs.
 *
 * InteractionService owns no graph state. It translates embedded-Host/backend
 * command calls to Kernel facades and deliberately does not expose the
 * underlying Kernel reference, runtime map, or graph-state executor. Tests that
 * must inspect internals use the internal-only helper under tests/support;
 * frontend code calls public `ps::Host` methods, while the embedded adapter
 * uses the `cmd_*` value accessors here.
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

  /**
   * @brief Forwards one transactional Graph load to the bound Kernel.
   * @param name Unique graph/session label.
   * @param root_dir Root directory that owns the session folder.
   * @param yaml_path Empty session-local-or-empty selector, or explicit source
   *        YAML copied before graph parsing.
   * @param config_path Optional session configuration source.
   * @param cache_root_dir Optional external cache-root directory.
   * @return Loaded graph label, or nullopt only for a duplicate session name.
   * @throws GraphError with exact scheduler, IO, YAML, schema, topology, or
   *         unexpected-internal failure categories before Graph publication.
   * @throws std::bad_alloc If Kernel load or error translation exhausts
   *         memory.
   * @note Kernel plans and atomically admits both scheduler intents before
   *       constructing either candidate. This final embedded boundary keeps
   *       GraphError unchanged, maps BadFile/filesystem failures to Io,
   *       residual YAML failures to InvalidYaml, and other exceptions to
   *       Unknown without adding fallback or partial publication.
   */
  std::optional<std::string> cmd_load_graph(
      const std::string& name, const std::string& root_dir,
      const std::string& yaml_path, const std::string& config_path = "",
      const std::string& cache_root_dir = "") {
    try {
      return kernel_.load_graph(name, root_dir, yaml_path, config_path,
                                cache_root_dir);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const GraphError&) {
      throw;
    } catch (const YAML::BadFile& error) {
      throw GraphError(GraphErrc::Io, "Graph YAML file access failed: " +
                                          std::string(error.what()));
    } catch (const YAML::Exception& error) {
      throw GraphError(
          GraphErrc::InvalidYaml,
          "Graph YAML processing failed: " + std::string(error.what()));
    } catch (const std::filesystem::filesystem_error& error) {
      throw GraphError(GraphErrc::Io, "Graph filesystem preparation failed: " +
                                          std::string(error.what()));
    } catch (const std::exception& error) {
      throw GraphError(GraphErrc::Unknown, "Unexpected graph load failure: " +
                                               std::string(error.what()));
    } catch (...) {
      throw GraphError(GraphErrc::Unknown,
                       "Unknown non-standard graph load failure");
    }
  }
  /**
   * @brief Stops graph-state admission for the first phase of Host close.
   * @param name Graph session whose bounded lane must reject new submissions.
   * @return True when the graph exists; false when no runtime is loaded.
   * @throws std::logic_error if invoked from the target lane worker.
   * @throws std::overflow_error if the close-generation counter is exhausted.
   * @throws std::system_error if executor lifecycle synchronization fails.
   * @note The runtime and scheduler remain owned by Kernel. Embedded Host calls
   *       this after publishing its close marker and draining pre-marker
   *       synchronous admissions, but before waiting on pre-registered async
   *       placeholders.
   */
  bool cmd_stop_graph_admission(const std::string& name) {
    return kernel_.stop_graph_admission(name);
  }

  /**
   * @brief Completes graph close after Host admission drainage.
   * @param name Graph session whose lane and schedulers must be torn down.
   * @return True when the runtime existed and was removed; false when absent.
   * @throws Any executor or scheduler lifecycle failure propagated by Kernel.
   * @note Kernel drains and joins the already-stopped lane before scheduler
   *       shutdown. A scheduler stop failure reopens one lane worker before it
   *       is rethrown.
   */
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
   * @brief Runs synchronous compute from an adapter-translated Kernel request.
   *
   * @param request Internal graph, target, cache, execution, telemetry, and
   * optional dirty/intent controls produced by the embedded Host adapter from
   * a public HostComputeRequest.
   * @return true when Kernel compute succeeds; false on missing graph or
   * handled compute failure.
   * @throws std::bad_alloc if Kernel compute execution or handled-failure
   *         LastError construction exhausts memory.
   * @note Frontends submit HostComputeRequest to ps::Host and never construct
   * this Kernel::ComputeRequest directly. The request is forwarded without
   * retaining borrowed benchmark sinks.
   */
  bool cmd_compute(const Kernel::ComputeRequest& request) {
    return kernel_.compute(request);
  }
  std::optional<TimingCollector> cmd_timing(const std::string& graph) {
    return kernel_.get_timing(graph);
  }

  // Plugins
  /**
   * @brief Loads operation plugins into the process-global owner.
   * @param dirs Directory patterns to scan.
   * @return Nothing.
   * @throws Exceptions from PluginManager::load_from_dirs unchanged.
   * @note Mutation is serialized with every Host plugin mutation/inspection.
   */
  void cmd_plugins_load(const std::vector<std::string>& dirs) {
    kernel_.plugins().load_from_dirs(dirs);
  }

  /**
   * @brief Loads operation plugins and returns process-global diagnostics.
   * @param dirs Directory patterns to scan.
   * @return Structured report for candidates processed by this call.
   * @throws Exceptions from PluginManager::load_from_dirs_report unchanged.
   * @note Successful handles, source labels, restoration snapshots, and load
   *       order belong to the shared process owner.
   */
  PluginLoadResult cmd_plugins_load_report(
      const std::vector<std::string>& dirs) {
    return kernel_.plugins().load_from_dirs_report(dirs);
  }

  /**
   * @brief Explicitly unloads every process-global operation plugin.
   * @return Number of active registry keys removed or restored.
   * @throws Nothing.
   * @note All Hosts observe the mutation. Callback snapshots already executing
   *       retain their library lease until invocation storage is destroyed.
   */
  int cmd_plugins_unload_all() {
    return kernel_.plugins().unload_all_plugins();
  }

  /**
   * @brief Seeds process-global built-in operation source labels.
   * @return Nothing.
   * @throws std::bad_alloc if registry/source storage allocation fails.
   * @note Seeding is serialized with plugin mutation and inspection.
   */
  void cmd_seed_builtin_ops() {
    kernel_.plugins().seed_builtins_from_registry();
  }

  // Ops overview (type:subtype -> source path or "built-in")
  /**
   * @brief Copies process-global operation source labels.
   * @return Coherent operation-key to source snapshot.
   * @throws std::bad_alloc if copying exhausts memory.
   * @note The result never borrows manager storage.
   */
  std::map<std::string, std::string> cmd_ops_sources() const {
    return kernel_.plugins().op_sources();
  }
  /**
   * @brief Lists combined operation keys for embedded Host translation.
   *
   * @return Internal combined operation keys with compatible HP/RT/tiled
   *         implementations collapsed.
   * @throws std::bad_alloc if registry snapshot allocation fails.
   * @note Only the embedded Host adapter consumes this internal facade method.
   *       Frontends call `ps::Host::ops_combined_keys()` and receive copied
   *       public values.
   */
  std::vector<std::string> cmd_ops_combined_keys() const {
    return kernel_.plugins().combined_keys();
  }

  /**
   * @brief Maps combined operation keys to internal source labels.
   *
   * @return Copied source labels keyed by combined operation key.
   * @throws std::bad_alloc if map or string allocation fails.
   * @note The embedded Host adapter converts this internal value into the
   *       result of `ps::Host::ops_combined_sources()`; frontend code never
   *       calls InteractionService directly.
   */
  std::map<std::string, std::string> cmd_ops_combined_sources() const {
    return kernel_.plugins().combined_sources();
  }

  // IO / cache / traversal / printing
  /**
   * @brief Reloads an existing graph from YAML through the Kernel facade.
   *
   * @param graph Existing graph/session name.
   * @param yaml_path Source YAML file path.
   * @return True on success; false for missing graphs or reload failures
   *         recorded in Kernel LastError.
   * @throws std::bad_alloc if reload execution or LastError construction
   *         exhausts memory.
   * @note Missing graphs do not create LastError. Existing-session empty-path,
   *       IO, syntax/schema, topology, and unexpected failures retain
   *       InvalidParameter, Io, InvalidYaml, MissingDependency/Cycle, and
   *       Unknown respectively. Embedded Host retains its close admission
   *       across this command and later public LastError translation. This
   *       internal method does not expose Kernel to frontends; public callers
   *       use `ps::Host::reload_graph()`.
   */
  bool cmd_reload_yaml(const std::string& graph, const std::string& yaml_path) {
    return kernel_.reload_graph_yaml(graph, yaml_path);
  }
  /**
   * @brief Saves one required graph session through the Kernel exact boundary.
   *
   * @param graph Graph session name to save.
   * @param yaml_path Destination YAML file path.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::NotFound` for an absent session or
   *         `GraphErrc::Io` for recoverable node serialization, YAML emission,
   *         or destination preparation/open/write/flush/close failure.
   * @throws std::bad_alloc if graph-state submission, node/YAML serialization,
   *         path handling, or diagnostic construction exhausts memory.
   * @throws std::exception for other graph-state submission or future failures.
   * @note Embedded Host retains its close admission while this command resolves
   *       and uses the graph runtime. Stream, serialization, and emitter
   *       exceptions are normalized to GraphErrc::Io by Kernel. All outcomes
   *       preserve graph topology, runtime state, and session ownership.
   *       Direct output is not an atomic replacement: a pre-open failure
   *       preserves existing bytes, while post-open failure may leave created,
   *       truncated, or partial output.
   */
  void cmd_save_yaml(const std::string& graph, const std::string& yaml_path) {
    kernel_.save_graph_yaml(graph, yaml_path);
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
  /**
   * @brief Drains one bounded compute-event batch through Kernel.
   * @param graph Loaded graph/session name.
   * @param limit Maximum events to remove.
   * @return Sequenced batch, or nullopt when the graph is missing.
   * @throws std::invalid_argument for an invalid limit without mutation.
   * @throws std::bad_alloc if output allocation fails without mutation.
   * @note This facade does not add a second buffer or destructive step.
   */
  std::optional<ComputeEventBatch> cmd_drain_compute_events(
      const std::string& graph, std::size_t limit) {
    return kernel_.drain_compute_events(graph, limit);
  }

  /**
   * @brief Reads one bounded non-destructive scheduler-trace page.
   * @param graph Loaded graph/session name.
   * @param after_sequence Exclusive sequence cursor.
   * @param limit Maximum trace entries to copy.
   * @return Internal trace page, or nullopt when the graph is missing.
   * @throws std::invalid_argument for an invalid limit or future cursor.
   * @throws std::bad_alloc if bounded output allocation fails.
   * @note All page metadata comes from the runtime's single locked
   *       observation point.
   */
  std::optional<GraphRuntime::SchedulerEventPage> cmd_scheduler_trace(
      const std::string& graph, uint64_t after_sequence, std::size_t limit) {
    return kernel_.scheduler_trace(graph, after_sequence, limit);
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
   * @brief Computes an image from an adapter-translated Kernel request.
   *
   * @param request Internal graph, target, cache, execution, telemetry, and
   * optional dirty/intent controls produced by the embedded Host adapter from
   * a public HostComputeRequest.
   * @return Cloned image, or nullopt when compute or image extraction fails.
   * @throws std::bad_alloc if Kernel compute/image execution or handled-
   *         failure LastError construction exhausts memory.
   * @note Frontends submit HostComputeRequest to ps::Host and never construct
   * this Kernel::ComputeRequest directly. The request is not retained after
   * image extraction completes.
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
  /**
   * @brief Replaces one required graph node from YAML text.
   *
   * @param graph Required graph session name.
   * @param node_id Required existing node id whose identity is preserved.
   * @param yaml_text Candidate replacement YAML mapping.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::NotFound` for an absent graph/node or
   *         `GraphErrc::InvalidYaml` for parsing/topology validation failure.
   * @throws std::bad_alloc if parsing, validation, submission, or replacement
   *         exhausts memory.
   * @throws std::exception for other graph-state executor failures.
   * @note Embedded Host retains its close admission while Kernel performs the
   *       required-node lookup and replacement in one graph-state work item.
   */
  void cmd_set_node_yaml(const std::string& graph, int node_id,
                         const std::string& yaml_text) {
    kernel_.set_node_yaml(graph, node_id, yaml_text);
  }
  /**
   * @brief Schedules an asynchronous compute command from a request object.
   *
   * @param request Graph, target node, cache, execution, telemetry, and
   * optional dirty/intent controls captured by value.
   * @return Future resolving to the work item's owned exact result, or nullopt
   *         when the graph is missing.
   * @throws std::bad_alloc if request, task, queue, or future-state allocation
   *         fails while Kernel schedules graph-state work.
   * @throws std::runtime_error if graph-state admission has stopped.
   * @throws std::system_error if Kernel cannot launch runtime or graph-state
   *         asynchronous execution.
   * @note benchmark_events is still caller-owned and must outlive the future.
   *       Future get() may rethrow std::bad_alloc from compute execution or
   *       exact diagnostic construction.
   */
  std::optional<std::future<Kernel::AsyncComputeResult>> cmd_compute_async(
      Kernel::ComputeRequest request) {
    return kernel_.compute_async(std::move(request));
  }
  /**
   * @brief Projects an ROI forward between required nodes in one graph.
   *
   * @param graph Required graph session name.
   * @param start_node_id Required source node id.
   * @param start_roi Source ROI in output coordinates.
   * @param target_node_id Required downstream target node id.
   * @return Projected ROI, or nullopt when existing endpoints produce no valid
   *         projection.
   * @throws GraphError with `GraphErrc::NotFound` for an absent graph/endpoint
   *         or another exact propagation failure category.
   * @throws std::bad_alloc if runtime startup, submission, projection, or
   *         diagnostics exhaust memory.
   * @throws std::exception for other runtime/executor failures.
   * @note Embedded Host retains its close admission while endpoint lookup and
   *       projection execute in one graph-state work item.
   */
  std::optional<cv::Rect> cmd_project_roi(const std::string& graph,
                                          int start_node_id,
                                          const cv::Rect& start_roi,
                                          int target_node_id) {
    return kernel_.project_roi_forward(graph, start_node_id, start_roi,
                                       target_node_id);
  }

  /**
   * @brief Projects an ROI backward between required nodes in one graph.
   *
   * @param graph Required graph session name.
   * @param target_node_id Required downstream target node id.
   * @param target_roi Target ROI in output coordinates.
   * @param source_node_id Required upstream source node id.
   * @return Projected source ROI, or nullopt when existing endpoints produce no
   *         valid projection.
   * @throws GraphError with `GraphErrc::NotFound` for an absent graph/endpoint
   *         or another exact propagation failure category.
   * @throws std::bad_alloc if runtime startup, submission, projection, or
   *         diagnostics exhaust memory.
   * @throws std::exception for other runtime/executor failures.
   * @note Embedded Host retains its close admission while endpoint lookup and
   *       projection execute in one graph-state work item.
   */
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
