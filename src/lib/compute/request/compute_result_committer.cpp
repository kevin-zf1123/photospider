#include "compute/request/compute_result_committer.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "core/value_image_adapter.hpp"
#include "graph/graph_cache_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Resolves one node's frozen authority before graph mutation.
 * @param planned_work Immutable per-node planning records.
 * @param node_id Graph node whose authority is required.
 * @return Borrowed exact output authority.
 * @throws GraphError with ComputeError when the plan omitted the node or its
 * authority.
 * @note The lookup consumes only callback-free planning state and cannot use a
 * provider result to invent authorization.
 */
const PlannedOutputAuthority& authority_for_node(
    const std::vector<PlannedNodeWork>& planned_work, int node_id) {
  const auto found = std::find_if(planned_work.begin(), planned_work.end(),
                                  [node_id](const PlannedNodeWork& work) {
                                    return work.node_id == node_id;
                                  });
  if (found == planned_work.end() || !found->output_authority.has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Formal commit is missing frozen output authority.");
  }
  return *found->output_authority;
}

}  // namespace

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
    const std::vector<PlannedNodeWork>& planned_work,
    std::vector<std::optional<NodeOutput>>& temp_results) const {
  if (temp_results.size() < execution_order.size()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Formal commit received incomplete result storage.");
  }
  std::vector<std::optional<RegionSet>> full_regions(temp_results.size());
  for (size_t i = 0; i < execution_order.size(); ++i) {
    if (temp_results[i].has_value()) {
      validate_planned_output(
          *temp_results[i],
          authority_for_node(planned_work, execution_order[i]),
          PlannedOutputReadiness::RequireReady);
      full_regions[i] =
          value_image_adapter::full_node_output_region(*temp_results[i]);
    }
  }

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
          state.hp_region = std::move(full_regions[i]);
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
          state.hp_region.reset();
        });
  }
}

}  // namespace ps::compute
