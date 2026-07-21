#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "compute/task_graph_planning.hpp"
#include "core/ps_types.hpp"         // NOLINT(build/include_subdir)
#include "graph/graph_revision.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"            // NOLINT(build/include_subdir)

namespace ps {

namespace testing {
class GraphModelTestAccess;
}  // namespace testing

class GraphCacheService;
class GraphIOService;
class GraphTraversalService;
class ComputeService;
namespace compute {
class ComputeTaskDispatcher;
class DownsampleExecutor;
class HighPrecisionDirtyExecutor;
class RealTimeDirtyExecutor;
}  // namespace compute

struct NodeTiming {
  int id = -1;
  std::string name;
  double elapsed_ms = 0.0;
  std::string source;
};

struct TimingCollector {
  std::vector<NodeTiming> node_timings;
  double total_ms = 0.0;
};

enum class GraphTopologyEdgeKind {
  ImageInput,
  ParameterInput,
};

struct GraphTopologyEdge {
  int from_node_id = -1;
  int to_node_id = -1;
  GraphTopologyEdgeKind kind = GraphTopologyEdgeKind::ImageInput;
  std::string from_output_name;
  std::string to_input_name;
  size_t input_index = 0;
};

struct GraphTopologyIndex {
  std::unordered_map<int, std::vector<GraphTopologyEdge>> incoming_by_node;
  std::unordered_map<int, std::vector<GraphTopologyEdge>> outgoing_by_node;

  void clear();
  bool empty() const;
};

class GraphModel {
 public:
  using NodeMap = std::unordered_map<int, Node>;

  TimingCollector timing_results;
  fs::path cache_root;
  std::optional<std::string> last_dirty_region_snapshot_debug;
  std::optional<compute::DirtyRegionSnapshot> last_dirty_region_snapshot;
  std::vector<compute::DirtyRegionSnapshot> recent_dirty_region_snapshots;
  uint64_t dirty_generation_counter = 0;
  std::unordered_map<int, uint64_t> dirty_source_hp_commit_generation;
  std::optional<compute::ComputePlan> last_compute_plan;
  std::vector<compute::ComputePlan> recent_compute_plans;
  std::optional<compute::ComputePlanSummary> last_compute_plan_summary;
  std::vector<compute::ComputePlanSummary> recent_compute_plan_summaries;

  struct DriveClearResult {
    uintmax_t removed_entries = 0;
  };
  struct MemoryClearResult {
    int cleared_nodes = 0;
  };
  struct CacheSaveResult {
    int saved_nodes = 0;
  };
  struct DiskSyncResult {
    int saved_nodes = 0;
    int removed_files = 0;
    int removed_dirs = 0;
  };

  /**
   * @brief Status of the most recent disk-cache load attempt.
   *
   * The status is diagnostic state recorded by GraphCacheService. It separates
   * true misses from skipped attempts, successful disk hits, and read/parse
   * failures while preserving the legacy bool-returning cache-load API.
   *
   * @note Status values describe only disk-cache loading. In-memory HP cache
   * reuse can cause a load attempt to be skipped while the legacy wrapper still
   * returns true because the node already has reusable output.
   */
  enum class DiskCacheLoadStatus {
    Skipped,
    Miss,
    Hit,
    Error,
  };

  /**
   * @brief Diagnostic record for one disk-cache load attempt.
   *
   * The record captures the node, cache entry, resolved disk paths, status,
   * optional error code, and human-readable message produced by
   * GraphCacheService. It is intentionally value-type state so tests,
   * frontends, and future event/LastError bridges can inspect the last attempt
   * without owning cached image data.
   *
   * @note `code` is meaningful when `status == DiskCacheLoadStatus::Error`.
   * For misses, hits, and skipped attempts it remains `GraphErrc::Unknown`.
   */
  struct DiskCacheLoadResult {
    /** @brief Node id whose disk-cache entry was evaluated. */
    int node_id = -1;
    /** @brief Cache type from the selected CacheEntry, usually `image`. */
    std::string cache_type;
    /** @brief Cache location from the selected CacheEntry. */
    std::string location;
    /** @brief Resolved image cache path, when an image cache was considered. */
    fs::path cache_file;
    /** @brief Resolved YAML metadata path paired with the image cache path. */
    fs::path metadata_file;
    /** @brief Outcome category for the attempt. */
    DiskCacheLoadStatus status = DiskCacheLoadStatus::Skipped;
    /** @brief Error category for read/parse failures. */
    GraphErrc code = GraphErrc::Unknown;
    /** @brief Reader-facing diagnostic message for the attempt. */
    std::string message;
  };

