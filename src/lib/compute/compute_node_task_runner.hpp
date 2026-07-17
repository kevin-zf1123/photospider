#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "compute/node_executor.hpp"
#include "compute/task_graph_planning.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "photospider/scheduler/scheduler_task_runtime.hpp"

namespace ps {
class GraphCacheService;
class GraphEventService;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Borrowed state required by NodeTaskRunner worker closures.
 *
 * The context packages graph services, dense plan indexes, temporary result
 * slots, resolved operations, and timing sinks for one dispatcher execute()
 * call. It has no ownership fields; all referenced objects must outlive the
 * scheduler tasks that use the runner.
 *
 * @note The struct is copied into NodeTaskRunner by reference. The owning
 * submission plan must wait for scheduler completion before destroying any
 * referenced vectors or services.
 */
struct NodeTaskRunnerContext {
  /** @brief Graph being read by worker tasks and later committed by caller. */
  GraphModel& graph;

  /** @brief Cache service used for disk reads before operation execution. */
  GraphCacheService& cache;

  /** @brief Event sink for computed and disk-cache node events. */
  GraphEventService& events;

  /** @brief Scheduler runtime used for tile trace events. */
  SchedulerTaskRuntime& task_runtime;

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

  /** @brief Operation variants resolved once for the planned HP intent. */
  const std::vector<std::optional<OpRegistry::OpVariant>>& resolved_ops;

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
   * @brief Binds one worker runner to a borrowed dispatch context.
   *
   * @param context Borrowed graph, service, timing, plan, and option state.
   * @throws Nothing directly.
   * @note The constructor stores references only. Callers must keep every
   * referenced object alive until scheduler completion.
   */
  explicit NodeTaskRunner(NodeTaskRunnerContext context);

  /**
   * @brief Runs the planned node at a dense execution index.
   *
   * @param node_idx Dense index into execution_order_, temp_results_, and
   * resolved_ops_.
   * @return Nothing.
   * @throws std::bad_alloc when node execution exhausts memory.
   * @throws GraphError with compute-stage context for other OpenCV, standard,
   * and unknown operation failures.
   * @note This method is called from scheduler worker closures and therefore
   * must leave exception transport to SchedulerTaskRuntime.
   */
  void run_node(int node_idx);

  /**
   * @brief Runs one planned task by task id.
   *
   * @param task_id Dense id into task_graph.tasks.
   * @return Nothing.
   * @throws std::bad_alloc when task execution exhausts memory.
   * @throws GraphError with compute-stage context for other operation
   * failures.
   * @note Tile tasks execute only their PlannedTask::output_roi. Node and
   * monolithic tasks delegate to run_node().
   */
  void run_task(int task_id);

 private:
  /** @brief Returns whether disk cache reads are allowed for this dispatch. */
  bool allow_disk_cache() const;

  /** @brief Resolves an upstream output from temp slots or committed HP cache.
   */
  const NodeOutput* upstream_output(int up_id) const;

  /** @brief Checks whether a node already has memory or temp output. */
  bool has_memory_or_temp_output(const Node& node, int node_idx) const;

  /** @brief Computes or cache-loads one planned node. */
  void compute_node(int node_idx, int node_id);

  /** @brief Computes one tile task into the node's temporary output buffer. */
  void compute_tile_task(const PlannedTask& task);

  /**
   * @brief Stops a tile task when the node was satisfied by disk cache.
   *
   * @param target_node Graph node whose disk cache entries may be inspected.
   * @param node_idx Dense planned-node index for temp output and status state.
   * @return True when the caller must skip tile execution.
   * @throws Exceptions from disk-cache diagnostic/output storage.
   * @note The method holds the per-node output mutex while it rechecks
   * node_precomputed_ and, if no staging output exists, attempts a single
   * disk-cache load. `temp_results_` alone is not an authority signal because
   * normal tiled execution also uses it as a partially written staging buffer.
   */
  bool try_satisfy_tile_from_disk_cache(const Node& target_node, int node_idx);

  /**
   * @brief Ensures a tile output buffer exists for the planned node.
   *
   * @param node_idx Dense planned-node index for output staging state.
   * @param target_node Node whose fallback dimensions may seed allocation.
   * @param image_inputs Ready image inputs used to infer channels and type.
   * @return Mutable buffer for tile writes, or nullptr when the node became
   * precomputed before buffer allocation.
   * @throws std::bad_alloc or OpenCV allocation exceptions when creating a new
   * aligned image buffer.
   * @note Callers must treat nullptr as a successful skip, not as a compute
   * error, because another tile task has already provided whole-node output.
   */
  ImageBuffer* ensure_tile_output_buffer(
      int node_idx, const Node& target_node,
      const std::vector<const NodeOutput*>& image_inputs);

  /** @brief Finalizes per-node metadata after the last tile task completes. */
  void finalize_tiled_node_if_complete(
      int node_idx, const Node& target_node,
      const std::vector<const NodeOutput*>& image_inputs,
      BenchmarkEvent& current_event);

  /** @brief Attempts to satisfy a node from disk cache into its temp slot. */
  void try_load_disk_cache(const Node& target_node, int node_idx);

  /**
   * @brief Builds request-local runtime parameters from ready upstream values.
   * @param target_node Node whose static parameters and bindings are read.
   * @return Cloned YAML map with parameter-input values overlaid.
   * @throws GraphError when a connected parameter output is unavailable.
   * @throws YAML::Exception or std::bad_alloc from value copying.
   * @note The committed node parameter state is never mutated.
   */
  YAML::Node resolve_runtime_parameters(const Node& target_node) const;

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

  /** @brief Builds tile execution configuration for tile-capable operations. */
  TiledExecutionConfig tiled_config_for(const Node& target_node,
                                        const OpRegistry::OpVariant& op) const;

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

  /** @brief Borrowed scheduler runtime used for tile trace events. */
  SchedulerTaskRuntime& task_runtime_;

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

  /** @brief Resolved high-precision operations aligned with execution_order_.
   */
  const std::vector<std::optional<OpRegistry::OpVariant>>& resolved_ops_;

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
   * @note This flag is distinct from temp_results_ presence: temp_results_ can
   * also hold the partially written staging buffer used by ordinary tiled
   * execution.
   */
  std::vector<std::atomic<bool>> node_precomputed_;

  /** @brief Mutexes guarding per-node temp output allocation. */
  std::vector<std::unique_ptr<std::mutex>> output_mutexes_;

  /** @brief Whether in-memory and disk cache should be bypassed. */
  bool force_recache_;

  /** @brief Whether timing and benchmark data should be collected. */
  bool enable_timing_;

  /** @brief Whether disk cache reads are disabled for this dispatch. */
  bool disable_disk_cache_;

  /** @brief Optional borrowed benchmark event sink, guarded by timing_mutex_.
   */
  std::vector<BenchmarkEvent>* benchmark_events_;
};

}  // namespace ps::compute
