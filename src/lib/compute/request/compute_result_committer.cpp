#include "compute/request/compute_result_committer.hpp"

#include <string>
#include <utility>
#include <vector>

#include "core/value_image_adapter.hpp"
#include "graph/graph_cache_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Validates one private output before formal graph-cache publication.
 * @param output Candidate request-local output.
 * @return Nothing after every named Value is valid and Ready.
 * @throws GraphError with ComputeError for compatibility staging, an invalid
 * named Value, or a non-Ready producer fence.
 * @throws std::logic_error when a retained fence observer is invalid.
 * @note Validation performs no wait, allocation, compatibility import, graph
 * mutation, revision minting, or cache publication.
 */
void validate_formal_output(const NodeOutput& output) {
  if (output.has_compatibility_image()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "Formal commit rejects compatibility ImageBuffer staging.");
  }
  for (const auto& [name, value] : output.named_values) {
    if (name.empty() || !value.valid()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Formal commit received an invalid named Value.");
    }
    if (!value.ready_fence().poll().ready()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Formal commit requires every named Value to be Ready.");
    }
  }
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
    std::vector<std::optional<NodeOutput>>& temp_results) const {
  std::vector<std::optional<RegionSet>> full_regions(temp_results.size());
  for (size_t i = 0; i < execution_order.size(); ++i) {
    if (temp_results[i].has_value()) {
      validate_formal_output(*temp_results[i]);
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
