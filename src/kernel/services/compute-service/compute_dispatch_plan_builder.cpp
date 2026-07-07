#include "kernel/services/compute-service/compute_dispatch_plan_builder.hpp"

#include <memory>
#include <optional>
#include <vector>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/services/graph_traversal_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Stores the latest compute plan and bounded summary history.
 *
 * @param graph GraphModel whose inspection fields receive the plan snapshot.
 * @param compute_plan Plan produced for the current dispatch.
 * @throws std::bad_alloc if summary history storage cannot grow.
 * @note Full ComputePlan data is kept only for the latest request; repeated
 * inspection history stores summaries to avoid copying large task graphs.
 */
void remember_dispatch_compute_plan(GraphModel& graph,
                                    const ComputePlan& compute_plan) {
  graph.last_compute_plan = compute_plan;
  graph.last_compute_plan_summary = summarize_compute_plan(graph, compute_plan);
  graph.recent_compute_plan_summaries.push_back(
      *graph.last_compute_plan_summary);
  if (graph.recent_compute_plan_summaries.size() > 16) {
    graph.recent_compute_plan_summaries.erase(
        graph.recent_compute_plan_summaries.begin());
  }
}

}  // namespace

ComputeDispatchPlanBuilder::ComputeDispatchPlanBuilder(
    GraphTraversalService& traversal)
    : traversal_(traversal) {}

ComputePlan ComputeDispatchPlanBuilder::build_high_precision_plan(
    GraphModel& graph, int node_id) const {
  const std::vector<int> execution_order =
      traversal_.topo_postorder_from(graph, node_id);
  NodeCacheTaskGraphPruner node_cache_pruner;
  const ComputeRequest request{ComputeIntent::GlobalHighPrecision, node_id,
                               true, std::nullopt};
  const std::shared_ptr<const FullTaskGraph> full_graph =
      get_or_expand_full_task_graph(graph, request.intent);
  ComputePlan compute_plan =
      node_cache_pruner.prune(*full_graph, request, execution_order, graph);
  remember_dispatch_compute_plan(graph, compute_plan);
  return compute_plan;
}

}  // namespace ps::compute
