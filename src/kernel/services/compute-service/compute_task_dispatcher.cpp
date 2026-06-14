#include "kernel/services/compute-service/compute_task_dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "kernel/services/compute-service/compute_metrics_recorder.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/compute-service/task_graph_planning.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Finalizes output metadata through the shared metrics recorder.
 *
 * @param output Node output produced by an operation or loaded from cache.
 * @param inputs Image inputs that contributed metadata to the output.
 * @param enable_timing Whether execution timing should be attached.
 * @param execution_ms Measured execution duration in milliseconds.
 * @throws Any exception propagated by ComputeMetricsRecorder.
 * @note This thin wrapper keeps metadata policy centralized while allowing the
 * dispatcher implementation to stay independent from recorder internals.
 */
void finalize_output_metadata(NodeOutput& output,
                              const std::vector<const NodeOutput*>& inputs,
                              bool enable_timing, double execution_ms) {
  ComputeMetricsRecorder::finalize_output_metadata(output, inputs,
                                                   enable_timing, execution_ms);
}

/**
 * @brief Stores the latest compute plan and a bounded recent-plan history.
 *
 * @param graph GraphModel whose inspection fields receive the plan snapshot.
 * @param compute_plan Plan produced for the current dispatch.
 * @throws std::bad_alloc if history storage cannot grow.
 * @note The history cap is intentionally small because plans are diagnostic
 * state, not an unbounded runtime log.
 */
void remember_compute_plan(GraphModel& graph, const ComputePlan& compute_plan) {
  graph.last_compute_plan = compute_plan;
  graph.recent_compute_plans.push_back(compute_plan);
  if (graph.recent_compute_plans.size() > 16) {
    graph.recent_compute_plans.erase(graph.recent_compute_plans.begin());
  }
}

/**
 * @brief Formats a node id with the node name when the graph still contains it.
 *
 * @param graph GraphModel used for node lookup.
 * @param node_id Node id being reported.
 * @return Human-readable context string for error messages.
 * @throws std::bad_alloc if string construction fails.
 * @note Missing nodes are reported by id only so error wrapping remains usable
 * while graph state is being mutated by callers.
 */
std::string node_context(const GraphModel& graph, int node_id) {
  const Node* node = graph.find_node(node_id);
  if (!node) {
    return "node " + std::to_string(node_id);
  }
  return "node " + std::to_string(node_id) + " (" + node->name + ")";
}

/**
 * @brief Creates a compute-stage GraphError exception pointer.
 *
 * @param graph GraphModel used to enrich the node label.
 * @param node_id Node whose operation or dependency resolution failed.
 * @param detail Original exception detail.
 * @return Exception pointer carrying GraphErrc::ComputeError.
 * @throws std::bad_alloc if the wrapped error string cannot be allocated.
 * @note Worker tasks use this helper before rethrowing so scheduler exception
 * capture receives a stable, graph-aware error category.
 */
std::exception_ptr compute_failure(const GraphModel& graph, int node_id,
                                   const std::string& detail) {
  return std::make_exception_ptr(
      GraphError(GraphErrc::ComputeError, "Compute stage at " +
                                              node_context(graph, node_id) +
                                              " failed: " + detail));
}

/**
 * @brief Creates a scheduler-stage GraphError exception pointer.
 *
 * @param graph GraphModel used to enrich the node label.
 * @param node_id Node whose dependent release failed.
 * @param detail Original scheduling exception detail.
 * @return Exception pointer carrying GraphErrc::ComputeError.
 * @throws std::bad_alloc if the wrapped error string cannot be allocated.
 * @note This separates compute failures from dependency-release failures while
 * preserving the scheduler runtime's cross-thread exception transport.
 */
std::exception_ptr scheduling_failure(const GraphModel& graph, int node_id,
                                      const std::string& detail) {
  return std::make_exception_ptr(
      GraphError(GraphErrc::ComputeError, "Scheduling stage after " +
                                              node_context(graph, node_id) +
                                              " failed: " + detail));
}