  struct NodeRuntimeState {
    plugin::ParameterMap& runtime_parameters;
    std::vector<OutputPort>& outputs;
    std::vector<CacheEntry>& caches;
    bool& preserved;
    std::optional<NodeOutput>& cached_output_high_precision;
    int& hp_version;
    std::optional<PixelRect>& hp_roi;
    std::optional<PixelSize>& last_input_size_hp;
    std::optional<DependencyLutCache>& dependency_lut_cache;
    uint64_t& dependency_lut_version;
    uint64_t& parameters_version;
  };

  /**
   * @brief Creates one live Graph with a fresh process identity and revision.
   * @param cache_root_dir Owned disk-cache root; an empty path disables root
   *        creation.
   * @throws std::overflow_error when live Graph identity space is exhausted.
   * @throws std::filesystem::filesystem_error when nonempty root creation
   *         fails.
   * @throws std::bad_alloc when path or filesystem diagnostics allocate.
   * @note Construction mints identity before creating the cache directory. A
   *       failed directory creation consumes that never-reused identity but
   *       publishes no Graph object. The Graph owns all model state while
   *       cache codecs and runtimes remain external collaborators.
   */
  explicit GraphModel(fs::path cache_root_dir = "cache");

  /**
   * @brief Returns the strong process-lifetime identity of this Graph state.
   * @return Identity minted by a live Graph or retained by its compute
   * snapshot.
   * @throws Nothing.
   * @note Session labels and topology generations are not identity substitutes.
   */
  GraphInstanceId instance_id() const noexcept { return instance_id_; }

  /**
   * @brief Returns the authoritative compute-correctness revision.
   * @return Nonzero revision retained by live state or a compute snapshot.
   * @throws Nothing.
   */
  GraphRevision revision() const noexcept { return revision_; }

  /**
   * @brief Reports whether this model was cloned as request-owned compute
   * state.
   * @return True only for clone_for_compute() results and their publication
   *         copies.
   * @throws Nothing.
   */
  bool is_compute_snapshot() const noexcept { return compute_snapshot_; }

  /**
   * @brief Publishes a successor revision prepared before an external mutation.
   * @param prepared Exact value returned by revision().next() before mutation.
   * @return Nothing.
   * @throws Nothing.
   * @note The caller must hold graph-state serialization. For reversible
   *       in-memory replacement, publish after all throwing preparation and
   *       immediately before the no-throw swap. For cache/filesystem clears,
   *       publish after all pure preparation and immediately before the first
   *       possibly partial side effect. Once published, the revision is never
   *       rolled back even if that side effect later throws.
   */
  void publish_prepared_revision(GraphRevision prepared) noexcept {
    revision_ = prepared;
  }

  /**
   * @brief Clones complete request-visible state for isolated compute work.
   *
   * Definition, topology, HP caches, planning/dirty/timing/diagnostic state,
   * task-shape cache, request flags, identity, and revision are copied. Image
   * payload owners retain their existing shared read-only semantics.
   *
   * @return Independently synchronized compute snapshot marked as staged.
   * @throws std::bad_alloc from copied graph or diagnostic storage.
   * @note Callers must invoke this from the graph-state lane or another
   * boundary that proves no writer can mutate the source during the copy.
   */
  std::unique_ptr<GraphModel> clone_for_compute() const;

