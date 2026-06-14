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

void finalize_output_metadata(NodeOutput& output,
                              const std::vector<const NodeOutput*>& inputs,
                              bool enable_timing, double execution_ms) {
  ComputeMetricsRecorder::finalize_output_metadata(output, inputs,
                                                   enable_timing, execution_ms);
}

void remember_compute_plan(GraphModel& graph, const ComputePlan& compute_plan) {
  graph.last_compute_plan = compute_plan;
  graph.recent_compute_plans.push_back(compute_plan);
  if (graph.recent_compute_plans.size() > 16) {
    graph.recent_compute_plans.erase(graph.recent_compute_plans.begin());
  }
}

std::string node_context(const GraphModel& graph, int node_id) {
  const Node* node = graph.find_node(node_id);
  if (!node) {
    return "node " + std::to_string(node_id);
  }
  return "node " + std::to_string(node_id) + " (" + node->name + ")";
}

std::exception_ptr compute_failure(const GraphModel& graph, int node_id,
                                   const std::string& detail) {
  return std::make_exception_ptr(
      GraphError(GraphErrc::ComputeError, "Compute stage at " +
                                              node_context(graph, node_id) +
                                              " failed: " + detail));
}

std::exception_ptr scheduling_failure(const GraphModel& graph, int node_id,
                                      const std::string& detail) {
  return std::make_exception_ptr(
      GraphError(GraphErrc::ComputeError, "Scheduling stage after " +
                                              node_context(graph, node_id) +
                                              " failed: " + detail));
}

struct NodeTaskRunnerContext {
  GraphModel& graph;
  GraphCacheService& cache;
  GraphEventService& events;
  SchedulerTaskRuntime& task_runtime;
  TimingCollector& timing_results;
  std::mutex& timing_mutex;
  const std::vector<int>& execution_order;
  const std::unordered_map<int, int>& id_to_idx;
  std::vector<std::optional<NodeOutput>>& temp_results;
  const std::vector<std::optional<OpRegistry::OpVariant>>& resolved_ops;
  bool force_recache = false;
  bool enable_timing = false;
  bool disable_disk_cache = false;
  std::vector<BenchmarkEvent>* benchmark_events = nullptr;
};

// Runs one planned graph node without mutating GraphModel. Outputs stay in the
// per-plan temp slots until ResultCommitter performs the serialized commit.
// The referenced containers are owned by TaskSubmissionPlan for one execute()
// call; NodeTaskRunner must never outlive that plan.
class NodeTaskRunner {
 public:
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
  bool allow_disk_cache() const {
    return !disable_disk_cache_ && !force_recache_;
  }

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

  bool has_memory_or_temp_output(const Node& node, int node_idx) const {
    return node.cached_output_high_precision.has_value() ||
           temp_results_[node_idx].has_value();
  }

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

  BenchmarkEvent start_event(const Node& target_node) const {
    BenchmarkEvent event;
    event.node_id = target_node.id;
    event.op_name = make_key(target_node.type, target_node.subtype);
    event.dependency_start_time = std::chrono::high_resolution_clock::now();
    event.execution_start_time = event.dependency_start_time;
    return event;
  }

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

  GraphModel& graph_;
  GraphCacheService& cache_;
  GraphEventService& events_;
  SchedulerTaskRuntime& task_runtime_;
  TimingCollector& timing_results_;
  std::mutex& timing_mutex_;
  const std::vector<int>& execution_order_;
  const std::unordered_map<int, int>& id_to_idx_;
  std::vector<std::optional<NodeOutput>>& temp_results_;
  const std::vector<std::optional<OpRegistry::OpVariant>>& resolved_ops_;
  bool force_recache_;
  bool enable_timing_;
  bool disable_disk_cache_;
  std::vector<BenchmarkEvent>* benchmark_events_;
};

// Owns the scheduler-facing shape of a single ComputePlan: dense indexes,
// dependency counters, task closures, and temporary result slots.
class TaskSubmissionPlan {
 public:
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

