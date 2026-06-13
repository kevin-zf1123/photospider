#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "graph_model.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace ps {
class GraphTraversalService;
class GraphCacheService;
class GraphEventService;
}  // namespace ps

namespace ps::compute {

// Dispatches node/cache-pruned ComputeTaskGraph semantics by materializing
// scheduler tasks, releasing ready work, and committing results. Scheduling
// policy stays in the configured SchedulerTaskRuntime.
class ComputeTaskDispatcher {
 public:
  using SequentialFallback =
      std::function<NodeOutput&(GraphModel&, int, bool allow_disk_cache)>;

  ComputeTaskDispatcher(GraphTraversalService& traversal,
                        GraphCacheService& cache, GraphEventService& events);

  NodeOutput& execute(GraphModel& graph, SchedulerTaskRuntime& task_runtime,
                      int node_id, const std::string& cache_precision,
                      bool force_recache, bool enable_timing,
                      bool disable_disk_cache,
                      std::vector<BenchmarkEvent>* benchmark_events,
                      SequentialFallback sequential_fallback);

  static void submit_dirty_ready_tasks_source_first(
      SchedulerTaskRuntime& task_runtime,
      std::vector<SchedulerTaskRuntime::Task>&& source_tasks,
      std::vector<SchedulerTaskRuntime::Task>&& downstream_tasks,
      std::optional<uint64_t> epoch = std::nullopt,
      std::function<void()> before_downstream = nullptr);

 private:
  static void clear_timing_results(GraphModel& graph);

  GraphTraversalService& traversal_;
  GraphCacheService& cache_;
  GraphEventService& events_;
};

}  // namespace ps::compute
