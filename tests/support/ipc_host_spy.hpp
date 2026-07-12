#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/host/host.hpp"

namespace ps::testing {

/**
 * @brief One copied invocation observed by `IpcHostSpy`.
 *
 * The record uses the version 1 wire method name so protocol tests can compare
 * routing without duplicating a second Host-method vocabulary. Fields that do
 * not apply to an invocation retain their default values.
 *
 * @throws std::bad_alloc when copied strings or containers cannot allocate.
 * @note Records own every captured value and never borrow router JSON or Host
 *       argument storage.
 */
struct IpcHostInvocation {
  /** @brief Version 1 method name associated with the Host call. */
  std::string method;

  /** @brief Exact Host session argument, when the method is session-scoped. */
  GraphSessionId session;

  /** @brief First node argument, when present. */
  NodeId first_node;

  /** @brief Second node argument, when present. */
  NodeId second_node;

  /** @brief Dirty-domain argument, when present. */
  DirtyDomain dirty_domain = DirtyDomain::HighPrecision;

  /** @brief ROI argument, when present. */
  PixelRect roi;

  /** @brief Path, YAML, precision, type, or description argument. */
  std::string text;

  /** @brief Optional node argument used by dependency-tree inspection. */
  std::optional<NodeId> optional_node;

  /** @brief Boolean option used by dependency-tree inspection. */
  bool flag = false;

  /** @brief Compute intent argument, when present. */
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;

  /** @brief Exact graph-load request, when the method is `graph.load`. */
  GraphLoadRequest load_request;

  /** @brief Exact public compute request, when the method is `compute.submit`.
   */
  std::optional<HostComputeRequest> compute_request;

  /** @brief Whether `compute.submit` selected the image-returning Host call. */
  bool image_compute = false;
};

/**
 * @brief Complete configurable Host spy for IPC router protocol tests.
 *
 * Every non-destructor `Host` operation is implemented. Calls are recorded by
 * stable wire name, while Host-routed graph-state value methods expose focused
 * setters for deterministic response encoding tests. Unconfigured operations
 * return canonical success and default-constructed public values.
 *
 * @throws std::bad_alloc when owned configuration or invocation storage cannot
 *         allocate.
 * @note The spy is test-only and contains no backend objects. Invocation and
 *       status state are mutex-protected so router serialization and lifecycle
 *       races can be exercised without introducing a data race in the spy.
 */
class IpcHostSpy final : public Host {
 public:
  /**
   * @brief Callable invoked inside a Host call after argument recording.
   * @throws Whatever the installed callable throws when invoked.
   * @note The callback receives a borrowed method-name view valid only for the
   *       invocation. `set_call_hook()` documents its synchronization limits.
   */
  using CallHook = std::function<void(std::string_view)>;

  /**
   * @brief Creates a spy that publishes one deterministic Host session.
   *
   * @param host_session Private Host session returned by successful load.
   * @throws std::bad_alloc if the session label cannot be copied.
   * @note The session is not listed until `load_graph()` succeeds.
   */
  explicit IpcHostSpy(
      GraphSessionId host_session = GraphSessionId{"ipc-host-spy-session"})
      : host_session_(std::move(host_session)) {}

  /**
   * @brief Removes all recorded calls without changing configured results.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note Tests normally call this after routing `graph.load` so assertions
   *       cover only the method under test.
   */
  void reset_invocations() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    invocations_.clear();
  }

