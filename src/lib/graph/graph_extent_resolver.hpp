#pragma once

#include <unordered_map>

#include "graph/graph_model.hpp"

namespace ps {

/**
 * @brief Resolves dependency-neutral HP output extents from graph state.
 *
 * The resolver first observes a committed HP image, then explicit effective
 * width/height parameters, and finally the first resolvable image parent.
 * Results are memoized in caller-owned request storage.
 *
 * @note The resolver owns no graph, cache, or persistence state. Returned
 * extents describe planning bounds and do not allocate image storage.
 */
class GraphExtentResolver {
 public:
  /**
   * @brief Resolves one node's current HP output extent.
   * @param graph Graph whose node state and image parents are inspected.
   * @param node_id Node whose output extent is requested.
   * @param cache Request-local memoization map updated before returning.
   * @return Positive dependency-neutral extent, or an empty extent when none
   * can be inferred.
   * @throws GraphError when node_id or a traversed parent is missing.
   * @throws std::bad_alloc when memoization or missing-node diagnostics
   * allocate.
   * @note Dimension lookup accepts representable Int64 or exact integral Double
   * values from typed parameter maps. A missing or incompatible request-local
   * value falls back to the static parameter; an absent or nonpositive
   * effective extent then falls through to image-parent resolution without
   * document-format conversion. Callers must keep graph topology and effective
   * parameter state stable for the lifetime of cache.
   */
  PixelSize resolve_output_extent(
      const GraphModel& graph, int node_id,
      std::unordered_map<int, PixelSize>& cache) const;
};

}  // namespace ps