/**
 * @brief Borrowed state required by NodeTaskRunner worker closures.
 *
 * The context packages graph services, dense plan indexes, temporary result
 * slots, resolved operations, and timing sinks for one execute() call. It is
 * copied into NodeTaskRunner by reference; none of the referenced objects may
 * be destroyed until the scheduler has completed all tasks for the plan.
 *
 * @note The struct intentionally has no ownership fields. Lifetime is governed
 * by ComputeTaskDispatcher::execute(), which builds the plan and waits for all
 * scheduler work before returning.
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
 * It deliberately avoids mutating GraphModel cache ownership; ResultCommitter
 * performs that serialized side effect after all worker tasks finish.
 *
 * @note The runner borrows plan vectors, services, and graph state. It must not
 * outlive the TaskSubmissionPlan or the active execute() call that created it.
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
  explicit NodeTaskRunner(NodeTaskRunnerContext context)
      : graph_(context.graph),
        cache_(context.cache),
        events_(context.events),
        task_runtime_(context.task_runtime),
        timing_results_(context.timing_results),
        timing_mutex_(context.timing_mutex),
        execution_order_(context.execution_order),
        id_to_idx_(context.id_to_idx),
        temp_results_(context.temp_results),
        resolved_ops_(context.resolved_ops),
        force_recache_(context.force_recache),
        enable_timing_(context.enable_timing),
        disable_disk_cache_(context.disable_disk_cache),
        benchmark_events_(context.benchmark_events) {}

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
  void run_node(int node_idx) {
    const int node_id = execution_order_.at(node_idx);
    try {
      compute_node(node_idx, node_id);
    } catch (const cv::Exception& e) {
      std::rethrow_exception(compute_failure(graph_, node_id, e.what()));
    } catch (const std::exception& e) {
      std::rethrow_exception(compute_failure(graph_, node_id, e.what()));
    } catch (...) {
      std::rethrow_exception(
          compute_failure(graph_, node_id, "unknown exception"));
    }
  }

 private:
  /**
   * @brief Returns whether disk cache reads are allowed for this dispatch.
   *
   * @return true when disk cache is enabled and the caller did not force
   * recache.
   * @throws Nothing.
   * @note Disk cache writes are controlled later by ResultCommitter and the
   * cache service configuration.
   */
  bool allow_disk_cache() const {
    return !disable_disk_cache_ && !force_recache_;
  }

  /**
   * @brief Resolves an upstream output from temp slots or committed HP cache.
   *
   * @param up_id Upstream node id referenced by an input binding.
   * @return Pointer to the upstream output, or nullptr when unavailable.
   * @throws std::out_of_range if dense indexes become inconsistent.
   * @note Temp results take precedence so downstream tasks observe outputs
   * produced in the same scheduler plan before those outputs are committed to
   * GraphModel.
   */
  const NodeOutput* upstream_output(int up_id) const {
    if (up_id < 0) {
      return nullptr;
    }
    const Node* upstream = graph_.find_node(up_id);
    if (!upstream) {
      return nullptr;
    }
    auto it_idx = id_to_idx_.find(up_id);
    if (it_idx != id_to_idx_.end()) {
      const int up_idx = it_idx->second;
      if (temp_results_[up_idx].has_value()) {
        return &*temp_results_[up_idx];
      }
    }
    if (upstream->cached_output_high_precision.has_value()) {
      return &*upstream->cached_output_high_precision;
    }
    return nullptr;
  }

  /**
   * @brief Checks whether a node already has a usable in-memory result.
   *
   * @param node Graph node being considered.
   * @param node_idx Dense index for the same node in temp_results_.
   * @return true when committed HP cache or a temp result exists.
   * @throws std::out_of_range if node_idx is outside temp_results_.
   * @note This helper intentionally ignores disk cache; disk cache loading is a
   * separate step so timing and event sources stay accurate.
   */
  bool has_memory_or_temp_output(const Node& node, int node_idx) const {
    return node.cached_output_high_precision.has_value() ||
           temp_results_[node_idx].has_value();
  }

  /**
   * @brief Computes or cache-loads one planned node.
   *
   * @param node_idx Dense index of the planned node.
   * @param node_id Graph node id for the same planned node.
   * @throws GraphError for missing dependencies or operation failures.
   * @note The acquire fence pairs with release_dependents() so dependent tasks
   * see upstream temp_results_ writes before resolving their inputs.
   */
  void compute_node(int node_idx, int node_id) {
    const Node& target_node = graph_.node(node_id);
    // Pairs with release_dependents() so downstream nodes see upstream
    // temp_results writes before resolving their inputs.
    std::atomic_thread_fence(std::memory_order_acquire);

    if (!has_memory_or_temp_output(target_node, node_idx)) {
      try_load_disk_cache(target_node, node_idx);
    }
    if (!has_memory_or_temp_output(target_node, node_idx)) {
      compute_uncached_node(target_node, node_idx);
    }
  }

  /**
   * @brief Attempts to satisfy a node from disk cache into its temp slot.
   *
   * @param target_node Node whose configured cache key is probed.
   * @param node_idx Dense index of target_node.
   * @throws Exceptions from GraphCacheService disk access.
   * @note A disk hit still waits for ResultCommitter before mutating the
   * GraphModel HP cache field.
   */
  void try_load_disk_cache(const Node& target_node, int node_idx) {
    if (!allow_disk_cache()) {
      return;
    }
    NodeOutput from_disk;
    if (cache_.try_load_from_disk_cache_into(graph_, target_node, from_disk)) {
      // Disk hits follow the same temp-result path as computed nodes, keeping
      // all formal GraphModel cache mutation in ResultCommitter.
      temp_results_[node_idx] = std::move(from_disk);
      record_disk_cache_hit(target_node);
    }
  }

  /**
   * @brief Builds runtime parameters by resolving parameter input bindings.
   *
   * @param target_node Node whose static parameters and parameter_inputs are
   * merged.
   * @return YAML map passed to operation execution.
   * @throws GraphError with GraphErrc::MissingDependency when an upstream
   * parameter source or named output is unavailable.
   * @note The returned YAML node is a per-task clone and does not mutate the
   * GraphModel node stored in the graph.
   */
  YAML::Node resolve_runtime_parameters(const Node& target_node) const {
    YAML::Node runtime_params = target_node.parameters
                                    ? YAML::Clone(target_node.parameters)
                                    : YAML::Node(YAML::NodeType::Map);
    for (const auto& p_input : target_node.parameter_inputs) {
      if (p_input.from_node_id < 0) {
        continue;
      }
      auto const* up_out = upstream_output(p_input.from_node_id);
      if (!up_out) {
        throw GraphError(GraphErrc::MissingDependency,
                         "Parameter input not ready for node " +
                             std::to_string(target_node.id));
      }
      auto it = up_out->data.find(p_input.from_output_name);
      if (it == up_out->data.end()) {
        throw GraphError(GraphErrc::MissingDependency,
                         "Node " + std::to_string(p_input.from_node_id) +
                             " missing output '" + p_input.from_output_name +
                             "'");
      }
      runtime_params[p_input.to_parameter_name] = it->second;
    }
    return runtime_params;
  }

  /**
   * @brief Resolves image input bindings for operation execution.
   *
   * @param target_node Node whose image_inputs are read.
   * @return Ordered upstream NodeOutput pointers for the operation.
   * @throws GraphError with GraphErrc::MissingDependency when an image input
   * output is not available.
   * @note Returned pointers refer either to plan temp slots or committed HP
   * cache entries and remain valid for the duration of the worker call.
   */
  std::vector<const NodeOutput*> resolve_image_inputs(
      const Node& target_node) const {
    std::vector<const NodeOutput*> inputs_ready;
    inputs_ready.reserve(target_node.image_inputs.size());
    for (const auto& i_input : target_node.image_inputs) {
      if (i_input.from_node_id < 0) {
        continue;
      }
      auto const* up_out = upstream_output(i_input.from_node_id);
      if (!up_out) {
        throw GraphError(
            GraphErrc::MissingDependency,
            "Image input not ready for node " + std::to_string(target_node.id));
      }
      inputs_ready.push_back(up_out);
    }
    return inputs_ready;
  }

  /**
   * @brief Builds tile execution configuration for tile-capable operations.
   *
   * @param target_node Node whose operation metadata is inspected.
   * @param op Resolved operation variant for target_node.
   * @return TiledExecutionConfig with metadata, tile size, and scheduler trace
   * callback when op is a TileOpFunc; otherwise the default config.
   * @throws Exceptions from operation metadata lookup or callback allocation.
   * @note The tile callback records scheduler trace events only. Tile execution
   * and image mutation remain owned by NodeExecutor and the operation.
   */
  TiledExecutionConfig tiled_config_for(const Node& target_node,
                                        const OpRegistry::OpVariant& op) const {
    TiledExecutionConfig tiled_config;
    if (!std::holds_alternative<TileOpFunc>(op)) {
      return tiled_config;
    }
    if (auto metadata = OpRegistry::instance().get_metadata(
            target_node.type, target_node.subtype)) {
      tiled_config.metadata = *metadata;
      if (metadata->tile_preference == TileSizePreference::MICRO) {
        tiled_config.tile_size = 16;
      } else if (metadata->tile_preference == TileSizePreference::MACRO) {
        tiled_config.tile_size = 256;
      }
    }
    tiled_config.on_tile = [this, node_id = target_node.id](const cv::Rect&) {
      task_runtime_.log_event(SchedulerTraceAction::ExecuteTile, node_id);
    };
    return tiled_config;
  }

  /**
   * @brief Creates a benchmark event initialized to the node execution start.
   *
   * @param target_node Node being measured.
   * @return BenchmarkEvent with node id, operation name, dependency start, and
   * execution start populated.
   * @throws std::bad_alloc if operation key construction fails.
   * @note Dependency and execution start are identical because dependency wait
   * time is already represented by scheduler task readiness.
   */
  BenchmarkEvent start_event(const Node& target_node) const {
    BenchmarkEvent event;
    event.node_id = target_node.id;
    event.op_name = make_key(target_node.type, target_node.subtype);
    event.dependency_start_time = std::chrono::high_resolution_clock::now();
    event.execution_start_time = event.dependency_start_time;
    return event;
  }

  /**
   * @brief Executes an operation when memory and disk cache did not satisfy it.
   *
   * @param target_node Graph node whose operation is executed.
   * @param node_idx Dense index used to read the resolved operation and write
   * the temp result.
   * @throws GraphError with GraphErrc::NoOperation or MissingDependency, plus
   * any exception propagated by NodeExecutor.
   * @note The method copies the graph node before assigning runtime_parameters
   * so worker tasks do not mutate shared GraphModel node state.
   */
  void compute_uncached_node(const Node& target_node, int node_idx) {
    const auto& op_opt = resolved_ops_[node_idx];
    if (!op_opt.has_value()) {
      throw GraphError(GraphErrc::NoOperation, "No op for " + target_node.type +
                                                   ":" + target_node.subtype);
    }

    YAML::Node runtime_params = resolve_runtime_parameters(target_node);
    std::vector<const NodeOutput*> inputs_ready =
        resolve_image_inputs(target_node);
    BenchmarkEvent current_event = start_event(target_node);

    // Runtime parameters are node-local execution state; copying avoids
    // mutating GraphModel while worker tasks are still running.
    Node node_for_exec = target_node;
    node_for_exec.runtime_parameters = runtime_params;
    TiledExecutionConfig tiled_config = tiled_config_for(target_node, *op_opt);
    NodeOutput result = NodeExecutor::execute(graph_, node_for_exec, *op_opt,
                                              inputs_ready, tiled_config);

    const double execution_ms =
        record_computed_output(target_node, current_event);
    finalize_output_metadata(result, inputs_ready, enable_timing_,
                             execution_ms);
    temp_results_[node_idx] = std::move(result);
  }

  /**
   * @brief Records timing and event state for a disk-cache hit.
   *
   * @param target_node Node whose output came from disk cache.
   * @throws std::bad_alloc if benchmark or timing vectors must grow.
   * @note benchmark_events_ and timing_results_ are both updated under
   * timing_mutex_ so timed dispatch remains thread-safe.
   */
  void record_disk_cache_hit(const Node& target_node) {
    if (!enable_timing_) {
      events_.push(target_node.id, target_node.name, "disk_cache", 0.0);
      return;
    }

    BenchmarkEvent event = start_event(target_node);
    event.execution_end_time = event.execution_start_time;
    event.execution_duration_ms = 0.0;
    event.source = "disk_cache";
    if (benchmark_events_) {
      std::lock_guard lk(timing_mutex_);
      benchmark_events_->push_back(event);
    }
    {
      std::lock_guard lk(timing_mutex_);
      timing_results_.node_timings.push_back(
          {target_node.id, target_node.name, 0.0, "disk_cache"});
    }
    events_.push(target_node.id, target_node.name, "disk_cache", 0.0);
  }

  /**
   * @brief Records timing and event state for a computed output.
   *
   * @param target_node Node whose operation just completed.
   * @param current_event Mutable benchmark event initialized by start_event().
   * @return Elapsed operation time in milliseconds, or 0.0 when timing is
   * disabled.
   * @throws std::bad_alloc if benchmark or timing vectors must grow.
   * @note The returned value is passed into output metadata finalization so the
   * output and graph timing stream stay consistent.
   */
  double record_computed_output(const Node& target_node,
                                BenchmarkEvent& current_event) {
    if (!enable_timing_) {
      events_.push(target_node.id, target_node.name, "computed", 0.0);
      return 0.0;
    }

    current_event.execution_end_time =
        std::chrono::high_resolution_clock::now();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  current_event.execution_end_time -
                                  current_event.execution_start_time)
                                  .count();
    current_event.source = "computed";
    current_event.execution_duration_ms = elapsed_ms;
    if (benchmark_events_) {
      std::lock_guard lk(timing_mutex_);
      benchmark_events_->push_back(current_event);
    }
    {
      std::lock_guard lk(timing_mutex_);
      timing_results_.node_timings.push_back(
          {target_node.id, target_node.name, elapsed_ms, "computed"});
    }
    events_.push(target_node.id, target_node.name, "computed", elapsed_ms);
    return elapsed_ms;
  }

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

