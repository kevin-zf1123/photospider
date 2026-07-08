#pragma once

/**
 * @file geometry.hpp
 * @brief Stable pixel geometry values for public Photospider contracts.
 *
 * Geometry values in this header intentionally avoid OpenCV and other adapter
 * types so host, IPC, and frontend consumers can describe image extents and
 * regions without depending on implementation libraries.
 */

namespace ps {

/**
 * @brief Integer pixel rectangle independent of implementation ownership.
 *
 * @throws Nothing.
 * @note Empty rectangles have non-positive width or height and should be
 *       ignored by consumers that require a real pixel area.
 */
struct PixelRect {
  /** @brief Left coordinate in pixels. */
  int x = 0;

  /** @brief Top coordinate in pixels. */
  int y = 0;

  /** @brief Rectangle width in pixels. */
  int width = 0;

  /** @brief Rectangle height in pixels. */
  int height = 0;
};

/**
 * @brief Integer two-dimensional extent.
 *
 * @throws Nothing.
 * @note Non-positive dimensions represent an unknown or empty extent.
 */
struct PixelSize {
  /** @brief Width in pixels. */
  int width = 0;

  /** @brief Height in pixels. */
  int height = 0;
};

}  // namespace ps
