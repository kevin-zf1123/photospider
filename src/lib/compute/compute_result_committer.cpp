#include "kernel/services/compute-service/compute_result_committer.hpp"

#include <string>
#include <utility>
#include <vector>

#include "kernel/services/graph_cache_service.hpp"

namespace ps::compute {

ComputeResultCommitter::ComputeResultCommitter(
    GraphCacheService& cache, std::mutex& graph_mutex,
    const std::string& cache_precision)
    : cache_(cache),
      graph_mutex_(graph_mutex),
      cache_precision_(cache_precision) {
}  // NOLINT(whitespace/indent_namespace)

void ComputeResultCommitter::finalize_timing(TimingCollector& timing_results,
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

void ComputeResultCommitter::commit(
    GraphModel& graph, const std::vector<int>& execution_order,
    std::vector<std::optional<NodeOutput>>& temp_results) const {
  std::scoped_lock lock(graph_mutex_);
  for (size_t i = 0; i < execution_order.size(); ++i) {
    if (!temp_results[i].has_value()) {
      continue;
    }
    const int node_id = execution_order[i];
    graph.mutate_node_runtime_state(
        node_id, [&](GraphModel::NodeRuntimeState& state) {
          state.cached_output_high_precision = std::move(*temp_results[i]);
          state.hp_version++;
        });
    cache_.save_cache_if_configured(graph, graph.node(node_id),
                                    cache_precision_);
  }
}

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

}  // namespace ps::compute
