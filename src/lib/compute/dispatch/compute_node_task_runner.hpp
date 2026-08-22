#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "benchmark/common/benchmark_types.hpp"
#include "compute/dirty/node_executor.hpp"
#include "compute/dispatch/task_graph_planning.hpp"
#include "execution/execution_task_runtime.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps {
class GraphCacheService;
class GraphEventService;
}  // namespace ps

namespace ps::compute {

class ComputeRunLease;

/**
 * @brief Borrowed state required by NodeTaskRunner worker closures.
 *
 * The context packages graph services, dense plan indexes, temporary result
 * slots, resolved operations, and timing sinks for one dispatcher execute()
 * call. It has no ownership fields; all referenced objects must outlive the
 * execution tasks that use the runner.
 *
 * @note The struct is copied into NodeTaskRunner by reference. The owning
 * submission plan must wait for runtime completion before destroying any
 * referenced vectors or services.
 */
struct NodeTaskRunnerContext {
  /** @brief Graph being read by worker tasks and later committed by caller. */
  GraphModel& graph;

  /** @brief Cache service used for disk reads before operation execution. */
  GraphCacheService& cache;

  /** @brief Event sink for computed and disk-cache node events. */
  GraphEventService& events;

  /** @brief Execution runtime used for tile trace events. */
  ExecutionTaskRuntime& task_runtime;

  /** @brief Shared timing collector updated under timing_mutex. */
  TimingCollector& timing_results;

  /** @brief Mutex protecting timing_results and optional benchmark_events. */
  std::mutex& timing_mutex;

  /** @brief Dense planned node id order produced by TaskSubmissionPlan. */
  const std::vector<int>& execution_order;

  /** @brief Node id to dense execution-order index lookup. */
  const std::unordered_map<int, int>& id_to_idx;

  /** @brief Per-node temporary outputs published before serialized commit. */
  std::vector<std::optional<NodeOutput>>& temp_results;

  /**
   * @brief Exact implementation snapshots resolved for the planned HP intent.
   * @note Each present value keeps the callback and scheduling metadata
   * selected under the same nonzero implementation identity.
   */
  const std::vector<std::optional<OpImplementation>>& resolved_ops;

  /** @brief Immutable task graph whose PlannedTask entries workers execute. */
  const ComputeTaskGraph& task_graph;

  /** @brief Whether worker tasks must ignore existing HP cache state. */
  bool force_recache = false;

  /** @brief Whether worker tasks record timing and benchmark events. */
  bool enable_timing = false;

  /** @brief Whether worker tasks may read disk cache entries. */
  bool disable_disk_cache = false;

  /** @brief Optional borrowed benchmark event sink. */
  std::vector<BenchmarkEvent>* benchmark_events = nullptr;

  /**
   * @brief Optional borrowed Run lease used only for cooperative observations.
   * @note The plan must not retain a lease strongly because the Run owns the
   * plan. Product dispatch keeps this pointed-to lifecycle lease alive through
   * synchronous execution settlement.
   */
  const ComputeRunLease* run_lease = nullptr;

  /**
   * @brief Frozen node output authorities retained by the ComputePlan.
   * @note Product execution supplies this pointer. A null pointer is malformed
   * preparation and fails closed before any result enters temporary staging.
   */
  const std::vector<PlannedNodeWork>* planned_work = nullptr;
};

/**
 * @brief Executes planned nodes into temporary result slots.
 *
 * NodeTaskRunner performs the worker-thread portion of high-precision
 * dispatch. It resolves already-published upstream outputs, optionally loads
 * disk cache entries, executes uncached operations, records timing/event data,
 * and stores the resulting NodeOutput in TaskSubmissionPlan-owned temp slots.
 * It deliberately avoids mutating GraphModel cache ownership; the result
 * committer performs that serialized side effect after all worker tasks finish.
 *
 * @note The runner borrows plan vectors, services, and graph state. It must not
 * outlive the active dispatcher execute() call that created it.
 */
class NodeTaskRunner {
 public:
  /**
   * @brief Selects which dependency counters one completed task may release.
   *
   * @note Ordinary tasks release only their own edges. Nonfinal tiled tasks
   * retire successfully without releasing edges because their shared Host
   * output is not publishable yet. The unique tiled publisher releases every
   * selected sibling's exact ROI edges after installing the complete Value.
   */
  enum class TaskDependencyRelease {
    /** @brief Release only this task's dependency edges. */
    CurrentTask,
    /** @brief Retire this tile but retain all node dependency edges. */
    DeferTiledNode,
    /** @brief Release every selected tile edge for this completed node. */
    CompleteTiledNode,
  };