  /**
   * @brief Swaps a fully prepared compute snapshot into visible runtime state.
   * @param prepared Publication copy whose complete compute state replaces this
   *        model; it receives the displaced state.
   * @return Nothing.
   * @throws Nothing.
   * @note The caller must first prove that both models have equal identity and
   *       revision, that prepared is a compute snapshot, and that publication
   *       executes in the graph-state lane. Identity, revision, cache root, and
   *       live request flags are deliberately not swapped. The two diagnostic
   *       stores exchange under address-ordered no-throw mutexes, and every
   *       other state swap in the publication phase is also no-throw.
   */
  void publish_compute_snapshot(GraphModel& prepared) noexcept;

  void set_quiet(bool q);
  bool is_quiet() const;

  /**
   * @brief Stores the latest disk-cache load diagnostic under its own lock.
   *
   * @param result Value-type diagnostic produced by GraphCacheService.
   * @return Nothing.
   * @throws std::bad_alloc if the by-value diagnostic copy cannot allocate.
   * @note Disk-cache diagnostics are updated from scheduler worker paths, so
   * they use one encapsulated no-throw mutex instead of graph_mutex_ or
   * timing_mutex_. Shared storage is reachable only through the diagnostic
   * store, and replacement is no-throw after argument preparation.
   */
  void record_disk_cache_load_result(DiskCacheLoadResult result);

  /**
   * @brief Returns a locked snapshot of the latest disk-cache load diagnostic.
   *
   * @return Diagnostic snapshot when one has been recorded, otherwise nullopt.
   * @throws std::bad_alloc if copying path/string members allocates.
   * @note Callers receive a value copy so they do not hold the diagnostic
   * store mutex while inspecting frontend or test state. Mutex acquisition is
   * no-throw and never nests another Graph lock.
   */
  std::optional<DiskCacheLoadResult> last_disk_cache_load_result_snapshot()
      const;

  /**
   * @brief Clears the latest disk-cache load diagnostic under its own lock.
   *
   * @return Nothing.
   * @throws Nothing.
   * @note Graph reload and clear paths use this to avoid stale diagnostics
   * leaking across graph lifetimes. The encapsulated store makes reset and
   * mutex acquisition one no-throw operation.
   */
  void clear_disk_cache_load_result() noexcept;

  /**
   * @brief Clears all nodes and resets model-level runtime state.
   *
   * The clear operation removes the visible graph topology, clears cache,
   * timing, dirty, diagnostic, and task-graph state, and advances the topology
   * generation so runtime-owned mirrors such as RealtimeProxyGraph discard any
   * state keyed by reused node ids from the previous graph contents.
   *
   * @return Nothing.
   * @throws std::overflow_error before mutation when GraphRevision or topology
   *         generation has no successor.
   * @note Success advances topology generation and GraphRevision exactly once.
   *       The graph remains usable after clear; later loads or replacements
   *       start from an empty topology generation boundary.
   */
  void clear();

  /**
   * @brief Adds one node through a validated transactional topology
   * replacement.
   * @param node Node definition and initial runtime state to copy.
   * @return Nothing.
   * @throws GraphError for duplicate ids, missing dependencies, or cycles.
   * @throws std::bad_alloc when candidate or topology preparation allocates.
   * @throws std::overflow_error when topology generation or revision exhausts.
   * @note Success advances topology generation and GraphRevision exactly once;
   *       every failure preserves the prior Graph.
   */
  void add_node(const Node& node);

  /**
   * @brief Replaces one existing node as one validated Graph mutation.
   * @param node Replacement node with the same graph-local id.
   * @return Nothing.
   * @throws GraphError for a missing id, missing dependency, or cycle.
   * @throws std::bad_alloc or std::overflow_error before publication.
   * @note Success advances topology generation and GraphRevision exactly once.
   */
  void replace_node(const Node& node);

  /**
   * @brief Removes one node and disconnects every edge that referenced it.
   * @param id Existing graph-local node id.
   * @return Nothing.
   * @throws GraphError when id is absent or candidate topology is invalid.
   * @throws std::bad_alloc or std::overflow_error before publication.
   * @note Success publishes node/topology state and both generations
   * atomically.
   */
  void remove_node(int id);

