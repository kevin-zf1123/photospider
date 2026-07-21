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
};

}  // namespace ps::testing
