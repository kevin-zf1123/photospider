#pragma once

#include <cstddef>
#include <memory>
#include <vector>

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
};

/**
 * @brief Writable non-owning view over an output image region.
 *
 * OutputTile is the private compute write representation passed to backend
 * adapters and operation-host callbacks. Its ROI is a backend-neutral value;
 * an OpenCV provider may translate it only when creating a local matrix view.
 *
 * @throws Nothing for value operations.
 * @note The buffer pointer is mutable because output tiles are the only tile
 *       views that may write pixels or update the destination buffer contents.
 */
struct OutputTile {
  /** @brief Borrowed destination buffer that receives tile output. */
  ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside buffer, clipped by the executor before dispatch.
   */
  PixelRect roi;
};

/**
 * @brief Scheduler-visible unit of tiled node work.
 *
 * TileTask binds one node, one writable output tile, and all read-only input
 * tiles required by the selected operator. The task owns no image memory; all
 * buffer pointers are borrowed for the duration of the tiled operator callback.
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