/**
 * @brief Owns the scheduler-facing shape of one high-precision ComputePlan.
 *
 * TaskSubmissionPlan converts the cache-pruned ComputePlan into dense indexes,
 * dependency counters, dependent adjacency lists, scheduler closures, and
 * temporary result slots. It is the authority for which nodes run during one
 * ComputeTaskDispatcher::execute() call.
 *
 * @note The plan owns task closures that capture this object by reference.
 * Therefore the plan must remain alive until SchedulerTaskRuntime has drained
 * all submitted tasks.
 */
class TaskSubmissionPlan {
 public:
  /**
   * @brief Builds a cache-pruned high-precision plan for a target node.
   *
   * @param graph GraphModel used for traversal, pruning, operation lookup, and
   * diagnostic plan storage.
   * @param traversal Traversal service used to compute the initial postorder.
   * @param node_id Target node id for the GlobalHighPrecision request.
   * @throws GraphError or standard exceptions from traversal, pruning, graph
   * lookup, allocation, or operation resolution.
   * @note The constructor records the generated ComputePlan into graph
   * inspection fields before scheduler tasks are built.
   */
  TaskSubmissionPlan(GraphModel& graph, GraphTraversalService& traversal,
                     int node_id)
      : graph_(graph),
        execution_order_(traversal.topo_postorder_from(graph, node_id)) {
    FullTaskGraphExpander full_expander;
    NodeCacheTaskGraphPruner node_cache_pruner;
    const ComputeRequest request{ComputeIntent::GlobalHighPrecision, node_id,
                                 true, std::nullopt};
    const FullTaskGraph full_graph =
        full_expander.expand(graph, request.intent);
    compute_plan_ =
        node_cache_pruner.prune(full_graph, request, execution_order_, graph);
    remember_compute_plan(graph, compute_plan_);
    // The pruned plan is the authority for both node order and dependencies;
    // do not rebuild dependencies from raw GraphModel edges below.
    execution_order_ = compute_plan_.planned_nodes;
    build_dense_index();
    build_dependency_state();
    resolve_operations();
    temp_results_.resize(execution_order_.size());
    tasks_.resize(execution_order_.size());
  }