  /**
   * @brief Binds one worker runner to a borrowed dispatch context.
   *
   * @param context Borrowed graph, service, timing, plan, and option state.
   * @throws Nothing directly.
   * @note The constructor stores references only. Callers must keep every
   * referenced object alive until runtime completion.
   */
  explicit NodeTaskRunner(NodeTaskRunnerContext context);

  /**
   * @brief Runs the planned node at a dense execution index.
   *
   * @param node_idx Dense index into execution_order_, temp_results_, and
   * resolved_ops_.
   * @return Nothing after the node's request-local result is available.
   * @throws std::bad_alloc when node execution exhausts memory.
   * @throws GraphError without node wrapping when cancellation is observed
   * before node identity lookup, or with compute-stage node context for other
   * OpenCV, standard, and unknown operation failures.
   * @note Cancellation prevents provider entry at the initial boundary. A
   * provider already executing remains non-preemptible except at tiled
   * callbacks and is observed again by its enclosing task route. This method
   * is called from execution worker closures and therefore leaves exception
   * transport to ExecutionTaskRuntime.
   */
  void run_node(int node_idx);

  /**
   * @brief Runs one planned task by task id.
   *
   * @param task_id Dense id into task_graph.tasks.
   * @return Exact dependency-release ownership after task execution.
   * @throws std::bad_alloc when task execution exhausts memory.
   * @throws GraphError without task wrapping when cancellation is observed
   * before task lookup, or with compute-stage node context for other operation
   * failures.
   * @note Tile tasks execute only their PlannedTask::output_roi and observe
   * cancellation before each provider tile. Node and monolithic tasks delegate
   * to run_node(); a monolithic provider already entered is non-preemptible.
   */
  TaskDependencyRelease run_task(int task_id);

  /**
   * @brief Estimates complete Host-owned runner structural storage.
   * @return Checked inline object, vector capacities, and output mutex bytes.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note Borrowed Graph/services/plan vectors and operation-created image
   * payloads are not owned by the runner and are excluded.
   */
  std::uint64_t retained_memory_bytes() const;

 private:
  /**
   * @brief Returns whether disk cache reads are allowed for this dispatch.
   *
   * @return True when neither disk-cache disablement nor force-recache is
   * active.
   * @throws Nothing.
   * @note This request flag gate does not inspect node validity.
   */
  bool allow_disk_cache() const;

  /**
   * @brief Resolves the exact planned output authority for one dense node.
   * @param node_idx Dense index into execution_order_.
   * @return Borrowed callback-free authority frozen during graph planning.
   * @throws GraphError with ComputeError when preparation omitted or corrupted
   * the node's output authority.
   * @note The lookup never consults provider-returned output or live registry
   * metadata and therefore cannot widen the frozen declaration.
   */
  const PlannedOutputAuthority& output_authority(int node_idx) const;

  /**
   * @brief Resolves a complete upstream output for a whole-output dependency.
   *
   * @param up_id Connected upstream node id, or a negative disconnected
   * sentinel.
   * @return Current-request temporary output when present, otherwise complete
   * reusable formal HP output; nullptr for disconnected, missing, absent, or
   * partial persistent output.
   * @throws std::logic_error, std::invalid_argument, std::overflow_error, or
   * std::bad_alloc when committed output validity cannot be checked.
   * @note Dependency release makes a temporary producer result complete before
   * a downstream task reads it. Persistent output is always filtered through
   * ComputeCachePolicy so exact partial Region state cannot reach a whole read.
   */
  const NodeOutput* upstream_output(int up_id) const;

  /**
   * @brief Checks for complete request-local or reusable persistent output.
   *
   * @param node Planned graph node whose formal HP validity is inspected.
   * @param node_idx Dense index of the current-request temporary result slot.
   * @return True when the temporary slot is populated or formal HP output has
   * exact complete Region coverage.
   * @throws std::logic_error, std::invalid_argument, std::overflow_error, or
   * std::bad_alloc when committed output validity cannot be checked.
   * @note A partial formal output deliberately returns false so disk lookup or
   * recomputation can satisfy the planned whole-output request.
   */
  bool has_reusable_memory_or_temp_output(const Node& node, int node_idx) const;

  /**
   * @brief Satisfies one planned whole-output node from valid cache or compute.
   *
   * @param node_idx Dense temporary-result and operation index.
   * @param node_id Graph node id corresponding to node_idx.
   * @return Nothing after a reusable temporary result is available.
   * @throws GraphError, std::bad_alloc, or provider/cache exceptions from disk
   * load, validity checking, parameter resolution, or uncached execution.
   * @note Partial persistent HP output is not a memory hit. The runner may try
   * disk cache and then executes the operation when no current-request or
   * exact complete reusable result exists.
   */
  void compute_node(int node_idx, int node_id);

