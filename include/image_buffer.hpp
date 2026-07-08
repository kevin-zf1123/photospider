#pragma once

#include <cstddef>
#include <memory>
#include <opencv2/core.hpp>
#include <vector>

#include "photospider/core/image_buffer.hpp"

namespace ps {

class Node;

/**
 * @brief Read-only non-owning view over an input image region.
 *
 * InputTile is the legacy internal tiled-operator view used by existing
 * adapters and compute executors. It keeps the historical `cv::Rect` ROI so the
 * implementation path remains buildable while the public core seam exposes
 * OpenCV-free `InputTileView` values.
 *
 * @throws Nothing for value operations.
 * @note The buffer pointer is const so tiled operator APIs cannot mutate
 *       ImageBuffer metadata or replace the upstream payload. OpenCV cv::Mat
 *       views do not provide hard pixel immutability, so tiled operators must
 *       still treat matrices obtained from InputTile as read-only.
 */
struct InputTile {
  /** @brief Borrowed upstream buffer that must remain alive during tile work. */
  const ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside buffer, clipped by the executor before dispatch. */
  cv::Rect roi;
};

/**
 * @brief Writable non-owning view over an output image region.
 *
 * OutputTile is the legacy internal tiled-operator write view. It preserves the
 * existing OpenCV rectangle contract for implementation code and adapters while
 * the public core seam keeps backend-neutral tile values separate.
 *
 * @throws Nothing for value operations.
 * @note The buffer pointer is mutable because output tiles are the only tile
 *       views that may write pixels or update the destination buffer contents.
 */
struct OutputTile {
  /** @brief Borrowed destination buffer that receives tile output. */
  ImageBuffer* buffer = nullptr;

  /** @brief Pixel ROI inside buffer, clipped by the executor before dispatch. */
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

  /** @brief Read-only input regions, including halo where required. */
  std::vector<InputTile> input_tiles;
};

}  // namespace ps
