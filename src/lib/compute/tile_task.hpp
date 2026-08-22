#pragma once

#include <vector>

#include "core/host_output_authorization.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/geometry.hpp"
#include "photospider/data/value.hpp"

namespace ps {

class Node;
struct SpatialContext;

/**
 * @brief Read-only non-owning view over one immutable input image region.
 * @throws Nothing for value operations.
 * @note The Value pointer and spatial metadata are borrowed for exactly one
 *       synchronous tile callback. Pixel access requires an explicit
 *       ImageView or provider adapter; no compatibility descriptor exists.
 */
struct InputTile final {
  /**
   * @brief Borrowed canonical immutable image Value.
   * @note Null represents a disconnected destination input slot.
   */
  const Value* value = nullptr;

  /** @brief Zero-based storage ROI clipped by the executor before dispatch. */
  PixelRect roi;

  /**
   * @brief Borrowed immutable spatial metadata for the upstream output.
   * @note Null is valid for geometry-only focused callers.
   */
  const SpatialContext* spatial = nullptr;
};

/**
 * @brief Borrowed checked Host write capability for an output image region.
 * @throws Nothing for value operations.
 * @note The plan fixes descriptor/Facet/Layout and the active grant is the sole
 *       mutable authority. All pointers expire with the callback.
 */
struct OutputTile final {
  /** @brief Borrowed immutable output plan. */
  const DenseImageOutputPlan* plan = nullptr;

  /** @brief Borrowed active tile grant providing checked mutable spans. */
  HostOutputWriteGrant* grant = nullptr;

  /** @brief Zero-based storage ROI matching the logical grant. */
  PixelRect roi;
};

/**
 * @brief Execution-visible unit of tiled node work.
 * @throws std::bad_alloc when input tile storage allocation fails.
 * @note The task owns no payload, graph state, or write authority.
 */
struct TileTask final {
  /** @brief Node whose tiled operator is invoked. */
  const Node* node = nullptr;

  /** @brief Writable output region for this task. */
  OutputTile output_tile;

  /** @brief Ordered read-only inputs including disconnected placeholders. */
  std::vector<InputTile> input_tiles;
};

}  // namespace ps