  /**
   * @brief Reports whether the pruned plan contains no executable nodes.
   *
   * @return true when execution_order_ is empty.
   * @throws Nothing.
   * @note An empty plan may still require sequential fallback for the target
   * node if no high-precision cache exists.
   */
  bool empty() const { return execution_order_.empty(); }

  /**
   * @brief Returns the number of planned executable nodes.
   *
   * @return Dense task count aligned with execution_order_.
   * @throws Nothing.
   */
  size_t size() const { return execution_order_.size(); }

  /**
   * @brief Returns the planned node id order.
   *
   * @return Const reference to dense planned node ids.
   * @throws Nothing.
   * @note The returned reference remains valid only while this plan object is
   * alive.
   */
  const std::vector<int>& execution_order() const { return execution_order_; }

  /**
   * @brief Returns the node id to dense index lookup.
   *
   * @return Const reference to the lookup map used by worker input resolution.
   * @throws Nothing.
   * @note The returned reference remains valid only while this plan object is
   * alive.
   */
  const std::unordered_map<int, int>& id_to_idx() const { return id_to_idx_; }

  /**
   * @brief Returns mutable temporary result slots for planned nodes.
   *
   * @return Reference to optional outputs aligned with execution_order_.
   * @throws Nothing.
   * @note Worker tasks publish into these slots; ResultCommitter later moves
   * populated values into GraphModel under the graph mutex.
   */
  std::vector<std::optional<NodeOutput>>& temp_results() {
    return temp_results_;
  }

  /**
   * @brief Returns resolved operations for planned high-precision nodes.
   *
   * @return Const reference to optional operation variants aligned with
   * execution_order_.
   * @throws Nothing.
   * @note Missing operations are preserved as empty optionals so NodeTaskRunner
   * can throw a node-specific GraphError at execution time.
   */
  const std::vector<std::optional<OpRegistry::OpVariant>>& resolved_ops()
      const {
    return resolved_ops_;
  }

