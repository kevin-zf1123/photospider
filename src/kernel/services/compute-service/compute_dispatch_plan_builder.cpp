#include "kernel/services/compute-service/compute_dispatch_plan_builder.hpp"

#include <optional>
#include <vector>

#include "graph_model.hpp"  // NOLINT(build/include_subdir)
#include "kernel/services/graph_traversal_service.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Stores a bounded compute-plan inspection history on the graph.
 *
 * @param graph GraphModel whose inspection fields receive the plan snapshot.
 * @param compute_plan Plan produced for the current dispatch.
 * @throws std::bad_alloc if recent-plan history storage cannot grow.
 * @note The history cap is intentionally small because plans are diagnostic
 * state, not an unbounded runtime log.
 */
void remember_dispatch_compute_plan(GraphModel& graph,
                                    const ComputePlan& compute_plan) {
  graph.last_compute_plan = compute_plan;
  graph.recent_compute_plans.push_back(compute_plan);
  if (graph.recent_compute_plans.size() > 16) {
    graph.recent_compute_plans.erase(graph.recent_compute_plans.begin());
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
  FullTaskGraphExpander full_expander;
  NodeCacheTaskGraphPruner node_cache_pruner;
  const ComputeRequest request{ComputeIntent::GlobalHighPrecision, node_id,
                               true, std::nullopt};
  const FullTaskGraph full_graph = full_expander.expand(graph, request.intent);
  ComputePlan compute_plan =
      node_cache_pruner.prune(full_graph, request, execution_order, graph);
  remember_dispatch_compute_plan(graph, compute_plan);
  return compute_plan;
}

}  // namespace ps::compute
