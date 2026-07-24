#pragma once

#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::testing {

/**
 * @brief Private bridge for placing a real Graph at a revision boundary.
 *
 * @note This seam mutates only tests' isolated Graph objects or graph-state
 * serialized test fixtures. It is not compiled into an installed interface
 * and exposes no production reset operation.
 */
class GraphModelTestAccess {
 public:
  /**
   * @brief Replaces the authoritative revision for an exhaustion test.
   * @param graph Isolated Graph whose graph-state lane is held by the caller.
   * @param revision Nonzero test revision to publish.
   * @return Nothing.
   * @throws Nothing.
   * @note Callers use UINT64_MAX only after fixture setup, then verify that
   * production mutation preparation fails before any graph or cache side
   * effect.
   */
  static void set_revision(GraphModel& graph, GraphRevision revision) noexcept {
    graph.revision_ = revision;
  }

  /**
   * @brief Exchanges only two private disk-cache diagnostic stores.
   * @param first Isolated Graph whose diagnostic store participates first.
   * @param second Isolated Graph whose diagnostic store participates second;
   *        it may alias first to exercise the production self-exchange path.
   * @return Nothing.
   * @throws Nothing.
   * @note This inline source-tree seam lets the dedicated concurrency process
   * exercise both address orders without concurrently swapping unrelated
   * GraphModel state. It is never compiled into the installable product.
   */
  static void exchange_disk_cache_diagnostics(GraphModel& first,
                                              GraphModel& second) noexcept {
    first.disk_cache_diagnostics_.exchange_with(second.disk_cache_diagnostics_);
  }
};

}  // namespace ps::testing
