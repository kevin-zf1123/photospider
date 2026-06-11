#pragma once

#include <functional>
#include <string>
#include <vector>

#include "benchmark/benchmark_types.hpp"
#include "graph_model.hpp"

namespace ps {
class GraphRuntime;
class GraphTraversalService;
class GraphCacheService;
class GraphEventService;
}  // namespace ps

namespace ps::compute {

class ParallelGraphExecutor {
 public:
  using SequentialFallback =
      std::function<NodeOutput&(GraphModel&, int, bool allow_disk_cache)>;

  ParallelGraphExecutor(GraphTraversalService& traversal,
                        GraphCacheService& cache, GraphEventService& events);

  NodeOutput& execute(GraphModel& graph, GraphRuntime& runtime, int node_id,
                      const std::string& cache_precision, bool force_recache,
                      bool enable_timing, bool disable_disk_cache,
                      std::vector<BenchmarkEvent>* benchmark_events,
                      SequentialFallback sequential_fallback);

 private:
  static void clear_timing_results(GraphModel& graph);

  GraphTraversalService& traversal_;
  GraphCacheService& cache_;
  GraphEventService& events_;
};

}  // namespace ps::compute