  /**
   * @brief Rewires one image input through validated candidate topology.
   * @param node_id Destination node id.
   * @param input_index Destination image-input index.
   * @param from_node_id Source node id, or -1 to disconnect.
   * @param from_output_name Source output-port name.
   * @return Nothing.
   * @throws GraphError for missing nodes, invalid index, dependency, or cycle.
   * @throws std::bad_alloc or std::overflow_error before publication.
   * @note Success advances topology generation and GraphRevision exactly once.
   */
  void rewire_image_input(int node_id, size_t input_index, int from_node_id,
                          std::string from_output_name);

  /**
   * @brief Rewires one connected parameter input transactionally.
   * @param node_id Destination node id.
   * @param input_index Destination parameter-input index.
   * @param from_node_id Source node id, or -1 to disconnect.
   * @param from_output_name Source named-output key.
   * @param to_parameter_name Destination parameter name.
   * @return Nothing.
   * @throws GraphError for missing nodes, invalid index, dependency, or cycle.
   * @throws std::bad_alloc or std::overflow_error before publication.
   * @note Success advances topology generation and GraphRevision exactly once.
   */
  void rewire_parameter_input(int node_id, size_t input_index, int from_node_id,
                              std::string from_output_name,
                              std::string to_parameter_name);
  /**
   * @brief Atomically replaces the graph node map after validating topology.
   *
   * @param nodes Replacement nodes keyed by node id.
   * @return Nothing.
   * @throws GraphError with `GraphErrc::MissingDependency` when an input
   *         references an absent node, or `GraphErrc::Cycle` when the
   *         replacement is cyclic.
   * @throws std::bad_alloc if validation, topology construction, or temporary
   *         storage allocation fails.
   * @throws std::overflow_error before publication when GraphRevision or
   *         topology generation has no successor.
   * @note Successful replacement resets graph runtime state and advances
   *       topology generation even when node ids are reused, preventing
   *       runtime-owned mirrors from preserving stale per-node state across
   *       graph reloads. Any validation or preparation failure preserves the
   *       prior node map, topology index, generation, revision, and runtime
   *       state. Success advances both generations exactly once.
   */
  void replace_nodes(NodeMap nodes);
  bool has_node(int id) const;
  bool empty() const;
  size_t node_count() const;
  std::vector<int> node_ids() const;
  const Node* find_node(int id) const;
  const Node& node(int id) const;
  void mutate_node_runtime_state(
      int id, const std::function<void(NodeRuntimeState&)>& mutator);
  void for_each_node(const std::function<void(const Node&)>& visitor) const;
  void validate_topology() const;
  /**
   * @brief Rebuilds adjacency from the current node map as one Graph mutation.
   * @return Nothing.
   * @throws GraphError for missing dependencies or cycles.
   * @throws std::bad_alloc or std::overflow_error before publication.
   * @note This public migration helper advances topology generation and
   *       GraphRevision once. Ordinary compute planning only reads topology.
   */
  void rebuild_topology_index();
  const GraphTopologyIndex& topology() const;
  const std::vector<GraphTopologyEdge>& upstream_edges(int node_id) const;
  const std::vector<GraphTopologyEdge>& downstream_edges(int node_id) const;

  /**
   * @brief Returns the topology generation for task graph cache keys.
   *
   * @return Monotonic generation incremented when graph topology is rebuilt,
   * cleared, or replaced.
   * @throws Nothing.
   * @note Runtime cache, ROI, and dirty state changes do not affect this
   * generation; FullTaskGraph expansion intentionally depends only on topology
   * and task-shape configuration.
   */
  uint64_t topology_generation() const;