  bool empty() const { return execution_order_.empty(); }

  size_t size() const { return execution_order_.size(); }

  const std::vector<int>& execution_order() const { return execution_order_; }

  const std::unordered_map<int, int>& id_to_idx() const { return id_to_idx_; }

  std::vector<std::optional<NodeOutput>>& temp_results() {
    return temp_results_;
  }

  const std::vector<std::optional<OpRegistry::OpVariant>>& resolved_ops()
      const {
    return resolved_ops_;
  }

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

  void log_initial_assignments(SchedulerTaskRuntime& task_runtime) const {
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      if (submitted_initial_indices_.count(static_cast<int>(i))) {
        task_runtime.log_event(SchedulerTraceAction::AssignInitial,
                               execution_order_[i]);
      }
    }
  }

 private:
  void build_dense_index() {
    id_to_idx_.reserve(execution_order_.size());
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      id_to_idx_[execution_order_[i]] = static_cast<int>(i);
    }
  }

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

  void resolve_operations() {
    resolved_ops_.resize(execution_order_.size());
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      const auto& node = graph_.node(execution_order_[i]);
      resolved_ops_[i] = OpRegistry::instance().resolve_for_intent(
          node.type, node.subtype, ComputeIntent::GlobalHighPrecision);
    }
  }

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

  void append_initial_task_for_node(int node_idx) {
    if (dependency_counters_[node_idx].load(std::memory_order_acquire) != 0) {
      return;
    }
    if (submitted_initial_indices_.insert(node_idx).second) {
      initial_tasks_.push_back(std::move(tasks_[node_idx]));
    }
  }

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

  void append_zero_dependency_tasks() {
    for (size_t i = 0; i < execution_order_.size(); ++i) {
      append_initial_task_for_node(static_cast<int>(i));
    }
  }

  GraphModel& graph_;
  std::vector<int> execution_order_;
  ComputePlan compute_plan_;
  std::unordered_map<int, int> id_to_idx_;
  std::vector<std::atomic<int>> dependency_counters_;
  std::vector<std::vector<int>> dependents_map_;
  std::vector<SchedulerTaskRuntime::Task> tasks_;
  std::vector<SchedulerTaskRuntime::Task> initial_tasks_;
  std::unordered_set<int> submitted_initial_indices_;
  std::vector<std::optional<NodeOutput>> temp_results_;
  std::vector<std::optional<OpRegistry::OpVariant>> resolved_ops_;
};

// Serializes the side effects that worker tasks deliberately avoided: timing
// totals, GraphModel cache mutation, and optional disk cache writes.
class ResultCommitter {
 public:
  ResultCommitter(GraphCacheService& cache, std::mutex& graph_mutex,
                  const std::string& cache_precision)
      : cache_(cache),
        graph_mutex_(graph_mutex),
        cache_precision_(cache_precision) {}

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
  GraphCacheService& cache_;
  std::mutex& graph_mutex_;
  const std::string& cache_precision_;
};

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

ComputeTaskDispatcher::ComputeTaskDispatcher(GraphTraversalService& traversal,
                                             GraphCacheService& cache,
                                             GraphEventService& events)
    : traversal_(traversal), cache_(cache), events_(events) {}

void ComputeTaskDispatcher::clear_timing_results(GraphModel& graph) {
  std::lock_guard<std::mutex> lk(graph.timing_mutex_);
  graph.timing_results.node_timings.clear();
  graph.timing_results.total_ms = 0.0;
}

void ComputeTaskDispatcher::submit_dirty_ready_tasks_source_first(
    SchedulerTaskRuntime& task_runtime,
    std::vector<SchedulerTaskRuntime::Task>&& source_tasks,
    std::vector<SchedulerTaskRuntime::Task>&& downstream_tasks,
    std::optional<uint64_t> epoch, std::function<void()> before_downstream) {
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
