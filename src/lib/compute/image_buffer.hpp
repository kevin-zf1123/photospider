#pragma once

#include <cstddef>
#include <memory>
#include <opencv2/core.hpp>
#include <vector>

#include "photospider/core/image_buffer.hpp"

namespace ps {

class Node;
struct SpatialContext;

/**
 * @brief Read-only non-owning view over an input image region.
 *
 * InputTile is the private compute representation passed between tile planning,
 * input normalization, backend adapters, and operation-host callbacks. Its
 * OpenCV rectangle is confined to the backend implementation boundary.
 *
 * @throws Nothing for value operations.
 * @note The buffer pointer is const so tiled operator APIs cannot mutate
 *       ImageBuffer metadata or replace the upstream payload. OpenCV cv::Mat
 *       views do not provide hard pixel immutability, so tiled operators must
 *       still treat matrices obtained from InputTile as read-only.
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
  cv::Rect roi;

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
 * adapters and operation-host callbacks. Its OpenCV rectangle never crosses
 * the public plugin contract, which uses backend-neutral geometry values.
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
  cv::Rect roi;
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