  /**
   * @brief Materializes scheduler closures for all planned nodes.
   *
   * @param runner Worker runner used by each closure.
   * @param task_runtime Scheduler runtime used for trace events, dependent task
   * submission, and completion accounting.
   * @throws std::bad_alloc if task closure storage allocation fails.
   * @note Each task captures this plan and runner by reference. Callers must
   * call wait_for_completion() before either object is destroyed.
   */
  void build_scheduler_tasks(NodeTaskRunner& runner,
                             SchedulerTaskRuntime& task_runtime) {
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      const int current_node_id = execution_order_[i];
      const int current_node_idx = static_cast<int>(i);
      tasks_[i] = [this, &runner, &task_runtime, current_node_id,
                   current_node_idx]() {
        task_runtime.log_event(SchedulerTraceAction::Execute, current_node_id);
        try {
          runner.run_node(current_node_idx);
          release_dependents(current_node_idx, current_node_id, task_runtime);
        } catch (...) {
          // SchedulerTaskRuntime owns cross-thread exception capture. The task
          // only adds trace context, then rethrows to the runtime wrapper.
          task_runtime.log_event(SchedulerTraceAction::RethrowException,
                                 current_node_id);
          throw;
        }
        task_runtime.dec_tasks_to_complete();
      };
    }
  }

  /**
   * @brief Moves ready initial scheduler tasks out of the plan.
   *
   * @return Initial task closures that may be submitted to
   * SchedulerTaskRuntime.
   * @throws std::bad_alloc if temporary ready-task storage grows.
   * @note TaskGraphReadyChecker is preferred. If the pruned graph carries no
   * initial ids, zero-dependency nodes are used as a compatibility fallback.
   */
  std::vector<SchedulerTaskRuntime::Task> take_initial_tasks() {
    initial_tasks_.clear();
    submitted_initial_indices_.clear();
    append_graph_ready_tasks();
    if (initial_tasks_.empty()) {
      // Some cache-pruned or legacy plans may not carry initial task ids.
      // Falling back to zero-dependency nodes keeps the scheduler contract
      // live.
      append_zero_dependency_tasks();
    }
    return std::move(initial_tasks_);
  }

  /**
   * @brief Emits trace events for tasks selected as initial scheduler work.
   *
   * @param task_runtime Scheduler runtime that receives AssignInitial events.
   * @throws Exceptions from task_runtime.log_event().
   * @note This is called after submit_initial_tasks() so traces reflect the
   * actual set of moved initial closures.
   */
  void log_initial_assignments(SchedulerTaskRuntime& task_runtime) const {
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      if (submitted_initial_indices_.count(static_cast<int>(i))) {
        task_runtime.log_event(SchedulerTraceAction::AssignInitial,
                               execution_order_[i]);
      }
    }
  }

 private:
  /**
   * @brief Builds dense node id indexes for planned nodes.
   *
   * @throws std::bad_alloc if the map allocation fails.
   * @note Dense indexes avoid vectors sized by sparse user-visible node ids.
   */
  void build_dense_index() {
    id_to_idx_.reserve(execution_order_.size());
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      id_to_idx_[execution_order_[i]] = static_cast<int>(i);
    }
  }

  /**
   * @brief Builds dependency counters and dependent adjacency lists.
   *
   * @throws std::bad_alloc if counter or adjacency storage allocation fails.
   * @note Only dependencies whose endpoints survive pruning are represented in
   * scheduler state.
   */
  void build_dependency_state() {
    const size_t node_count = execution_order_.size();
    dependency_counters_ = std::vector<std::atomic<int>>(node_count);
    dependents_map_.assign(node_count, {});
    for (auto& counter : dependency_counters_) {
      counter.store(0, std::memory_order_relaxed);
    }

    // Scheduler arrays are indexed densely, not by node id. This preserves
    // sparse graph ids such as node 100 without oversized vectors.
    std::unordered_set<int> execution_set(execution_order_.begin(),
                                          execution_order_.end());
    for (const auto& dependency : compute_plan_.task_graph.dependencies) {
      if (!execution_set.count(dependency.from_node_id) ||
          !execution_set.count(dependency.to_node_id)) {
        continue;
      }
      const int dep_idx = id_to_idx_.at(dependency.from_node_id);
      const int dependent_idx = id_to_idx_.at(dependency.to_node_id);
      dependents_map_[dep_idx].push_back(dependent_idx);
      dependency_counters_[dependent_idx].fetch_add(1,
                                                    std::memory_order_relaxed);
    }
  }

  /**
   * @brief Resolves GlobalHighPrecision operation variants for planned nodes.
   *
   * @throws Exceptions from graph node lookup or operation registry access.
   * @note The result vector keeps empty optionals for unresolved operations so
   * execution can fail with target-node context.
   */
  void resolve_operations() {
    resolved_ops_.resize(execution_order_.size());
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      const auto& node = graph_.node(execution_order_[i]);
      resolved_ops_[i] = OpRegistry::instance().resolve_for_intent(
          node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
    }
  }

  /**
   * @brief Releases dependent tasks whose upstream counters reached zero.
   *
   * @param current_node_idx Dense index of the completed node.
   * @param current_node_id Graph node id of the completed node for trace and
   * error context.
   * @param task_runtime Scheduler runtime receiving newly-ready worker tasks.
   * @throws GraphError wrapping dependency-release or scheduler submission
   * failures.
   * @note The release fence publishes temp_results_ writes before a dependent
   * closure can resolve upstream outputs after its counter reaches zero.
   */
  void release_dependents(int current_node_idx, int current_node_id,
                          SchedulerTaskRuntime& task_runtime) {
    try {
      // Publish temp_results before the dependent task can observe its counter
      // reaching zero and start resolving upstream outputs.
      std::atomic_thread_fence(std::memory_order_release);
      for (int dependent_idx : dependents_map_[current_node_idx]) {
        const int previous = dependency_counters_[dependent_idx].fetch_sub(
            1, std::memory_order_acq_rel);
        if (previous == 1) {
          task_runtime.submit_ready_task_from_worker(
              std::move(tasks_[dependent_idx]));
        }
      }
    } catch (const std::out_of_range& e) {
      std::rethrow_exception(scheduling_failure(
          graph_, current_node_id, "out_of_range: " + std::string(e.what())));
    } catch (const std::exception& e) {
      std::rethrow_exception(
          scheduling_failure(graph_, current_node_id, e.what()));
    } catch (...) {
      std::rethrow_exception(
          scheduling_failure(graph_, current_node_id, "unknown exception"));
    }
  }

  /**
   * @brief Appends a node's task when all dependencies are already satisfied.
   *
   * @param node_idx Dense index of the candidate initial task.
   * @throws std::bad_alloc if ready-task or deduplication storage grows.
   * @note The submitted_initial_indices_ set prevents moving the same task more
   * than once when graph-ready and zero-dependency paths overlap.
   */
  void append_initial_task_for_node(int node_idx) {
    if (dependency_counters_[node_idx].load(std::memory_order_acquire) != 0) {
      return;
    }
    if (submitted_initial_indices_.insert(node_idx).second) {
      initial_tasks_.push_back(std::move(tasks_[node_idx]));
    }
  }

  /**
   * @brief Appends initial tasks identified by the planned task graph.
   *
   * @throws std::bad_alloc if ready-task storage grows.
   * @note Invalid or pruned task ids are ignored because dependency counters
   * remain the final scheduler safety check.
   */
  void append_graph_ready_tasks() {
    TaskGraphReadyChecker ready_checker;
    const std::vector<int> initial_ready_task_ids =
        ready_checker.initial_ready_task_ids(compute_plan_.task_graph);
    for (int task_id : initial_ready_task_ids) {
      if (task_id < 0 ||
          task_id >= static_cast<int>(compute_plan_.task_graph.tasks.size())) {
        continue;
      }
      const int planned_node_id =
          compute_plan_.task_graph.tasks[task_id].node_id;
      auto idx_it = id_to_idx_.find(planned_node_id);
      if (idx_it == id_to_idx_.end()) {
        continue;
      }
      append_initial_task_for_node(idx_it->second);
    }
  }

  /**
   * @brief Appends all zero-dependency nodes as initial scheduler work.
   *
   * @throws std::bad_alloc if ready-task storage grows.
   * @note Used only when the pruned task graph did not expose explicit initial
   * task ids.
   */
  void append_zero_dependency_tasks() {
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      append_initial_task_for_node(static_cast<int>(i));
    }
  }

  /** @brief Borrowed graph used for plan inspection and operation resolution.
   */
  GraphModel& graph_;

  /** @brief Dense planned node ids after cache pruning. */
  std::vector<int> execution_order_;

  /** @brief Cache-pruned compute plan recorded for inspection and scheduling.
   */
  ComputePlan compute_plan_;

  /** @brief Node id to dense execution index lookup. */
  std::unordered_map<int, int> id_to_idx_;

  /** @brief Remaining dependency counts for each dense planned node. */
  std::vector<std::atomic<int>> dependency_counters_;

  /** @brief Dense dependent node indexes keyed by upstream dense index. */
  std::vector<std::vector<int>> dependents_map_;

  /** @brief Scheduler closures aligned with execution_order_. */
  std::vector<SchedulerTaskRuntime::Task> tasks_;

  /** @brief Initial task closures moved out for submit_initial_tasks(). */
  std::vector<SchedulerTaskRuntime::Task> initial_tasks_;

  /** @brief Dense indexes already selected as initial tasks. */
  std::unordered_set<int> submitted_initial_indices_;

  /** @brief Temporary worker outputs aligned with execution_order_. */
  std::vector<std::optional<NodeOutput>> temp_results_;

  /** @brief Resolved operation variants aligned with execution_order_. */
  std::vector<std::optional<OpRegistry::OpVariant>> resolved_ops_;
};

