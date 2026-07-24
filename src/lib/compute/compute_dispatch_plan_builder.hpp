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
 * full graph for GlobalHighPrecision and prunes it to the target
 * node/cache cone. Admission-aware callers may defer graph inspection
 * publication until after lifecycle installation.
 *
 * @note The builder borrows GraphTraversalService and never stores graph
 * pointers. The returned ComputePlan is immutable execution input for the
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
   * @brief Builds one GlobalHighPrecision execution plan.
   *
   * @param graph GraphModel containing the target node and runtime cache state.
   * @param node_id Target node id for the compute request.
   * @param publish_inspection Whether to publish the plan to GraphModel
   * inspection fields before returning.
   * @return Cache-pruned ComputePlan whose planned_nodes define execution
   * order and whose task_graph defines dependencies.
   * @throws GraphError or standard exceptions from traversal, graph expansion,
   * pruning, graph lookup, or diagnostic history allocation.
   * @note Admission-aware callers pass false, retain the returned plan, and
   * call publish_plan_inspection() only after registry installation.
   */
  ComputePlan build_high_precision_plan(GraphModel& graph, int node_id,
                                        bool publish_inspection = true) const;

  /**
   * @brief Publishes one already-built plan to bounded graph inspection state.
   * @param graph Graph receiving the latest plan and summary history.
   * @param compute_plan Immutable plan built for the installed request.
   * @return Nothing.
   * @throws std::bad_alloc when plan or summary history storage grows.
   * @note Admission-aware dispatch calls this after lifecycle installation and
   * before physical ready publication.
   */
  static void publish_plan_inspection(GraphModel& graph,
                                      const ComputePlan& compute_plan);

 private:
  /** @brief Borrowed topology service used to derive target dependency order.
   */
  GraphTraversalService& traversal_;
};

}  // namespace ps::compute
