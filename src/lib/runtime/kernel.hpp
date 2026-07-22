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
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "compute/compute_service.hpp"
#include "compute/dirty_control_lane.hpp"
#include "core/cache_metadata_codec.hpp"
#include "core/image_artifact_codec.hpp"
#include "graph/graph_cache_service.hpp"
#include "graph/graph_inspect_service.hpp"
#include "graph/graph_io_service.hpp"
#include "graph/graph_traversal_service.hpp"
#include "graph/roi_propagation_service.hpp"
#include "plugin/plugin_manager.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps {

enum class PolicyClass;
struct HostPolicyConfig;
struct PolicyInfoSnapshot;

namespace testing {

/**
 * @brief Test-only bridge that may inspect Kernel internals.
 *
 * KernelTestAccess is declared here only so Kernel can friend it. The
 * definition lives under tests/support and is not part of the Host API.
 * Frontends use `ps::Host`; the embedded Host adapter alone may translate Host
 * requests to Kernel operations such as inspect_graph, execution_trace,
 * compute, and compute_async. Neither path exposes runtime ownership details.
 *
 * @note This forward declaration intentionally exposes no operations.
 */
class KernelTestAccess;

}  // namespace testing

/**
 * @brief Internal multi-graph coordinator for lifecycle, graph-state commands,
 * compute, execution-route access, policy access, and plugin management.
 *
 * Kernel owns one GraphRuntime per graph name and keeps its internal adapter
 * contract stable while delegating graph IO, traversal, inspection, cache, ROI,
 * dirty control, and compute work to narrower services. The embedded Host
 * adapter maps public Host values to these internal operations; frontend code
 * neither includes nor constructs Kernel types. Visible Graph capture,
 * mutation, commit predicates, and publication run through each runtime's
 * GraphStateExecutor. Operation execution uses request-owned snapshots outside
 * that lane, while a private compute-request lane serializes same-Graph compute
 * and route-binding access. The Host-lifetime ExecutionService receives ready
 * task callbacks but never owns graph-state operation dispatch.
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
   * @brief Creates a Kernel with every configured persistence dependency.
   *
   * @param image_codec Shared codec owner used by graph cache operations.
   * @param metadata_codec Shared metadata owner used by graph cache operations.
   * @param document_reader Shared reader owner used by graph and node loads.
   * @param document_writer Shared writer owner used by graph and node saves.
   * @param execution_service Explicit process CPU execution owner.
   * @throws std::invalid_argument when any required owner is empty.
   * @note The embedded product composition root selects concrete
   *       implementations. Kernel retains them through its cache and graph IO
   *       services for the complete lifetime of every admitted operation.
   */
  Kernel(std::shared_ptr<const ImageArtifactCodec> image_codec,
         std::shared_ptr<const CacheMetadataCodec> metadata_codec,
         std::shared_ptr<const GraphDocumentReader> document_reader,
         std::shared_ptr<const GraphDocumentWriter> document_writer,
         std::shared_ptr<compute::ExecutionService> execution_service);

  /**
   * @brief Drains every owned graph runtime before releasing Kernel services.
   * @throws Nothing.
   * @note Destruction clears `graphs_` explicitly while cache, traversal,
   * diagnostic, IO, and ROI collaborators are still alive. Each
   * `GraphRuntime` first drains its compute-request lane, then drains
   * graph-state and joins both workers, so work items that captured this Kernel
   * cannot observe destroyed collaborators. External callers must stop Kernel
   * API admission before destruction; concurrent unadmitted public calls are
   * unsupported.
   */
  ~Kernel() noexcept;

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
   * ExecutionOptions separates execution and graph-output quiet-mode choices
   * from cache and telemetry options. It is copied into async futures, so the
   * embedded adapter and backend callers should treat it as immutable once the
   * request is submitted.
   *
   * @note parallel selects execution-bound compute where supported. quiet is
   * applied to the graph model around the compute call.
   */
  struct ComputeExecutionOptions {
    /** @brief Whether the request should use queued execution-service work. */
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

    /** @brief Route-service and quiet-mode execution controls. */
    ComputeExecutionOptions execution;

    /** @brief Timing and benchmark recording controls. */
    ComputeTelemetryOptions telemetry;

    /** @brief Optional compute intent; nullopt selects legacy HP behavior. */
    std::optional<ComputeIntent> intent;

    /** @brief Optional HP-space dirty ROI for dirty HP or RT updates. */
    std::optional<PixelRect> dirty_roi;

    /**
     * @brief Optional private current-request cancellation authority.
     * @note Internal backend tests and future private launch owners may retain
     * this source while compute is active. Embedded Host conversion leaves it
     * null, and the field is absent from installed Host, CLI, and IPC values.
     */
    std::shared_ptr<compute::ComputeRequestCancellationSource>
        cancellation_source;

    /**
     * @brief Product-assigned canonical key/generation before staging.
     * @note Embedded Host conversion leaves this empty. Kernel assigns it at
     * latest-wins publication and ComputeService copies it into every child
     * Run descriptor. It is private and absent from Host, CLI, and IPC v1.
     */
    std::optional<compute::SupersessionIdentity> supersession;
  };

  /**
   * @brief Creates and transactionally loads one internal graph runtime.
   *
   * @param name Unique graph/session name.
   * @param root_dir Root directory that owns the session folder.
   * @param yaml_path Omitted source selector or explicit YAML copied into the
   *        session before load.
   * @param config_path Optional config file copied into the session.
   * @param cache_root_dir Optional external cache-root directory.
   * @return Loaded graph name, or nullopt only when the name is already
   *         published or loses the final map-insertion race.
   * @throws std::bad_alloc if path, runtime, route, graph, or diagnostic
   *         allocation exhausts memory.
   * @throws GraphError with `GraphErrc::Io` for explicit-source or session
   *         filesystem failure;
   *         `GraphErrc::InvalidYaml` for syntax/schema rejection;
   *         `GraphErrc::MissingDependency` or `GraphErrc::Cycle` for topology
   *         rejection; and `GraphErrc::Unknown` for unexpected
   *         document-ingestion failures.
   * @throws std::exception for execution-service/runtime startup failures not
   *         classified by Kernel; InteractionService normalizes them before
   *         the embedded Host boundary.
   * @note The first graph load configures the Kernel-lifetime ExecutionService
   *       even if later document ingestion fails; its fixed workers are
   *       process infrastructure and retain no per-Graph reservation. The
   *       unpublished runtime and session publication still roll back
   *       transactionally. An empty yaml_path loads existing
   *       `<root_dir>/<name>/content.yaml` or intentionally publishes an empty
   *       graph; a nonempty source is explicit and never falls back. Complete
   *       document validation precedes map insertion. The return label is
   *       allocated before the runtime enters the owned graph map, and
   *       returning it after insertion uses only noexcept moves.
   */
  std::optional<std::string> load_graph(const std::string& name,
                                        const std::string& root_dir,
                                        const std::string& yaml_path,
                                        const std::string& config_path = "",
                                        const std::string& cache_root_dir = "");

  /**
   * @brief Stops one loaded graph's compute-request admission before Host
   * drain.
   *
   * @param name Graph session name whose request lane rejects new submissions.
   * @return true when a runtime exists and its request lane is draining/closed;
   *         false when the session name is unknown.
   * @throws std::logic_error if invoked from the target lane worker.
   * @throws std::overflow_error if the close-generation counter is exhausted.
   * @throws std::system_error if executor lifecycle synchronization fails.
   * @note This is the non-destructive first phase of embedded Host close. It
   *       preserves the graph map entry, route bindings, model, accepted
   *       request-lane work, graph-state admission, and LastError state while
   *       waking producers blocked by the full request FIFO. Graph-state stays
   *       open so accepted compute can commit. `close_graph()` performs the
   *       later request-drain and graph-state drain phase
   * after Host-level admitted callers and async publication have settled.
   */
  bool stop_graph_admission(const std::string& name);

  /**
   * @brief Stops and removes a loaded graph runtime.
   *
   * @param name Graph session name to close.
   * @return true when a runtime existed and was removed; false when the session
   *         name is unknown.
   * @throws Any exception propagated while stopping the runtime; no exception
   *         is thrown when the graph name is unknown.
   * @note Close first drains and joins the compute-request lane while
   *       graph-state remains available for accepted commits. It then stops,
   *       drains, and joins graph-state before runtime destruction. Embedded
   *       Host first rejects new calls and drains pre-marker synchronous
   *       admissions, calls `stop_graph_admission()`, then waits for async
   *       scheduling/status publication before invoking this method. The
   *       graph map entry and its mutex-protected LastError snapshot are erased
   *       only after stop succeeds. If stop throws, graph-state and
   *       compute-request workers are recreated in that order before rethrow so
   *       the retained session can be inspected, computed, or closed again;
   *       replacement-worker failure is surfaced.
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
   * best-effort last_error() state. The private compute-request lane retains
   * the call; graph-state is used only for runtime start/snapshot capture and
   * final revision-validated publication.
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
   *         fails while scheduling compute-request work.
   * @throws std::runtime_error if compute-request admission has stopped.
   * @throws std::system_error if request-lane or graph-state synchronization
   *         fails.
   * @note The future owns the request copy, but benchmark_events remains
   * caller-owned and must outlive future completion. Calling get() on the
   * returned future may rethrow std::bad_alloc from compute execution or
   * failure-diagnostic allocation inside the async work item. Recoverable
   * failures are captured in AsyncComputeResult by that same work item. Runtime
   * start and snapshot capture occur in a nested graph-state work item rather
   * than during request-lane admission.
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
  std::optional<PixelRect> project_roi_forward(const std::string& name,
                                               int start_node_id,
                                               const PixelRect& start_roi,
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
  std::optional<PixelRect> project_roi_backward(const std::string& name,
                                                int target_node_id,
                                                const PixelRect& target_roi,
                                                int source_node_id);

  /**
   * @brief Reloads an existing graph session from a document through
   * graph-state serialization.
   *
   * @param name Graph session name to reload.
   * @param document_path Source document path.
   * @return true when reload succeeds; false when the graph is missing or the
   *         reload fails with a handled document error.
   * @throws std::bad_alloc if reload execution or handled-failure LastError
   *         construction exhausts memory.
   * @note Missing graph sessions preserve the legacy quiet false result without
   *       updating LastError. For existing sessions, an empty path records
   *       `GraphErrc::InvalidParameter`; IO, syntax/schema, topology, and
   *       unexpected failures record their exact stable categories. Embedded
   *       Host retains lifecycle admission across session resolution, this
   *       graph-state submission, and LastError translation; any other caller
   *       that can race close must provide equivalent runtime-lifetime
   *       admission. Any handled failure leaves nodes, topology adjacency/
   *       generation, runtime graph state, and session identity unchanged;
   *       `std::bad_alloc` propagates with the same preservation guarantee.
   */
  bool reload_graph_document(const std::string& name,
                             const std::string& document_path);
  /**
   * @brief Saves one required graph session through graph-state serialization.
   *
   * @param name Graph session name to save.
   * @param document_path Destination document path.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::NotFound` when the session is absent,
   *         or `GraphErrc::Io` when recoverable document emission or
   *         destination preparation/open/write/flush/close fails.
   * @throws std::bad_alloc if graph-state submission, document serialization,
   *         path handling, or diagnostic construction exhausts memory.
   * @throws std::exception for other graph-state submission or future failures.
   * @note The embedded Host holds a session admission across this call so
   *       concurrent close cannot invalidate the resolved runtime. The save
   *       itself executes in the session's GraphStateExecutor. Save-specific
   *       stream, serialization, and emitter exceptions are normalized to Io
   *       at this boundary. All outcomes preserve graph topology, runtime
   *       state, and session ownership. The direct destination write is not an
   *       atomic replacement: failure before open preserves existing bytes,
   *       while a post-open failure may leave created, truncated, or partial
   *       output.
   */
  void save_graph_document(const std::string& name,
                           const std::string& document_path);

  /**
   * @brief Invalidates the Graph revision and removes its disk cache.
   * @param name Graph session whose cache root should be cleared.
   * @return True on success; false when the graph is missing or an ordinary
   *         cache/executor failure is handled by the quiet facade.
   * @throws std::bad_alloc from revision preparation, submission, or cache
   *         clearing.
   * @note Revision overflow fails before filesystem mutation. After successful
   *       preparation, the successor is published before removal begins and
   *       is never rolled back if removal/recreation later fails partially.
   */
  bool clear_drive_cache(const std::string& name);

  /**
   * @brief Invalidates the Graph revision and clears formal HP memory caches.
   * @param name Graph session whose node caches should be cleared.
   * @return True on success; false for a missing graph or handled ordinary
   *         cache/executor failure.
   * @throws std::bad_alloc from preparation, submission, or cache traversal.
   * @note Revision overflow occurs before node mutation. Once the successor is
   *       published, a later partial clear failure never restores the old
   *       revision, preventing captured Runs from publishing stale output.
   */
  bool clear_memory_cache(const std::string& name);

  /**
   * @brief Invalidates the Graph revision and clears disk plus memory caches.
   * @param name Graph session whose cache authority should be cleared.
   * @return True on success; false for a missing graph or handled ordinary
   *         cache/executor failure.
   * @throws std::bad_alloc from preparation, submission, or either clear phase.
   * @note The successor revision is published before the disk phase. It stays
   *       advanced if disk or memory clearing later fails after partial work.
   */
  bool clear_cache(const std::string& name);
  bool cache_all_nodes(const std::string& name,
                       const std::string& cache_precision);

  /**
   * @brief Invalidates the Graph revision and frees non-ending HP caches.
   * @param name Graph session whose transient caches should be released.
   * @return True on success; false for a missing graph or handled ordinary
   *         traversal/cache/executor failure.
   * @throws std::bad_alloc from preparation, submission, or traversal.
   * @note Revision overflow precedes mutation. A published successor is never
   *       rolled back after any partially successful node-cache release.
   */
  bool free_transient_memory(const std::string& name);
  bool synchronize_disk_cache(const std::string& name,
                              const std::string& cache_precision);
  bool clear_graph(const std::string& name);

  /**
   * @brief Clears disk cache with revision invalidation and removal counts.
   * @param name Graph session whose disk cache should be cleared.
   * @return Removal statistics, or nullopt for a missing graph or handled
   *         ordinary failure.
   * @throws std::bad_alloc from preparation, submission, or cache clearing.
   * @note Uses the same prepare-publish-clear ordering and no-rollback rule as
   *       clear_drive_cache().
   */
  std::optional<GraphModel::DriveClearResult> clear_drive_cache_stats(
      const std::string& name);

  /**
   * @brief Clears formal HP memory caches with revision invalidation and count.
   * @param name Graph session whose memory cache should be cleared.
   * @return Clear statistics, or nullopt for a missing graph or handled
   *         ordinary failure.
   * @throws std::bad_alloc from preparation, submission, or cache traversal.
   * @note Uses the same prepare-publish-clear ordering and no-rollback rule as
   *       clear_memory_cache().
   */
  std::optional<GraphModel::MemoryClearResult> clear_memory_cache_stats(
      const std::string& name);
  std::optional<GraphModel::CacheSaveResult> cache_all_nodes_stats(
      const std::string& name, const std::string& cache_precision);
  /**
   * @brief Frees transient HP caches with revision invalidation and count.
   * @param name Graph session whose non-ending caches should be released.
   * @return Clear statistics, or nullopt for a missing graph or handled
   *         ordinary failure.
   * @throws std::bad_alloc from preparation, submission, or traversal.
   * @note Uses the same prepare-publish-clear ordering and no-rollback rule as
   *       free_transient_memory().
   */
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
   * @brief Reads one bounded non-destructive execution-trace page.
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
  std::optional<GraphRuntime::ExecutionEventPage> execution_trace(
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

  /**
   * @brief Begins one dirty-source lifecycle and returns its updated snapshot.
   * @param name Loaded graph/session name.
   * @param node_id Source node entering the updating state.
   * @param domain HP or RT dirty domain.
   * @param source_roi Positive-area kernel-native source ROI.
   * @return Updated snapshot, or nullopt on missing graph/handled failure.
   * @throws std::bad_alloc when graph-state submission, planning, snapshot
   *         copying, or LastError construction exhausts memory.
   * @note Other failures are recorded in LastError. The convenience facade
   *       discards control hints returned by begin_dirty_source_control().
   */
  std::optional<compute::DirtyRegionSnapshot> begin_dirty_source(
      const std::string& name, int node_id, compute::DirtyDomain domain,
      const PixelRect& source_roi);

  /**
   * @brief Begins one dirty-source lifecycle and preserves dispatch hints.
   * @param name Loaded graph/session name.
   * @param node_id Source node entering the updating state.
   * @param domain HP or RT dirty domain.
   * @param source_roi Positive-area kernel-native source ROI.
   * @return Control result, or nullopt on missing graph/handled failure.
   * @throws std::bad_alloc when graph-state submission, planning, snapshot
   *         copying, or LastError construction exhausts memory.
   * @note The complete transition runs as one serialized graph-state work
   *       item; it does not submit physical execution work itself.
   */
  std::optional<compute::DirtyControlLaneResult> begin_dirty_source_control(
      const std::string& name, int node_id, compute::DirtyDomain domain,
      const PixelRect& source_roi);

  /**
   * @brief Appends one dirty-source ROI and returns the updated snapshot.
   * @param name Loaded graph/session name.
   * @param node_id Source node receiving the incremental ROI.
   * @param domain HP or RT dirty domain.
   * @param source_roi Positive-area kernel-native source ROI.
   * @return Updated snapshot, or nullopt on missing graph/handled failure.
   * @throws std::bad_alloc when graph-state submission, planning, snapshot
   *         copying, or LastError construction exhausts memory.
   * @note Other failures are recorded in LastError. The convenience facade
   *       discards control hints returned by update_dirty_source_control().
   */
  std::optional<compute::DirtyRegionSnapshot> update_dirty_source(
      const std::string& name, int node_id, compute::DirtyDomain domain,
      const PixelRect& source_roi);

  /**
   * @brief Appends one dirty-source ROI and preserves dispatch hints.
   * @param name Loaded graph/session name.
   * @param node_id Source node receiving the incremental ROI.
   * @param domain HP or RT dirty domain.
   * @param source_roi Positive-area kernel-native source ROI.
   * @return Control result, or nullopt on missing graph/handled failure.
   * @throws std::bad_alloc when graph-state submission, planning, snapshot
   *         copying, or LastError construction exhausts memory.
   * @note The complete transition runs as one serialized graph-state work
   *       item and keeps geometry as PixelRect.
   */
  std::optional<compute::DirtyControlLaneResult> update_dirty_source_control(
      const std::string& name, int node_id, compute::DirtyDomain domain,
      const PixelRect& source_roi);

  /**
   * @brief Ends one dirty-source lifecycle and returns the updated snapshot.
   * @param name Loaded graph/session name.
   * @param node_id Source node leaving the updating state.
   * @param domain HP or RT dirty domain.
   * @return Updated snapshot, or nullopt on missing graph/handled failure.
   * @throws std::bad_alloc when graph-state submission, planning, snapshot
   *         copying, or LastError construction exhausts memory.
   * @note No new ROI is appended. The convenience facade discards control
   *       hints returned by end_dirty_source_control().
   */
  std::optional<compute::DirtyRegionSnapshot> end_dirty_source(
      const std::string& name, int node_id, compute::DirtyDomain domain);

  /**
   * @brief Ends one dirty-source lifecycle and preserves cutoff hints.
   * @param name Loaded graph/session name.
   * @param node_id Source node leaving the updating state.
   * @param domain HP or RT dirty domain.
   * @return Control result, or nullopt on missing graph/handled failure.
   * @throws std::bad_alloc when graph-state submission, planning, snapshot
   *         copying, or LastError construction exhausts memory.
   * @note cutoff_after_downstream is derived only after serialized snapshot
   *       rebuilding; this facade does not own an execution queue.
   */
  std::optional<compute::DirtyControlLaneResult> end_dirty_source_control(
      const std::string& name, int node_id, compute::DirtyDomain domain);
  /**
   * @brief Computes a node and returns its output image from a request object.
   *
   * @param request Graph name, target node, cache, execution, telemetry, and
   * optional intent/dirty ROI controls.
   * @return Cloned output descriptor, or nullopt when graph lookup, compute, or
   * image cloning fails.
   * @throws std::bad_alloc if compute/image execution or handled-failure
   *         LastError construction exhausts memory.
   * @note The image is cloned out of graph-owned storage before returning.
   *       Other compute and image-cloning exceptions preserve the historical
   *       nullopt preview/save contract.
   */
  std::optional<ImageBuffer> compute_and_get_image(
      const ComputeRequest& request);

  std::optional<std::vector<int>> list_node_ids(const std::string& name);
  /**
   * @brief Serializes one required node's persistent definition as text.
   *
   * @param name Loaded graph session name.
   * @param node_id Required node identifier.
   * @return Serialized node document, or nullopt when the graph-state facade
   *         reports a recoverable missing graph/node failure.
   * @throws std::bad_alloc if graph-state submission, detached definition
   *         capture, document conversion, or result storage exhausts memory.
   * @note Capture runs under GraphStateExecutor serialization and excludes
   *       runtime parameters, computed outputs, revisions, ROIs, and LUT state.
   *       Conversion proceeds through the injected document writer used by
   *       complete graph operations.
   */
  std::optional<std::string> get_node_document(const std::string& name,
                                               int node_id);
  /**
   * @brief Replaces one required node from document text under graph-state
   *        serialization.
   *
   * @param name Required graph session name.
   * @param node_id Required existing node id whose identity is preserved.
   * @param document_text Candidate replacement node document.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::NotFound` when the session or node is
   *         absent, or `GraphErrc::InvalidYaml` when parsing or complete
   *         candidate-topology validation fails.
   * @throws std::bad_alloc if parsing, validation, graph-state submission, or
   *         replacement exhausts memory.
   * @throws std::exception for other graph-state executor failures.
   * @note Required-node lookup, injected reader conversion, forced id
   *       assignment, in-memory materialization, validation, and replacement
   *       execute in one GraphStateExecutor work item. Embedded Host retains a
   *       session admission across the call so concurrent close cannot erase
   *       the runtime.
   */
  void set_node_document(const std::string& name, int node_id,
                         const std::string& document_text);

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

  /**
   * @brief Copies canonical policy types visible to this process domain.
   * @return Lexically sorted built-in and DSO policy type names.
   * @throws std::bad_alloc or std::system_error when observation storage or
   *         synchronization fails.
   * @note The copied values contain no registry, callback, context, or DSO
   *       ownership capability.
   */
  std::vector<std::string> policy_available_types() const;

  /**
   * @brief Copies the description for one canonical policy type.
   * @param type_name Canonical registered policy type name.
   * @return Bounded Host-owned description text.
   * @throws GraphError with `GraphErrc::NotFound` when the type is absent.
   * @throws std::bad_alloc when copied storage exhausts memory.
   * @note The returned text remains valid after later registry mutation.
   */
  std::string policy_description(const std::string& type_name) const;

  /**
   * @brief Scans caller-ordered directories for policy ABI-v1 DSOs.
   * @param directories Directory strings processed in caller order.
   * @return Number of complete DSOs atomically published by this scan.
   * @throws GraphError using the policy loader's exact recoverable category.
   * @throws std::bad_alloc for Host or synchronous plugin allocation failure.
   * @note Each DSO is one all-or-nothing registry transaction; successful
   *       earlier DSOs remain visible if a later candidate fails.
   */
  std::size_t policy_scan(const std::vector<std::string>& directories);

  /**
   * @brief Loads one policy ABI-v1 DSO as a registry transaction.
   * @param path Nonempty dynamic-library path.
   * @return Nothing after complete publication.
   * @throws GraphError using the policy loader's exact error mapping.
   * @throws std::bad_alloc for Host or synchronous plugin allocation failure.
   * @note No policy type is visible until every exported row validates.
   */
  void policy_load(const std::string& path);

  /**
   * @brief Copies currently visible policy DSO labels.
   * @return Globally nondecreasing Host-owned path labels.
   * @throws std::bad_alloc or std::system_error while copying observation
   *         state.
   * @note Labels are diagnostic values, not native handles or leases.
   */
  std::vector<std::string> policy_loaded_plugins() const;

  /**
   * @brief Atomically replaces both process policy-class bindings.
   * @param config Canonical Interactive and Throughput defaults.
   * @return Nothing after both new generations are published together.
   * @throws GraphError for invalid, unsupported, callback, or generation
   *         failure.
   * @throws std::bad_alloc when candidate preparation exhausts memory.
   * @note Failure preserves both current bindings and their generations.
   */
  void configure_policy_defaults(const HostPolicyConfig& config);

  /**
   * @brief Copies one process policy binding and its immutable first fault.
   * @param policy_class Interactive or Throughput.
   * @return Complete Host-owned binding snapshot.
   * @throws GraphError with `InvalidParameter` for an invalid enum value.
   * @throws std::bad_alloc when copied observation storage exhausts memory.
   * @note Read-only observation is permitted from a policy callback.
   */
  PolicyInfoSnapshot policy_info(PolicyClass policy_class) const;

  /**
   * @brief Replaces exactly one process policy-class binding.
   * @param policy_class Interactive or Throughput.
   * @param type Canonical type supporting the requested class.
   * @return Nothing after the new nonzero generation is published.
   * @throws GraphError for invalid, unsupported, callback, or generation
   *         failure.
   * @throws std::bad_alloc when candidate preparation exhausts memory.
   * @note Same-name replacement still advances generation and retires the old
   *       context exactly once.
   */
  void replace_policy(PolicyClass policy_class, const std::string& type);

  /**
   * @brief Private execution defaults captured for future Graph loads.
   *
   * @throws std::bad_alloc If constructing or copying route storage
   * exhausts memory.
   * @note Selecting built-in CPU may resolve and start the fixed service pool
   * while defaults are stored. Pool workers are uncharged infrastructure.
   * Route vocabulary is validated before publication, and existing runtimes
   * retain their installed bindings.
   */
  struct ExecutionConfig {
    /** @brief Private route for global high-precision compute. */
    std::string hp_type = "cpu";

    /** @brief Private route for real-time dirty-region updates. */
    std::string rt_type = "cpu";

    /**
     * @brief Process execution worker request shared by both intent routes.
     * @note Zero selects bounded automatic resolution; positive values must be
     * no greater than `kExecutionWorkerRequestMax` at public boundaries.
     */
    unsigned int worker_count = 0;
  };

  /**
   * @brief Copies the complete private execution-route vocabulary.
   * @return Exactly `cpu`, `gpu_pipeline`, and `serial_debug` in lexical
   * order.
   * @throws std::bad_alloc when result storage exhausts memory.
   * @note Routes are fixed process implementation values, not plugins.
   */
  std::vector<std::string> execution_available_types() const;

  /**
   * @brief Copies the description for one private execution route.
   * @param type_name Exact route name.
   * @return Host-owned display text.
   * @throws GraphError with `GraphErrc::NotFound` when the route is unknown.
   * @throws std::bad_alloc when result storage exhausts memory.
   * @note The removed scheduler names and `heterogeneous` alias are rejected.
   */
  std::string execution_description(const std::string& type_name) const;

  /**
   * @brief Stores future Graph defaults and freezes CPU workers when selected.
   * @param config Type names and worker request copied into Kernel ownership.
   * @return Nothing.
   * @throws std::invalid_argument If a positive worker request conflicts with
   * the already fixed injected service count.
   * @throws std::invalid_argument If the resolved fixed worker count exceeds
   * the composition-root CPU limit.
   * @throws std::bad_alloc or std::system_error If route copying or
   * first fixed-pool construction fails.
   * @note A configuration selecting built-in CPU freezes and starts the one
   * injected service pool owned for this Kernel, but does not mint a pool
   * ledger reservation. All supported routes share that fixed worker domain.
   * Later zero/equal requests preserve the pool and a conflicting positive
   * request is rejected. Graph load and replacement never resize a configured
   * pool; existing Graph bindings remain unchanged.
   */
  void set_execution_config(const ExecutionConfig& config);

  /**
   * @brief Returns current future-Graph execution defaults.
   * @return Borrowed immutable configuration valid until the next setter call
   * or Kernel destruction.
   * @throws Nothing.
   * @note The snapshot may name a type that later becomes unavailable; Graph
   * load always performs fresh planning before pair admission. Once the CPU
   * service is configured, worker_count contains its resolved fixed count.
   */
  const ExecutionConfig& get_execution_config() const;

  /**
   * @brief Replaces one session execution route through an ownerless
   * transaction.
   * @param name Graph session name.
   * @param intent Compute intent whose route is replaced.
   * @param type Exact private execution route name.
   * @return True after publication; false for a missing Graph, unknown route,
   * or handled request-lane lifecycle failure.
   * @throws std::bad_alloc if copied route or request-lane storage exhausts
   * memory.
   * @note Validation precedes a nonzero generation update serialized with
   * same-Graph compute requests. The transaction constructs no plugin, worker
   * owner, route adapter, or resource reservation.
   */
  bool replace_execution(const std::string& name, ComputeIntent intent,
                         const std::string& type);

  /**
   * @brief Copies one coherent execution-route information snapshot.
   * @param name Graph session name.
   * @param intent Compute intent whose route is inspected.
   * @return Owned route name/statistics, or nullopt when unavailable.
   * @throws std::bad_alloc if request-lane submission or copied text allocation
   *         fails.
   * @note Inspection uses the same compute-request serialization boundary as
   *       compute and replacement; no executor pointer escapes the callback.
   */
  std::optional<std::pair<std::string, std::string>> get_execution_info(
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
   * @note Used by const inspection APIs that need copied runtime values.
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
   * physical execution workers.
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
   * GraphError, standard exceptions, and non-standard exceptions otherwise
   * become nullopt with exact or Unknown LastError state.
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
    } catch (...) {
      store_last_error(name, LastError{GraphErrc::Unknown,
                                       std::string(exception_prefix) +
                                           "unknown non-standard exception"});
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
   *         fails while submitting compute-request work.
   * @throws std::runtime_error if the compute-request lane stopped admission.
   * @throws std::system_error if request-lane synchronization fails.
   * @note The returned future owns the request copy. benchmark_events remains
   * caller-owned and must outlive future completion. Future get() may rethrow
   * std::bad_alloc from compute execution or exact diagnostic construction.
   */
  std::optional<std::future<AsyncComputeResult>> compute_async_request(
      ComputeRequest request);

  /**
   * @brief Runs compute and returns an owned target image descriptor.
   *
   * @param request Internal compute request with image-returning arguments.
   * @return Cloned CPU ImageBuffer target image, or nullopt on missing graph,
   * compute failure, or empty output.
   * @throws std::bad_alloc if compute/image execution or handled-failure
   *         LastError construction exhausts memory.
   * @note Missing graphs return nullopt before LastError state is touched.
   * Successful compute paths clear stale LastError state, including the
   * no-image-output case. Other compute/image exceptions become nullopt.
   */
  std::optional<ImageBuffer> compute_and_get_image_request(
      const ComputeRequest& request);

  /**
   * @brief Captures, executes, and revision-commits one Kernel compute request.
   *
   * @param runtime Runtime supplying the visible graph-state lane, staged RT
   * proxy source, event service, and serialized route-binding lifetime.
   * @param request Kernel request already retained by the compute-request lane.
   * @param committed_output Optional destination for an owned copy of the exact
   * staged target output after visible commit succeeds.
   * @return Nothing after visible commit and any requested output copy succeed.
   * @throws GraphError if snapshot capture, ComputeService execution, staged
   * validation, persistence, or exact revision commit fails.
   * @throws std::bad_alloc if snapshot, policy, request, or output state
   * allocation fails.
   * @throws std::exception for execution and operation failures propagated by
   * ComputeService.
   * @note Capture and final publication use GraphStateExecutor; operation work
   * uses only request-owned Graph/proxy snapshots outside that lane. When
   * committed_output is non-null, the copy is taken from that same staged
   * domain before its lifetime ends, so an ordinary graph-state mutation cannot
   * interleave between commit and result capture. The caller must already hold
   * the runtime's compute-request lane so route bindings cannot be inspected
   * or replaced concurrently.
   */
  void execute_staged_compute_request(GraphRuntime& runtime,
                                      const ComputeRequest& request,
                                      NodeOutput* committed_output = nullptr);

  /**
   * @brief Graph-name map owning every runtime and both admitted serial lanes.
   * @note `Kernel::~Kernel()` clears this map explicitly before ordinary member
   * destruction so runtime drainage completes while every borrowed Kernel
   * collaborator remains alive. External lifecycle admission must already have
   * stopped calls that could access this unsynchronized map.
   */
  std::map<std::string, std::unique_ptr<GraphRuntime>> graphs_;

  /**
   * @brief Serializes all reads, writes, and erases of `last_error_`.
   *
   * @note The mutex is independent of per-graph GraphStateExecutor instances
   *       because asynchronous computes for different sessions may publish
   *       diagnostics concurrently. It is never held while graph-state or
   *       execution-service work executes.
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
  /**
   * @brief Cache service retaining the Kernel-injected artifact codecs.
   * @note `Kernel::~Kernel()` drains and destroys every `GraphRuntime` before
   * ordinary member teardown reaches this service. Admitted graph-state work
   * may therefore borrow the service and codecs until its runtime worker is
   * joined.
   */
  GraphCacheService cache_service_;
  /**
   * @brief Format-neutral service retaining the Kernel-injected document IO.
   * @note The service outlives every admitted graph-state work item because
   *       runtime drainage precedes ordinary Kernel member destruction.
   */
  GraphIOService io_service_;
  RoiPropagationService roi_propagation_service_;

  /**
   * @brief Explicitly injected process CPU execution owner.
   *
   * @note Kernel passes a reference to request-local ComputeService instances.
   * The destructor body first drains and clears every Graph runtime, so no
   * admitted work can retain a ComputeService when this owner releases.
   */
  std::shared_ptr<compute::ExecutionService> execution_service_;

  /** @brief Future-Graph private route and process-worker defaults. */
  ExecutionConfig execution_config_;
};

}  // namespace ps