/**
 * @brief Serializes side effects after scheduler worker tasks finish.
 *
 * ResultCommitter owns the post-dispatch mutation phase that workers
 * deliberately avoid: timing total calculation, GraphModel high-precision cache
 * updates, HP version increments, and configured disk cache writes.
 *
 * @note commit() holds the graph mutex while moving temp outputs into node
 * runtime state so GraphModel remains the sole owner of committed HP cache
 * entries.
 */
class ResultCommitter {
 public:
  /**
   * @brief Binds the committer to cache and graph synchronization state.
   *
   * @param cache Cache service used for optional disk cache writes.
   * @param graph_mutex Graph mutex guarding runtime cache mutation.
   * @param cache_precision Cache precision label forwarded to cache writes.
   * @throws Nothing directly.
   * @note The committer stores references only and must not outlive the graph
   * and cache service used for the active dispatch.
   */
  ResultCommitter(GraphCacheService& cache, std::mutex& graph_mutex,
                  const std::string& cache_precision)
      : cache_(cache),
        graph_mutex_(graph_mutex),
        cache_precision_(cache_precision) {}

  /**
   * @brief Recomputes total timed execution duration from node timings.
   *
   * @param timing_results Timing collector to finalize.
   * @param timing_mutex Mutex protecting timing_results.
   * @throws Nothing directly.
   * @note The total is recomputed after all workers finish to avoid concurrent
   * aggregate mutation during task execution.
   */
  void finalize_timing(TimingCollector& timing_results,
                       std::mutex& timing_mutex) const {
    double total = 0.0;
    {
      std::lock_guard lk(timing_mutex);
      for (const auto& timing : timing_results.node_timings) {
        total += timing.elapsed_ms;
      }
      timing_results.total_ms = total;
    }
  }