  /**
   * @brief Looks up an immutable cached FullTaskGraph by key.
   *
   * @param key Cache key built from topology generation, intent, and task shape
   * config.
   * @return Shared immutable graph when present, otherwise nullptr.
   * @throws Nothing directly.
   * @note The graph owns no runtime dependency state and may be shared by HP
   * and RT sibling planning only when the key intent matches.
   */
  std::shared_ptr<const compute::FullTaskGraph> cached_full_task_graph(
      const std::string& key) const;

  /**
   * @brief Stores an immutable FullTaskGraph cache entry.
   *
   * @param key Cache key built by the compute planning layer.
   * @param graph Immutable full graph to share across request pruning.
   * @throws std::bad_alloc if cache storage grows.
   * @note Inserting a null graph is ignored.
   */
  void remember_full_task_graph(
      const std::string& key,
      std::shared_ptr<const compute::FullTaskGraph> graph);

  /**
   * @brief Clears cached FullTaskGraph expansions for all compute intents.
   *
   * @throws Nothing under current unordered_map clear behavior.
   * @note Force-recache and input-shape-changing paths use this before
   * planning so tiled task ROIs are rebuilt from current graph extents instead
   * of a previous expansion.
   */
  void clear_full_task_graph_cache();

  void set_skip_save_cache(bool v);
  bool skip_save_cache() const;

  std::atomic<double> total_io_time_ms{0.0};

 private:
  friend class GraphCacheService;
  friend class GraphIOService;
  friend class GraphTraversalService;
  friend class ComputeService;
  friend class compute::ComputeTaskDispatcher;
  friend class compute::DownsampleExecutor;
  friend class compute::HighPrecisionDirtyExecutor;
  friend class compute::RealTimeDirtyExecutor;
  friend class testing::GraphModelTestAccess;

  /** @brief Selects the allocation-free constructor used only by snapshots. */
  struct ComputeSnapshotTag {};

  /**
   * @brief Encapsulates the sole mutable disk-cache diagnostic and its mutex.
   *
   * The store serializes record, snapshot, clear, clone, and staged-publication
   * exchange. Its no-throw mutex preserves GraphModel's no-throw publication
   * phase without holding a diagnostic lock across filesystem, cache,
   * scheduler, callback, lane, ledger, or other Graph-lock operations.
   *
   * @note The mutex is non-recursive. Two-store exchange acquires mutexes in
   * address order, so live/staged publication has no reverse lock order. The
   * contained optional is private to prevent future GraphModel code from
   * bypassing this synchronization contract.
   */
  class DiskCacheDiagnosticStore {
   public:
    /**
     * @brief Replaces the current diagnostic with a complete prepared value.
     * @param result Diagnostic whose owned path/string state is moved in.
     * @return Nothing.
     * @throws std::bad_alloc only if preparing local value storage allocates.
     * @note Shared-state replacement occurs under the sole store mutex and is
     * no-throw once that local preparation has completed.
     */
    void record(DiskCacheLoadResult result);

    /**
     * @brief Copies the current diagnostic while holding the store mutex.
     * @return Independent diagnostic value, or nullopt when empty.
     * @throws std::bad_alloc if path/string copying allocates.
     * @note The mutex is released on both success and exception paths.
     */
    std::optional<DiskCacheLoadResult> snapshot() const;

    /**
     * @brief Resets the current diagnostic under the store mutex.
     * @return Nothing.
     * @throws Nothing.
     */
    void clear() noexcept;

    /**
     * @brief Exchanges two live/staged diagnostics under ordered mutexes.
     * @param other Distinct store receiving this store's displaced value.
     * @return Nothing.
     * @throws Nothing.
     * @note Self-exchange is a no-op. Distinct mutexes are acquired in address
     * order and released before GraphModel touches any other state.
     */
    void exchange_with(DiskCacheDiagnosticStore& other) noexcept;

   private:
    /**
     * @brief Acquires the store's no-throw spin mutex.
     * @return Nothing.
     * @throws Nothing.
     * @note Contention yields the worker instead of acquiring another lock or
     * invoking an external callback.
     */
    void lock() const noexcept;

    /**
     * @brief Releases the store's no-throw spin mutex.
     * @return Nothing.
     * @throws Nothing.
     */
    void unlock() const noexcept;

