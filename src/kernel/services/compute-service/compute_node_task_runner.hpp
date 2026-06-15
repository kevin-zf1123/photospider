#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/scheduler/scheduler_task_runtime.hpp"
#include "kernel/services/compute-service/node_executor.hpp"

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
   * @throws GraphError with compute-stage context for OpenCV, standard, and
   * unknown operation failures.
   * @note This method is called from scheduler worker closures and therefore
   * must leave exception transport to SchedulerTaskRuntime.
   */
  void run_node(int node_idx);

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

  /** @brief Attempts to satisfy a node from disk cache into its temp slot. */
  void try_load_disk_cache(const Node& target_node, int node_idx);

  /** @brief Builds runtime parameters by resolving parameter input bindings. */
  YAML::Node resolve_runtime_parameters(const Node& target_node) const;

  /** @brief Resolves image input bindings for operation execution. */
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