  /**
   * @brief Commits populated temp outputs into GraphModel runtime cache state.
   *
   * @param graph Graph whose high-precision node caches are updated.
   * @param execution_order Dense planned node id order.
   * @param temp_results Temporary outputs aligned with execution_order.
   * @throws Exceptions from GraphModel mutation or GraphCacheService writes.
   * @note temp_results values are moved. After commit(), populated slots no
   * longer own valid output values.
   */
  void commit(GraphModel& graph, const std::vector<int>& execution_order,
              std::vector<std::optional<NodeOutput>>& temp_results) const {
    std::scoped_lock lock(graph_mutex_);
    for (size_t i = 0; i < execution_order.size(); ++i) {
      if (!temp_results[i].has_value()) {
        continue;
      }
      const int node_id = execution_order[i];
      // A single serialized commit preserves GraphModel's HP cache authority
      // after worker tasks have finished producing independent outputs.
      graph.mutate_node_runtime_state(
          node_id, [&](GraphModel::NodeRuntimeState& state) {
            state.cached_output_high_precision = std::move(*temp_results[i]);
            state.hp_version++;
          });
      cache_.save_cache_if_configured(graph, graph.node(node_id),
                                      cache_precision_);
    }
  }

 private:
  /** @brief Borrowed cache service used after GraphModel mutation. */
  GraphCacheService& cache_;

  /** @brief Borrowed graph mutex held during committed cache mutation. */
  std::mutex& graph_mutex_;

  /** @brief Cache precision label used by save_cache_if_configured(). */
  const std::string& cache_precision_;
};

/**
 * @brief Clears high-precision memory cache for nodes in a planned dispatch.
 *
 * @param graph Graph whose planned node HP caches are cleared.
 * @param graph_mutex Mutex guarding node runtime state mutation.
 * @param order Planned node ids whose HP cache entries should be reset.
 * @throws Exceptions from GraphModel runtime-state mutation.
 * @note Missing node ids are skipped so stale diagnostic plan ids do not cause
 * recache requests to fail before execution starts.
 */
void clear_planned_high_precision_caches(GraphModel& graph,
                                         std::mutex& graph_mutex,
                                         const std::vector<int>& order) {
  std::scoped_lock lock(graph_mutex);
  for (int id : order) {
    if (!graph.has_node(id)) {
      continue;
    }
    graph.mutate_node_runtime_state(
        id, [](GraphModel::NodeRuntimeState& state) {
          state.cached_output_high_precision.reset();
        });
  }
}

/**
 * @brief Submits the planned scheduler work or runs the sequential fallback.
 *
 * @param graph GraphModel being computed.
 * @param task_runtime Scheduler runtime receiving initial planned tasks.
 * @param node_id Target node id for fallback checks.
 * @param disable_disk_cache Whether the sequential fallback may read disk
 * cache.
 * @param plan Built scheduler submission plan.
 * @param sequential_fallback Fallback callback for empty plans.
 * @throws Exceptions from fallback execution, task submission, or scheduler
 * completion.
 * @note Scheduler completion is awaited before plan-owned task closures and
 * temporary result slots can be destroyed.
 */
void dispatch_or_run_fallback(
    GraphModel& graph, SchedulerTaskRuntime& task_runtime, int node_id,
    bool disable_disk_cache, TaskSubmissionPlan& plan,
    ComputeTaskDispatcher::SequentialFallback sequential_fallback) {
  if (plan.empty() && graph.has_node(node_id)) {
    if (!graph.node(node_id).cached_output_high_precision.has_value()) {
      sequential_fallback(graph, node_id, !disable_disk_cache);
    }
    return;
  }

  std::vector<SchedulerTaskRuntime::Task> initial_tasks =
      plan.take_initial_tasks();
  task_runtime.submit_initial_tasks(std::move(initial_tasks),
                                    static_cast<int>(plan.size()));
  plan.log_initial_assignments(task_runtime);
  task_runtime.wait_for_completion();
}

}  // namespace

/**
 * @brief Constructs the dispatcher with borrowed compute services.
 *
 * @param traversal Traversal service used by TaskSubmissionPlan.
 * @param cache Cache service used by workers and ResultCommitter.
 * @param events Event service used by NodeTaskRunner.
 * @throws Nothing directly.
 * @note Implementation stores references only; ownership remains with
 * ComputeService.
 */
ComputeTaskDispatcher::ComputeTaskDispatcher(GraphTraversalService& traversal,
                                             GraphCacheService& cache,
                                             GraphEventService& events)
    : traversal_(traversal), cache_(cache), events_(events) {}

/**
 * @brief Clears timing entries for a new timed dispatch.
 *
 * @param graph Graph whose TimingCollector is reset.
 * @throws Nothing directly.
 * @note This does not reset graph.total_io_time_ms; execute() handles that
 * field alongside this helper.
 */
void ComputeTaskDispatcher::clear_timing_results(GraphModel& graph) {
  std::lock_guard<std::mutex> lk(graph.timing_mutex_);
  graph.timing_results.node_timings.clear();
  graph.timing_results.total_ms = 0.0;
}

