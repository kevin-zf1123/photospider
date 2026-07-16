#pragma once

#include "compute/task_graph_planning.hpp"

namespace ps {
class GraphModel;
class GraphTraversalService;
}  // namespace ps

namespace ps::compute {

/**
 * @brief Builds the cache-pruned high-precision ComputePlan for dispatcher
 * execution.
 *
 * ComputeDispatchPlanBuilder is the request-planning unit used by
 * ComputeTaskDispatcher. It derives the target traversal order, expands the
 * full graph for GlobalHighPrecision, prunes it to the target node/cache cone,
 * and records the resulting plan for graph inspection.
 *
 * @note The builder borrows GraphTraversalService and never stores graph
 * pointers. The returned ComputePlan is immutable scheduler input for the
 * active dispatch.
 */
class ComputeDispatchPlanBuilder {
 public:
  /**
   * @brief Constructs a builder over the traversal service used by the graph.
   *
   * @param traversal Topology service used to derive target postorder.
   * @throws Nothing directly.
   * @note The service reference must outlive the builder call.
   */
  explicit ComputeDispatchPlanBuilder(GraphTraversalService& traversal);

  /**
   * @brief Builds and records one GlobalHighPrecision scheduler plan.
   *
   * @param graph GraphModel containing the target node and runtime cache state.
   * @param node_id Target node id for the compute request.
   * @return Cache-pruned ComputePlan whose planned_nodes define execution
   * order and whose task_graph defines dependencies.
   * @throws GraphError or standard exceptions from traversal, graph expansion,
   * pruning, graph lookup, or diagnostic history allocation.
   * @note The plan is appended to GraphModel inspection history before it is
   * returned so runtime debugging sees exactly the submitted plan.
   */
  ComputePlan build_high_precision_plan(GraphModel& graph, int node_id) const;

 private:
  /** @brief Borrowed topology service used to derive target dependency order.
   */
  GraphTraversalService& traversal_;
};

}  // namespace ps::compute
