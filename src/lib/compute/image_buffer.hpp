#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "core/host_output_authorization.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/geometry.hpp"
#include "photospider/core/image_buffer.hpp"

namespace ps {

class Node;
struct SpatialContext;

/**
 * @brief Read-only non-owning view over an input image region.
 *
 * InputTile is the private compute representation passed between tile planning,
 * input normalization, backend adapters, and operation-host callbacks. Its
 * ROI uses the same dependency-neutral geometry as the public operation
 * contract.
 *
 * @throws Nothing for value operations.
 * @note The buffer pointer is const so tiled operator APIs cannot mutate
 *       ImageBuffer metadata or replace the upstream payload. Backend views
 *       may not provide hard pixel immutability, so tiled operators must still
 *       treat views obtained from InputTile as read-only.
 */
struct InputTile {
  /**
   * @brief Borrowed upstream buffer that must remain alive during tile work.
   * @note Null represents a disconnected destination input slot; the enclosing
   * input-tile vector retains that slot's graph index.
   */
  const ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside buffer, clipped by the executor before dispatch.
   */
  PixelRect roi;

  /**
   * @brief Borrowed immutable spatial metadata for the upstream output.
   *
   * @note The pointer has the same callback lifetime as buffer. It may be null
   * when the producer has no spatial metadata or a focused caller constructs a
   * geometry-only tile.
   */
  const SpatialContext* spatial = nullptr;

  /**
   * @brief Borrowed canonical immutable Value for operation-ABI projection.
   *
   * @note The pointer is null for geometry-only focused callers. Production
   * NodeExecutor tiles point at the exact normalized input Value that outlives
   * dispatch, allowing descriptor identities, versions, and digests to remain
   * independent from the callback-local ImageBuffer compatibility snapshot.
   */
  const Value* value = nullptr;
};

/**
 * @brief Borrowed checked Host write capability for an output image region.
 *
 * OutputTile is the private compute write representation passed to backend
 * adapters and operation-host callbacks. Its immutable plan fixes metadata and
 * ownership while its active grant is the sole mutable authority. The ROI is a
 * zero-based storage-relative projection of the grant's exact logical image
 * Region: adapters must add the plan data-window origin before comparing it to
 * grant.image_region(), while pixel/stride access keeps the zero-based value.
 *
 * @throws Nothing for value operations.
 * @note Neither pointer is owning. Both remain valid only during one trusted
 * callback. Source-private adapters may create a callback-local ImageBuffer
 * alias, but no mutable pointer or owner may survive grant retirement.
 */
struct OutputTile {
  /** @brief Borrowed immutable output plan. */
  const DenseImageOutputPlan* plan = nullptr;

  /** @brief Borrowed active tile grant providing checked mutable spans. */
  HostOutputWriteGrant* grant = nullptr;

  /**
   * @brief Zero-based storage ROI matching the origin-adjusted logical grant.
   * @note This value is clipped to plan width/height before dispatch and must
   * never be retained as logical validity metadata without checked translation
   * through plan.image_facet().data_window.
   */
  PixelRect roi;
};

/**
 * @brief Execution-visible unit of tiled node work.
 *
 * TileTask binds one node, one checked output grant, and all read-only input
 * compatibility tiles required by the selected operator. The task owns no
 * image memory or authority; all pointers are borrowed for the callback.
 *
 * @throws Nothing for value operations except vector allocation on mutation.
 * @note Input tiles may point to normalized temporary NodeOutput storage owned
 * by the TiledInputContext for the surrounding node execution.
 */
struct TileTask {
  /** @brief Node whose tiled operator is being invoked. */
  const Node* node = nullptr;

  /** @brief Writable output region for this task. */
  OutputTile output_tile;

  /**
   * @brief Read-only input regions, including disconnected slot placeholders
   * and halo where required.
   */
  std::vector<InputTile> input_tiles;
};

}  // namespace ps