/**
 * @brief Submits dirty ready tasks with source-before-downstream ordering.
 *
 * @param task_runtime Scheduler runtime that executes and records task errors.
 * @param source_tasks Dirty source tasks to run with high priority.
 * @param downstream_tasks Dependent tasks to run with normal priority after
 * source completion and optional validation.
 * @param epoch Optional scheduler epoch passed through to task submission.
 * @param before_downstream Optional boundary validation callback.
 * @throws Rethrows any task or callback exception after recording it in
 * task_runtime.
 * @note Source tasks may run concurrently, but downstream tasks are submitted
 * one at a time to preserve deterministic ordering at dirty update call sites.
 */
void ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
    SchedulerTaskRuntime& task_runtime,
    std::vector<SchedulerTaskRuntime::Task>&& source_tasks,
    std::vector<SchedulerTaskRuntime::Task>&& downstream_tasks,
    std::optional<uint64_t> epoch, std::function<void()> before_downstream) {
  /// Submits one task and waits for its completion before returning.
  auto submit_one = [&](SchedulerTaskRuntime::Task&& task,
                        SchedulerTaskPriority priority) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> future = done->get_future();
    task_runtime.submit_ready_task_any_thread(
        [task = std::move(task), done, &task_runtime]() mutable {
          try {
            if (task) {
              task();
            }
            done->set_value();
          } catch (...) {
            auto error = std::current_exception();
            task_runtime.log_event(SchedulerTraceAction::RethrowException, -1);
            task_runtime.set_exception(error);
            done->set_exception(error);
          }
        },
        priority, epoch);
    future.get();
  };

  /// Submits a task batch either concurrently or in deterministic vector order.
  auto submit_and_wait = [&](std::vector<SchedulerTaskRuntime::Task>&& tasks,
                             SchedulerTaskPriority priority,
                             bool preserve_order) {
    if (preserve_order) {
      for (auto& task : tasks) {
        submit_one(std::move(task), priority);
      }
      return;
    }
    std::vector<std::future<void>> futures;
    futures.reserve(tasks.size());
    for (auto& task : tasks) {
      auto done = std::make_shared<std::promise<void>>();
      futures.push_back(done->get_future());
      task_runtime.submit_ready_task_any_thread(
          [task = std::move(task), done, &task_runtime]() mutable {
            try {
              if (task) {
                task();
              }
              done->set_value();
            } catch (...) {
              auto error = std::current_exception();
              task_runtime.log_event(SchedulerTraceAction::RethrowException,
                                     -1);
              task_runtime.set_exception(error);
              done->set_exception(error);
            }
          },
          priority, epoch);
    }
    for (auto& future : futures) {
      future.get();
    }
  };

  submit_and_wait(std::move(source_tasks), SchedulerTaskPriority::High, false);
  if (before_downstream) {
    try {
      before_downstream();
    } catch (...) {
      auto error = std::current_exception();
      task_runtime.log_event(SchedulerTraceAction::RethrowException, -1);
      task_runtime.set_exception(error);
      std::rethrow_exception(error);
    }
  }
  submit_and_wait(std::move(downstream_tasks), SchedulerTaskPriority::Normal,
                  true);
}

/**
 * @brief Executes one high-precision dispatch through the scheduler runtime.
 *
 * @param graph GraphModel whose target output is computed.
 * @param task_runtime Scheduler runtime used for this dispatch.
 * @param request Per-call dispatch options.
 * @param sequential_fallback Synchronous fallback for empty task plans.
 * @return Mutable high-precision output stored on the target graph node.
 * @throws GraphError for missing targets, missing final output, compute
 * failures, or scheduling failures; may also propagate operation/cache/fallback
 * exceptions with added context.
 * @note The function builds all worker closures before submission, waits for
 * completion, then commits temp outputs under graph_mutex_.
 */
NodeOutput& ComputeTaskDispatcher::execute(
    GraphModel& graph, SchedulerTaskRuntime& task_runtime,
    const ComputeDispatchRequest& request,
    SequentialFallback sequential_fallback) {
  const int node_id = request.node_id;
  auto& timing_results = graph.timing_results;
  auto& timing_mutex = graph.timing_mutex_;
  auto& graph_mutex = graph.graph_mutex_;
  if (!graph.has_node(node_id)) {
    throw GraphError(
        GraphErrc::NotFound,
        "Cannot compute: node " + std::to_string(node_id) + " not found.");
  }

  if (request.enable_timing) {
    clear_timing_results(graph);
    graph.total_io_time_ms = 0.0;
  }

  TaskSubmissionPlan plan(graph, traversal_, node_id);
  if (request.force_recache) {
    clear_planned_high_precision_caches(graph, graph_mutex,
                                        plan.execution_order());
  }

  NodeTaskRunner runner(NodeTaskRunnerContext{
      graph,
      cache_,
      events_,
      task_runtime,
      timing_results,
      timing_mutex,
      plan.execution_order(),
      plan.id_to_idx(),
      plan.temp_results(),
      plan.resolved_ops(),
      request.force_recache,
      request.enable_timing,
      request.disable_disk_cache,
      request.benchmark_events,
  });
  plan.build_scheduler_tasks(runner, task_runtime);
  dispatch_or_run_fallback(graph, task_runtime, node_id,
                           request.disable_disk_cache, plan,
                           sequential_fallback);

  ResultCommitter committer(cache_, graph_mutex, request.cache_precision);
  if (request.enable_timing) {
    committer.finalize_timing(timing_results, timing_mutex);
  }
  committer.commit(graph, plan.execution_order(), plan.temp_results());

  if (!graph.node(node_id).cached_output_high_precision) {
    throw GraphError(GraphErrc::ComputeError,
                     "Parallel computation finished but target node has no "
                     "output. An upstream error likely occurred.");
  }
  return *graph.mutable_node(node_id).cached_output_high_precision;
}

}  // namespace ps::compute