  /**
   * @brief Computes one tile task into request-local output staging.
   *
   * @param task Immutable tile task with node identity, ROI, and tile size.
   * @return Current-task, deferred-node, or complete-node release ownership.
   * @throws GraphError when task identity/operation/dependencies are invalid.
   * @throws std::logic_error, std::invalid_argument, std::overflow_error,
   * std::bad_alloc, or provider/cache exceptions from validity checking,
   * allocation, disk loading, or tile execution.
   * @note Existing formal HP output suppresses all sibling tile work only when
   * ComputeCachePolicy proves exact complete Region coverage. A partial formal
   * output remains visible state but is recomputed for this Whole plan.
   */
  TaskDependencyRelease compute_tile_task(const PlannedTask& task);

  /**
   * @brief Stops a tile task when the node was satisfied by disk cache.
   *
   * @param target_node Graph node whose disk cache entries may be inspected.
   * @param node_idx Dense planned-node index for temp output and status state.
   * @return True when the caller must skip tile execution.
   * @throws Exceptions from disk-cache diagnostic/output storage.
   * @note The method holds the per-node output mutex while it rechecks
   * node_precomputed_ and, if no staging output exists, attempts a single
   * disk-cache load. `temp_results_` remains empty during ordinary tiled
   * binding writes and receives only the final sealed output.
   */
  bool try_satisfy_tile_from_disk_cache(const Node& target_node, int node_idx);

  /**
   * @brief Ensures one Host output binding exists for the planned tiled node.
   *
   * @param node_idx Dense planned-node index for output staging state.
   * @param target_node Node whose fallback dimensions may seed allocation.
   * @param image_inputs Ready image inputs used to infer channels and type.
   * @return Borrowed open binding for checked tile grants, or nullptr when the
   * node became precomputed before allocation.
   * @throws std::invalid_argument or std::overflow_error for invalid canonical
   * input facts or output-plan arithmetic.
   * @throws std::bad_alloc when creating plan, aligned Value allocation, or
   * binding state fails.
   * @note Callers must treat nullptr as a successful skip, not as a compute
   * error, because another tile task has already provided whole-node output.
   * The returned pointer remains stable until the last successful sibling
   * moves and seals the binding.
   */
  HostOutputBinding* ensure_tile_output_binding(
      int node_idx, const Node& target_node,
      const std::vector<const NodeOutput*>& image_inputs);

  /**
   * @brief Finalizes output after the last tile task completes.
   * @param node_idx Dense planned-node index for output staging state.
   * @param target_node Graph node whose event and metadata are finalized.
   * @param image_inputs Complete inputs contributing spatial metadata.
   * @param current_event Mutable benchmark event for the publishing task.
   * @return True only for the unique task that installed the complete output.
   * @throws GraphError when completion has no Host output binding.
   * @throws Host binding, metadata, event, or allocation exceptions unchanged.
   * @note The final task seals, finalizes metadata, and installs temp_results_
   * before returning complete-node ownership of all exact tile-edge release.
   */
  bool finalize_tiled_node_if_complete(
      int node_idx, const Node& target_node,
      const std::vector<const NodeOutput*>& image_inputs,
      BenchmarkEvent& current_event);

  /**
   * @brief Attempts to satisfy a node from disk cache into its temp slot.
   * @param target_node Planned node whose configured artifact may be read.
   * @param node_idx Dense planned-node index resolving the frozen schema.
   * @return Nothing after a compatible hit is staged or an incompatible/missing
   * artifact is left for provider recomputation.
   * @throws GraphError, allocation, diagnostic, or output-validation
   * exceptions unchanged.
   * @note The complete frozen image/parameter/generic shape is passed to the
   * cache service. Generic output misses before filesystem inspection; sibling
   * presence misses before either codec; decoded parameter-key mismatch misses
   * before returning Hit. None can become a partial staged hit.
   */
  void try_load_disk_cache(const Node& target_node, int node_idx);

  /**
   * @brief Builds request-local runtime parameters from ready upstream values.
   * @param target_node Node whose static parameters and bindings are read.
   * @return Owned ParameterMap with parameter-input values overlaid.
   * @throws GraphError when a connected parameter output is unavailable.
   * @throws std::bad_alloc from recursive value copying.
   * @note The committed node parameter state is never mutated.
   */
  plugin::ParameterMap resolve_runtime_parameters(
      const Node& target_node) const;

  /**
   * @brief Resolves image bindings without compressing destination slots.
   * @param target_node Node whose image-input declarations are read.
   * @return Borrowed output pointers aligned with node.image_inputs; a
   * disconnected slot remains nullptr.
   * @throws GraphError when a connected image output is unavailable.
   * @throws std::bad_alloc when vector allocation fails.
   * @note Returned pointers borrow request-local or committed output storage.
   */
  std::vector<const NodeOutput*> resolve_image_inputs(
      const Node& target_node) const;