  /**
   * @brief Returns a copied snapshot of all calls in entry order.
   *
   * @return Owned invocation records ordered by Host entry.
   * @throws std::bad_alloc if the copied vector or strings cannot allocate.
   */
  std::vector<IpcHostInvocation> invocations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return invocations_;
  }

  /**
   * @brief Counts calls recorded for one stable method name.
   *
   * @param method Version 1 wire method name.
   * @return Number of matching Host entries.
   * @throws Nothing.
   */
  std::size_t call_count(std::string_view method) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(
        std::count_if(invocations_.begin(), invocations_.end(),
                      [method](const IpcHostInvocation& invocation) {
                        return invocation.method == method;
                      }));
  }

  /**
   * @brief Configures the status returned by one Host operation.
   *
   * @param method Stable wire method name used by invocation records.
   * @param status Exact public status to return.
   * @return Nothing.
   * @throws std::bad_alloc if map or diagnostic storage cannot allocate.
   * @note The configured operation still records and executes exactly once.
   */
  void set_status(std::string method, OperationStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    statuses_[std::move(method)] = std::move(status);
  }

  /**
   * @brief Installs a synchronization hook for Host-entry race tests.
   *
   * @param hook Callable receiving the recorded stable method name, or an empty
   *        callable to disable synchronization.
   * @return Nothing.
   * @throws std::bad_alloc if callable state cannot be copied.
   * @note The hook runs after the invocation record is committed and outside
   *       the spy mutex. It runs while the router still owns its Host mutex,
   *       so it may query this spy or block on a one-way test gate but must not
   *       synchronously reenter a Host-backed route or form a wait cycle.
   */
  void set_call_hook(CallHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    call_hook_ = std::move(hook);
  }

  /**
   * @brief Configures `graph.node_yaml.get` copied output.
   * @param yaml_text Exact YAML text returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied text cannot allocate.
   */
  void set_node_yaml(std::string yaml_text) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_yaml_ = std::move(yaml_text);
  }

  /**
   * @brief Configures `inspect.node_ids` copied output.
   * @param nodes Ordered node ids returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_node_ids(std::vector<NodeId> nodes) {
    std::lock_guard<std::mutex> lock(mutex_);
    node_ids_ = std::move(nodes);
  }

  /**
   * @brief Configures `inspect.ending_nodes` copied output.
   * @param nodes Ordered ending-node ids returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_ending_nodes(std::vector<NodeId> nodes) {
    std::lock_guard<std::mutex> lock(mutex_);
    ending_nodes_ = std::move(nodes);
  }

  /**
   * @brief Configures copied output for every dirty lifecycle method.
   * @param snapshot Complete public dirty-region value returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied snapshot storage cannot allocate.
   */
  void set_dirty_region(DirtyRegionInspectionSnapshot snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    dirty_region_ = std::move(snapshot);
  }

  /**
   * @brief Configures `inspect.roi_forward` copied output.
   * @param roi Exact rectangle returned on success.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_forward_roi(PixelRect roi) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    forward_roi_ = roi;
  }

  /**
   * @brief Configures `inspect.roi_backward` copied output.
   * @param roi Exact rectangle returned on success.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_backward_roi(PixelRect roi) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    backward_roi_ = roi;
  }

  /**
   * @brief Configures `compute.timing` copied output.
   * @param timing Complete public timing snapshot returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied snapshot storage cannot allocate.
   */
  void set_timing(TimingSnapshot timing) {
    std::lock_guard<std::mutex> lock(mutex_);
    timing_ = std::move(timing);
  }

  /**
   * @brief Configures `compute.last_io_time` copied output.
   * @param milliseconds Exact Host diagnostic value.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_last_io_time(double milliseconds) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    last_io_time_ = milliseconds;
  }

  /**
   * @brief Configures the nested `compute.last_error` status value.
   * @param status Exact Host diagnostic status.
   * @return Nothing.
   * @throws std::bad_alloc if diagnostic copies cannot allocate.
   */
  void set_last_error(OperationStatus status) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(status);
  }

  /**
   * @brief Configures the copied image returned by image-mode compute.
   * @param image Exact public image value returned on Host success.
   * @return Nothing.
   * @throws std::bad_alloc if shared ownership or copied metadata allocates.
   * @note The configured `compute.submit` status remains authoritative; tests
   *       may provide an intentionally invalid image to exercise nested output
   *       publication failures after an accepted submit.
   */
  void set_compute_image(ImageBuffer image) {
    std::lock_guard<std::mutex> lock(mutex_);
    compute_image_ = std::move(image);
  }

  /**
   * @brief Configures `inspect.node` copied output.
   * @param node Complete public node value returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_inspected_node(NodeInspectionView node) {
    std::lock_guard<std::mutex> lock(mutex_);
    inspected_node_ = std::move(node);
  }

  /**
   * @brief Configures `inspect.graph` copied output.
   * @param graph Complete public graph value returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_inspected_graph(GraphInspectionView graph) {
    std::lock_guard<std::mutex> lock(mutex_);
    inspected_graph_ = std::move(graph);
  }

  /**
   * @brief Configures `inspect.dependency_tree` copied output.
   * @param tree Complete public flattened tree returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_dependency_tree(HostDependencyTreeSnapshot tree) {
    std::lock_guard<std::mutex> lock(mutex_);
    dependency_tree_ = std::move(tree);
  }

  /**
   * @brief Configures `inspect.traversal_orders` copied output.
   * @param orders Complete ending-node traversal map returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_traversal_orders(std::map<int, std::vector<NodeId>> orders) {
    std::lock_guard<std::mutex> lock(mutex_);
    traversal_orders_ = std::move(orders);
  }

  /**
   * @brief Configures `inspect.traversal_details` copied output.
   * @param details Complete ending-node detail map returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_traversal_details(
      std::map<int, std::vector<HostTraversalNodeSnapshot>> details) {
    std::lock_guard<std::mutex> lock(mutex_);
    traversal_details_ = std::move(details);
  }

  /**
   * @brief Configures `inspect.trees_containing_node` copied output.
   * @param roots Ordered ending-node ids returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_trees_containing_node(std::vector<NodeId> roots) {
    std::lock_guard<std::mutex> lock(mutex_);
    trees_containing_node_ = std::move(roots);
  }

  /**
   * @brief Configures the optional latest planning inspection value.
   * @param planning Optional copied snapshot returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_compute_planning(
      std::optional<ComputePlanningInspectionSnapshot> planning) {
    std::lock_guard<std::mutex> lock(mutex_);
    compute_planning_ = std::move(planning);
  }

  /**
   * @brief Configures recent planning inspection history.
   * @param snapshots Ordered copied history returned on success.
   * @return Nothing.
   * @throws std::bad_alloc if copied storage cannot allocate.
   */
  void set_recent_compute_planning(
      std::vector<ComputePlanningInspectionSnapshot> snapshots) {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_compute_planning_ = std::move(snapshots);
  }

  /** @copydoc Host::load_graph */
  Result<GraphSessionId> load_graph(const GraphLoadRequest& request) override {
    IpcHostInvocation invocation;
    invocation.method = "graph.load";
    invocation.session = request.session;
    invocation.load_request = request;
    record(std::move(invocation));
    const OperationStatus status = status_for("graph.load");
    GraphSessionId host_session;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      host_session = host_session_;
      if (status.ok) {
        loaded_ = true;
      }
    }
    return {status, std::move(host_session)};
  }

  /** @copydoc Host::close_graph */
  VoidResult close_graph(const GraphSessionId& session) override {
    record(session_invocation("graph.close", session));
    const OperationStatus status = status_for("graph.close");
    if (status.ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      loaded_ = false;
    }
    return {status};
  }

  /** @copydoc Host::list_graphs */
  Result<std::vector<GraphSessionId>> list_graphs() const override {
    record(method_invocation("graph.list"));
    const OperationStatus status = status_for("graph.list");
    std::vector<GraphSessionId> sessions;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (loaded_) {
        sessions.push_back(host_session_);
      }
    }
    return {status, std::move(sessions)};
  }

  /** @copydoc Host::reload_graph */
  VoidResult reload_graph(const GraphSessionId& session,
                          const std::string& yaml_path) override {
    record(text_invocation("graph.reload", session, yaml_path));
    return {status_for("graph.reload")};
  }

  /** @copydoc Host::save_graph */
  VoidResult save_graph(const GraphSessionId& session,
                        const std::string& yaml_path) override {
    record(text_invocation("graph.save", session, yaml_path));
    return {status_for("graph.save")};
  }

  /** @copydoc Host::clear_graph */
  VoidResult clear_graph(const GraphSessionId& session) override {
    record(session_invocation("graph.clear", session));
    return {status_for("graph.clear")};
  }

  /** @copydoc Host::compute */
  VoidResult compute(const HostComputeRequest& request) override {
    IpcHostInvocation invocation =
        session_invocation("compute.submit", request.session);
    invocation.compute_request = request;
    invocation.image_compute = false;
    record(std::move(invocation));
    return {status_for("compute.submit")};
  }

  /** @copydoc Host::compute_async */
  Result<std::future<OperationStatus>> compute_async(
      HostComputeRequest request) override {
    record(session_invocation("compute.submit", request.session));
    const OperationStatus status = status_for("compute.submit");
    std::promise<OperationStatus> promise;
    promise.set_value(status);
    return {status, promise.get_future()};
  }

  /** @copydoc Host::compute_and_get_image */
  Result<ImageBuffer> compute_and_get_image(
      const HostComputeRequest& request) override {
    IpcHostInvocation invocation =
        session_invocation("compute.submit", request.session);
    invocation.compute_request = request;
    invocation.image_compute = true;
    record(std::move(invocation));
    return configured_result("compute.submit", compute_image_);
  }

  /** @copydoc Host::timing */
  Result<TimingSnapshot> timing(const GraphSessionId& session) override {
    record(session_invocation("compute.timing", session));
    return configured_result("compute.timing", timing_);
  }

  /** @copydoc Host::last_io_time */
  Result<double> last_io_time(const GraphSessionId& session) const override {
    record(session_invocation("compute.last_io_time", session));
    return configured_result("compute.last_io_time", last_io_time_);
  }

  /** @copydoc Host::last_error */
  OperationStatus last_error(const GraphSessionId& session) const override {
    record(session_invocation("compute.last_error", session));
    return configured_value(last_error_);
  }

  /** @copydoc Host::list_node_ids */
  Result<std::vector<NodeId>> list_node_ids(
      const GraphSessionId& session) override {
    record(session_invocation("inspect.node_ids", session));
    return configured_result("inspect.node_ids", node_ids_);
  }

  /** @copydoc Host::ending_nodes */
  Result<std::vector<NodeId>> ending_nodes(
      const GraphSessionId& session) override {
    record(session_invocation("inspect.ending_nodes", session));
    return configured_result("inspect.ending_nodes", ending_nodes_);
  }

  /** @copydoc Host::get_node_yaml */
  Result<std::string> get_node_yaml(const GraphSessionId& session,
                                    NodeId node) override {
    record(node_invocation("graph.node_yaml.get", session, node));
    return configured_result("graph.node_yaml.get", node_yaml_);
  }

  /** @copydoc Host::set_node_yaml */
  VoidResult set_node_yaml(const GraphSessionId& session, NodeId node,
                           const std::string& yaml_text) override {
    IpcHostInvocation invocation =
        node_invocation("graph.node_yaml.set", session, node);
    invocation.text = yaml_text;
    record(std::move(invocation));
    return {status_for("graph.node_yaml.set")};
  }

  /** @copydoc Host::inspect_node */
  Result<NodeInspectionView> inspect_node(const GraphSessionId& session,
                                          NodeId node) override {
    record(node_invocation("inspect.node", session, node));
    return configured_result("inspect.node", inspected_node_);
  }

  /** @copydoc Host::inspect_graph */
  Result<GraphInspectionView> inspect_graph(
      const GraphSessionId& session) override {
    record(session_invocation("inspect.graph", session));
    return configured_result("inspect.graph", inspected_graph_);
  }

  /** @copydoc Host::dependency_tree */
  Result<HostDependencyTreeSnapshot> dependency_tree(
      const GraphSessionId& session, std::optional<NodeId> node,
      bool include_metadata) override {
    IpcHostInvocation invocation =
        session_invocation("inspect.dependency_tree", session);
    invocation.optional_node = node;
    invocation.flag = include_metadata;
    record(std::move(invocation));
    return configured_result("inspect.dependency_tree", dependency_tree_);
  }

  /** @copydoc Host::traversal_orders */
  Result<std::map<int, std::vector<NodeId>>> traversal_orders(
      const GraphSessionId& session) override {
    record(session_invocation("inspect.traversal_orders", session));
    return configured_result("inspect.traversal_orders", traversal_orders_);
  }

  /** @copydoc Host::traversal_details */
  Result<std::map<int, std::vector<HostTraversalNodeSnapshot>>>
  traversal_details(const GraphSessionId& session) override {
    record(session_invocation("inspect.traversal_details", session));
    return configured_result("inspect.traversal_details", traversal_details_);
  }

  /** @copydoc Host::trees_containing_node */
  Result<std::vector<NodeId>> trees_containing_node(
      const GraphSessionId& session, NodeId node) override {
    record(node_invocation("inspect.trees_containing_node", session, node));
    return configured_result("inspect.trees_containing_node",
                             trees_containing_node_);
  }

  /** @copydoc Host::project_roi */
  Result<PixelRect> project_roi(const GraphSessionId& session,
                                NodeId start_node, const PixelRect& start_roi,
                                NodeId target_node) override {
    IpcHostInvocation invocation =
        node_invocation("inspect.roi_forward", session, start_node);
    invocation.second_node = target_node;
    invocation.roi = start_roi;
    record(std::move(invocation));
    return configured_result("inspect.roi_forward", forward_roi_);
  }

  /** @copydoc Host::project_roi_backward */
  Result<PixelRect> project_roi_backward(const GraphSessionId& session,
                                         NodeId target_node,
                                         const PixelRect& target_roi,
                                         NodeId source_node) override {
    IpcHostInvocation invocation =
        node_invocation("inspect.roi_backward", session, target_node);
    invocation.second_node = source_node;
    invocation.roi = target_roi;
    record(std::move(invocation));
    return configured_result("inspect.roi_backward", backward_roi_);
  }

  /** @copydoc Host::dirty_region_snapshot */
  Result<DirtyRegionInspectionSnapshot> dirty_region_snapshot(
      const GraphSessionId& session) override {
    record(session_invocation("inspect.dirty_region", session));
    return configured_result("inspect.dirty_region", dirty_region_);
  }

  /** @copydoc Host::compute_planning_snapshot */
  Result<std::optional<ComputePlanningInspectionSnapshot>>
  compute_planning_snapshot(const GraphSessionId& session) override {
    record(session_invocation("inspect.compute_planning", session));
    return configured_result("inspect.compute_planning", compute_planning_);
  }

  /** @copydoc Host::recent_compute_planning_snapshots */
  Result<std::vector<ComputePlanningInspectionSnapshot>>
  recent_compute_planning_snapshots(const GraphSessionId& session) override {
    record(session_invocation("inspect.recent_compute_planning", session));
    return configured_result("inspect.recent_compute_planning",
                             recent_compute_planning_);
  }

  /** @copydoc Host::begin_dirty_source */
  Result<DirtyRegionInspectionSnapshot> begin_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi) override {
    record(dirty_invocation("dirty.begin", session, node, domain, source_roi));
    return configured_result("dirty.begin", dirty_region_);
  }

  /** @copydoc Host::update_dirty_source */
  Result<DirtyRegionInspectionSnapshot> update_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& source_roi) override {
    record(dirty_invocation("dirty.update", session, node, domain, source_roi));
    return configured_result("dirty.update", dirty_region_);
  }

  /** @copydoc Host::end_dirty_source */
  Result<DirtyRegionInspectionSnapshot> end_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain) override {
    record(dirty_invocation("dirty.end", session, node, domain, PixelRect{}));
    return configured_result("dirty.end", dirty_region_);
  }

  /** @copydoc Host::drain_compute_events */
  Result<ComputeEventBatch> drain_compute_events(const GraphSessionId& session,
                                                 std::size_t limit) override {
    IpcHostInvocation invocation = session_invocation("events.drain", session);
    invocation.first_node.value = static_cast<int>(limit);
    record(std::move(invocation));
    return {status_for("events.drain"), ComputeEventBatch{}};
  }

  /** @copydoc Host::scheduler_trace */
  Result<SchedulerTracePage> scheduler_trace(const GraphSessionId& session,
                                             uint64_t after_sequence,
                                             std::size_t limit) override {
    IpcHostInvocation invocation =
        session_invocation("scheduler.trace", session);
    invocation.first_node.value = static_cast<int>(limit);
    invocation.text = std::to_string(after_sequence);
    record(std::move(invocation));
    return {status_for("scheduler.trace"), SchedulerTracePage{}};
  }

  /** @copydoc Host::clear_cache */
  VoidResult clear_cache(const GraphSessionId& session) override {
    record(session_invocation("cache.clear_all", session));
    return {status_for("cache.clear_all")};
  }

  /** @copydoc Host::clear_drive_cache */
  VoidResult clear_drive_cache(const GraphSessionId& session) override {
    record(session_invocation("cache.clear_drive", session));
    return {status_for("cache.clear_drive")};
  }

  /** @copydoc Host::clear_memory_cache */
  VoidResult clear_memory_cache(const GraphSessionId& session) override {
    record(session_invocation("cache.clear_memory", session));
    return {status_for("cache.clear_memory")};
  }

  /** @copydoc Host::cache_all_nodes */
  VoidResult cache_all_nodes(const GraphSessionId& session,
                             const std::string& precision) override {
    record(text_invocation("cache.cache_all_nodes", session, precision));
    return {status_for("cache.cache_all_nodes")};
  }

  /** @copydoc Host::free_transient_memory */
  VoidResult free_transient_memory(const GraphSessionId& session) override {
    record(session_invocation("cache.free_transient", session));
    return {status_for("cache.free_transient")};
  }

  /** @copydoc Host::synchronize_disk_cache */
  VoidResult synchronize_disk_cache(const GraphSessionId& session,
                                    const std::string& precision) override {
    record(text_invocation("cache.synchronize_disk", session, precision));
    return {status_for("cache.synchronize_disk")};
  }

  /** @copydoc Host::plugins_load_report */
  Result<HostPluginLoadReport> plugins_load_report(
      const std::vector<std::string>& dirs) override {
    IpcHostInvocation invocation = method_invocation("plugins.load_report");
    invocation.text = dirs.empty() ? std::string{} : dirs.front();
    record(std::move(invocation));
    return {status_for("plugins.load_report"), HostPluginLoadReport{}};
  }

  /** @copydoc Host::plugins_load */
  VoidResult plugins_load(const std::vector<std::string>& dirs) override {
    IpcHostInvocation invocation = method_invocation("plugins.load_report");
    invocation.text = dirs.empty() ? std::string{} : dirs.front();
    record(std::move(invocation));
    return {status_for("plugins.load_report")};
  }

  /** @copydoc Host::plugins_unload_all */
  Result<int> plugins_unload_all() override {
    record(method_invocation("plugins.unload_all"));
    return {status_for("plugins.unload_all"), 0};
  }

  /** @copydoc Host::seed_builtin_ops */
  VoidResult seed_builtin_ops() override {
    record(method_invocation("plugins.seed_builtins"));
    return {status_for("plugins.seed_builtins")};
  }

  /** @copydoc Host::ops_sources */
  Result<std::map<std::string, std::string>> ops_sources() const override {
    record(method_invocation("plugins.ops_sources"));
    return {status_for("plugins.ops_sources"), {}};
  }

  /** @copydoc Host::ops_combined_keys */
  Result<std::vector<std::string>> ops_combined_keys() const override {
    record(method_invocation("plugins.ops_combined_keys"));
    return {status_for("plugins.ops_combined_keys"), {}};
  }

  /** @copydoc Host::ops_combined_sources */
  Result<std::map<std::string, std::string>> ops_combined_sources()
      const override {
    record(method_invocation("plugins.ops_combined_sources"));
    return {status_for("plugins.ops_combined_sources"), {}};
  }

  /** @copydoc Host::scheduler_available_types */
  Result<std::vector<std::string>> scheduler_available_types() const override {
    record(method_invocation("scheduler.types"));
    return {status_for("scheduler.types"), {}};
  }

  /** @copydoc Host::scheduler_description */
  Result<std::string> scheduler_description(
      const std::string& type_name) const override {
    IpcHostInvocation invocation = method_invocation("scheduler.description");
    invocation.text = type_name;
    record(std::move(invocation));
    return {status_for("scheduler.description"), {}};
  }

  /** @copydoc Host::scheduler_scan */
  Result<size_t> scheduler_scan(const std::vector<std::string>& dirs) override {
    IpcHostInvocation invocation = method_invocation("scheduler.scan");
    invocation.text = dirs.empty() ? std::string{} : dirs.front();
    record(std::move(invocation));
    return {status_for("scheduler.scan"), 0};
  }

  /** @copydoc Host::scheduler_load */
  VoidResult scheduler_load(const std::string& path) override {
    IpcHostInvocation invocation = method_invocation("scheduler.load");
    invocation.text = path;
    record(std::move(invocation));
    return {status_for("scheduler.load")};
  }

  /** @copydoc Host::scheduler_loaded_plugins */
  Result<std::vector<std::string>> scheduler_loaded_plugins() const override {
    record(method_invocation("scheduler.loaded_plugins"));
    return {status_for("scheduler.loaded_plugins"), {}};
  }

  /** @copydoc Host::configure_scheduler_defaults */
  VoidResult configure_scheduler_defaults(
      const HostSchedulerConfig& config) override {
    IpcHostInvocation invocation =
        method_invocation("scheduler.configure_defaults");
    invocation.text = config.hp_type + "\n" + config.rt_type;
    invocation.first_node.value = static_cast<int>(config.worker_count);
    record(std::move(invocation));
    return {status_for("scheduler.configure_defaults")};
  }

  /** @copydoc Host::scheduler_info */
  Result<SchedulerInfoSnapshot> scheduler_info(
      const GraphSessionId& session, ComputeIntent intent) const override {
    IpcHostInvocation invocation =
        session_invocation("scheduler.info", session);
    invocation.intent = intent;
    record(std::move(invocation));
    return {status_for("scheduler.info"), SchedulerInfoSnapshot{}};
  }

  /** @copydoc Host::replace_scheduler */
  VoidResult replace_scheduler(const GraphSessionId& session,
                               ComputeIntent intent,
                               const std::string& type) override {
    IpcHostInvocation invocation =
        text_invocation("scheduler.replace", session, type);
    invocation.intent = intent;
    record(std::move(invocation));
    return {status_for("scheduler.replace")};
  }

 private:
  /**
   * @brief Builds one method-only invocation.
   * @param method Stable wire method name.
   * @return Invocation with default argument fields.
   * @throws std::bad_alloc if method copying cannot allocate.
   */
  static IpcHostInvocation method_invocation(std::string_view method) {
    IpcHostInvocation invocation;
    invocation.method = method;
    return invocation;
  }

  /**
   * @brief Builds one session-scoped invocation.
   * @param method Stable wire method name.
   * @param session Exact Host session argument.
   * @return Invocation containing the copied session.
   * @throws std::bad_alloc if copied strings cannot allocate.
   */
  static IpcHostInvocation session_invocation(std::string_view method,
                                              const GraphSessionId& session) {
    IpcHostInvocation invocation = method_invocation(method);
    invocation.session = session;
    return invocation;
  }

  /**
   * @brief Builds one session-and-node invocation.
   * @param method Stable wire method name.
   * @param session Exact Host session argument.
   * @param node Exact first node argument.
   * @return Invocation containing the copied arguments.
   * @throws std::bad_alloc if copied strings cannot allocate.
   */
  static IpcHostInvocation node_invocation(std::string_view method,
                                           const GraphSessionId& session,
                                           NodeId node) {
    IpcHostInvocation invocation = session_invocation(method, session);
    invocation.first_node = node;
    return invocation;
  }

  /**
   * @brief Builds one session-and-text invocation.
   * @param method Stable wire method name.
   * @param session Exact Host session argument.
   * @param text Exact path, YAML, precision, or type argument.
   * @return Invocation containing the copied arguments.
   * @throws std::bad_alloc if copied strings cannot allocate.
   */
  static IpcHostInvocation text_invocation(std::string_view method,
                                           const GraphSessionId& session,
                                           const std::string& text) {
    IpcHostInvocation invocation = session_invocation(method, session);
    invocation.text = text;
    return invocation;
  }

  /**
   * @brief Builds one dirty lifecycle invocation.
   * @param method Stable wire method name.
   * @param session Exact Host session argument.
   * @param node Exact source node.
   * @param domain Exact dirty domain.
   * @param roi Exact source ROI, or default rectangle for `dirty.end`.
   * @return Invocation containing the copied arguments.
   * @throws std::bad_alloc if copied strings cannot allocate.
   */
  static IpcHostInvocation dirty_invocation(std::string_view method,
                                            const GraphSessionId& session,
                                            NodeId node, DirtyDomain domain,
                                            const PixelRect& roi) {
    IpcHostInvocation invocation = node_invocation(method, session, node);
    invocation.dirty_domain = domain;
    invocation.roi = roi;
    return invocation;
  }

  /**
   * @brief Commits one invocation and runs the optional synchronization hook.
   * @param invocation Complete owned record.
   * @return Nothing.
   * @throws std::bad_alloc if record or hook copying cannot allocate.
   * @note The hook runs outside `mutex_` so it may safely query the spy. The
   *       caller still holds the router Host mutex, therefore a hook must not
   *       synchronously reenter Host-backed routing or form a cyclic wait.
   */
  void record(IpcHostInvocation invocation) const {
    const std::string method = invocation.method;
    CallHook hook;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      invocations_.push_back(std::move(invocation));
      hook = call_hook_;
    }
    if (hook) {
      hook(method);
    }
  }

  /**
   * @brief Reads one configured status or canonical success.
   * @param method Stable wire method name.
   * @return Copied configured status.
   * @throws std::bad_alloc if diagnostic copying cannot allocate.
   */
  OperationStatus status_for(std::string_view method) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = statuses_.find(std::string(method));
    return found == statuses_.end() ? OperationStatus{} : found->second;
  }

  /**
   * @brief Copies one configured public value under the spy lock.
   * @tparam Value Copyable public value type.
   * @param value Configured member to copy.
   * @return Owned value copy.
   * @throws Whatever copying `Value` throws, including `std::bad_alloc`.
   */
  template <typename Value>
  Value configured_value(const Value& value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value;
  }

  /**
   * @brief Creates a status/value result from one configured member.
   * @tparam Value Copyable public value type.
   * @param method Stable wire method name selecting the status.
   * @param value Configured value member to copy.
   * @return Exact configured result.
   * @throws Whatever status or value copying throws, including
   *         `std::bad_alloc`.
   */
  template <typename Value>
  Result<Value> configured_result(std::string_view method,
                                  const Value& value) const {
    return {status_for(method), configured_value(value)};
  }

  /** @brief Protects configuration, lifecycle, records, and call hook. */
  mutable std::mutex mutex_;

  /** @brief Private Host session published by successful graph load. */
  GraphSessionId host_session_;

  /** @brief Whether the private Host session is currently listed. */
  bool loaded_ = false;

  /** @brief Method-specific exact statuses; absence means success. */
  std::map<std::string, OperationStatus> statuses_;

  /** @brief Ordered copied Host invocation records. */
  mutable std::vector<IpcHostInvocation> invocations_;

  /** @brief Optional post-record synchronization callback. */
  CallHook call_hook_;

  /** @brief Configured node YAML output. */
  std::string node_yaml_;

  /** @brief Configured node-list output. */
  std::vector<NodeId> node_ids_;

  /** @brief Configured ending-node output. */
  std::vector<NodeId> ending_nodes_;

  /** @brief Configured dirty-region output shared by dirty lifecycle routes. */
  DirtyRegionInspectionSnapshot dirty_region_;

  /** @brief Configured forward ROI output. */
  PixelRect forward_roi_;

  /** @brief Configured backward ROI output. */
  PixelRect backward_roi_;

  /** @brief Configured compute timing output. */
  TimingSnapshot timing_;

  /** @brief Configured last-IO diagnostic output. */
  double last_io_time_ = 0.0;

  /** @brief Configured nested last-error diagnostic output. */
  OperationStatus last_error_;

  /** @brief Configured image-mode compute output. */
  ImageBuffer compute_image_;

  /** @brief Configured single-node inspection output. */
  NodeInspectionView inspected_node_;

  /** @brief Configured graph inspection output. */
  GraphInspectionView inspected_graph_;

  /** @brief Configured dependency-tree inspection output. */
  HostDependencyTreeSnapshot dependency_tree_;

  /** @brief Configured traversal order branches. */
  std::map<int, std::vector<NodeId>> traversal_orders_;

  /** @brief Configured traversal detail branches. */
  std::map<int, std::vector<HostTraversalNodeSnapshot>> traversal_details_;

  /** @brief Configured tree roots containing a requested node. */
  std::vector<NodeId> trees_containing_node_;

  /** @brief Configured optional current planning snapshot. */
  std::optional<ComputePlanningInspectionSnapshot> compute_planning_;

  /** @brief Configured recent planning snapshot history. */
  std::vector<ComputePlanningInspectionSnapshot> recent_compute_planning_;
};

}  // namespace ps::testing