    /** @brief No-throw mutex flag protecting value_. */
    mutable std::atomic_flag mutex_ = ATOMIC_FLAG_INIT;

    /** @brief Latest diagnostic, accessed only while mutex_ is held. */
    std::optional<DiskCacheLoadResult> value_;
  };

  /**
   * @brief Constructs empty staged storage with retained Graph provenance.
   * @param tag Snapshot-only overload selector.
   * @param cache_root_dir Cache root copied from the live Graph.
   * @param instance_id Strong live Graph identity to retain.
   * @param revision Authoritative revision to retain.
   * @throws Nothing except implementation-defined filesystem path move.
   * @note This constructor performs no directory creation and never mints a new
   *       identity.
   */
  GraphModel(ComputeSnapshotTag tag, fs::path cache_root_dir,
             GraphInstanceId instance_id, GraphRevision revision);

  /**
   * @brief Clears compute-derived state after whole-Graph replacement.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller owns graph-state serialization. The implementation performs
   *       only no-throw container clears, diagnostic-store reset, optional
   *       resets, and scalar stores so a prepared structural publication
   *       cannot fail after container swap.
   */
  void reset_runtime_state() noexcept;

  /**
   * @brief Publishes a prepared node map and adjacency with checked
   * generations.
   * @param nodes Validated candidate node map.
   * @param topology Matching candidate adjacency index.
   * @param reset_runtime Whether reload/whole replacement resets runtime state.
   * @return Nothing.
   * @throws std::overflow_error before publication when either generation is
   *         exhausted.
   * @note Candidate containers must already be complete. Container swaps and
   *       scalar publication, including the encapsulated diagnostic reset, are
   *       non-throwing after the checked preparation.
   */
  void publish_structural_replacement(NodeMap nodes,
                                      GraphTopologyIndex topology,
                                      bool reset_runtime);
  Node* find_node_mutable(int id);
  Node& mutable_node(int id);

  NodeMap nodes_;
  GraphTopologyIndex topology_;
  std::mutex graph_mutex_;
  mutable std::mutex timing_mutex_;
  DiskCacheDiagnosticStore disk_cache_diagnostics_;
  uint64_t topology_generation_ = 0;
  std::unordered_map<std::string, std::shared_ptr<const compute::FullTaskGraph>>
      full_task_graph_cache_;
  bool quiet_ = true;
  std::atomic<bool> skip_save_cache_{false};

  /** @brief Strong identity minted only for a live Graph construction. */
  GraphInstanceId instance_id_;

  /** @brief Authoritative revision preserved by compute publication. */
  GraphRevision revision_;

  /** @brief True when this object is request-owned staged compute state. */
  bool compute_snapshot_ = false;
};

/**
 * @brief Resolves the exact effective operation parameters from graph caches.
 *
 * @param node Node whose static and parameter-input values are merged.
 * @param graph Graph providing authoritative cached named outputs.
 * @return Deep-owned canonical parameter map shared by ROI callbacks and
 *         dependency-cache identity.
 * @throws GraphError with MissingDependency when a required parameter input or
 *         named output is unavailable.
 * @throws std::bad_alloc unchanged from copying recursive parameter values.
 * @note The function never trusts graph-node runtime_parameters because compute
 *       may resolve those values only on an execution-local node copy.
 */
plugin::ParameterMap resolve_effective_parameter_snapshot(
    const Node& node, const GraphModel& graph);

/**
 * @brief Resolves cached image-input extents by destination input index.
 * @param node Node whose image-input topology is inspected.
 * @param graph Graph providing authoritative HP cached image descriptors.
 * @return Extent vector with empty entries for missing/unknown inputs.
 * @throws std::bad_alloc when vector storage grows.
 * @note Graph extent resolution may overwrite unknown entries when it has
 *       stronger request-local knowledge.
 */
std::vector<PixelSize> cached_image_input_extents(const Node& node,
                                                  const GraphModel& graph);

}  // namespace ps