  /**
   * @brief Builds tile execution configuration for tile-capable operations.
   * @param target_node Node whose metadata and trace identity are captured.
   * @param implementation Exact implementation whose callback and scheduling
   * metadata were selected together.
   * @return Default configuration for monolithic operations, otherwise
   * exact-metadata-derived tile sizing plus a per-tile observation callback.
   * @throws std::bad_alloc if copied callback or metadata storage allocates.
   * @throws GraphError or execution trace exceptions when the installed
   * callback later observes cancellation or logs tile execution.
   * @note The callback observes cancellation before logging and before the
   * provider enters each tile; it borrows this runner through synchronous
   * dispatcher settlement.
   */
  TiledExecutionConfig tiled_config_for(
      const Node& target_node, const OpImplementation& implementation) const;

  /** @brief Creates a benchmark event initialized to execution start. */
  BenchmarkEvent start_event(const Node& target_node) const;

  /** @brief Executes an operation when caches did not satisfy the node. */
  void compute_uncached_node(const Node& target_node, int node_idx);

  /** @brief Records timing and event state for a disk-cache hit. */
  void record_disk_cache_hit(const Node& target_node);

  /** @brief Records timing and event state for a computed output. */
  double record_computed_output(const Node& target_node,
                                BenchmarkEvent& current_event);

  /** @brief Borrowed graph read by workers and committed after dispatch. */
  GraphModel& graph_;

  /** @brief Borrowed cache service used for optional disk cache reads. */
  GraphCacheService& cache_;

  /** @brief Borrowed event sink for node execution status. */
  GraphEventService& events_;

  /** @brief Borrowed execution runtime used for tile trace events. */
  ExecutionTaskRuntime& task_runtime_;

  /** @brief Shared timing collector updated only while timing_mutex_ is held.
   */
  TimingCollector& timing_results_;

  /** @brief Mutex guarding timing_results_ and benchmark_events_. */
  std::mutex& timing_mutex_;

  /** @brief Dense planned node id order shared with TaskSubmissionPlan. */
  const std::vector<int>& execution_order_;

  /** @brief Node id to dense execution index lookup for upstream resolution. */
  const std::unordered_map<int, int>& id_to_idx_;

  /** @brief Per-plan output slots produced by worker tasks before commit. */
  std::vector<std::optional<NodeOutput>>& temp_results_;

  /**
   * @brief Exact HP implementation snapshots aligned with execution_order_.
   * @note The runner never re-queries metadata by operation key after route
   * selection.
   */
  const std::vector<std::optional<OpImplementation>>& resolved_ops_;

  /** @brief Immutable task graph containing task ids and ROIs. */
  const ComputeTaskGraph& task_graph_;

  /** @brief Full output size inferred from all tile tasks for each node. */
  std::vector<PixelSize> planned_output_sizes_;

  /** @brief Number of tile tasks planned per node index. */
  std::vector<int> tile_task_counts_;

  /** @brief Completed tile count per node index. */
  std::vector<std::atomic<int>> completed_tile_counts_;

  /**
   * @brief Marks nodes satisfied by whole-node memory or disk-cache output.
   *
   * @note This flag distinguishes reusable/cache satisfaction from the final
   * sealed temp result installed by ordinary tiled execution.
   */
  std::vector<std::atomic<bool>> node_precomputed_;

  /** @brief Mutexes guarding per-node temp output allocation. */
  std::vector<std::unique_ptr<std::mutex>> output_mutexes_;

  /**
   * @brief Per-node unpublished Host bindings shared by sibling tile tasks.
   * @note A slot is allocated once under its output mutex and moved only by the
   * last successfully completed tile before formal temp-result publication.
   */
  std::vector<std::unique_ptr<HostOutputBinding>> tile_output_bindings_;

  /** @brief Whether in-memory and disk cache should be bypassed. */
  bool force_recache_;

  /** @brief Whether timing and benchmark data should be collected. */
  bool enable_timing_;

  /** @brief Whether disk cache reads are disabled for this dispatch. */
  bool disable_disk_cache_;

  /** @brief Optional borrowed benchmark event sink, guarded by timing_mutex_.
   */
  std::vector<BenchmarkEvent>* benchmark_events_;

  /**
   * @brief Optional borrowed lifecycle lease for node/tile cancellation checks.
   * @note This pointer never owns Run lifetime and is valid through dispatcher
   * settlement by the NodeTaskRunnerContext contract.
   */
  const ComputeRunLease* run_lease_;

  /** @brief Borrowed frozen per-node work and output-authority records. */
  const std::vector<PlannedNodeWork>* planned_work_;
};

}  // namespace ps::compute
