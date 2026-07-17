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

  /**
   * @brief Compares every pixel-coordinate field.
   * @param other Rectangle to compare.
   * @return True when origin and extent are identical.
   * @throws Nothing.
   * @note Empty rectangles retain their origin for value equality.
   */
  bool operator==(const PixelRect& other) const noexcept {
    return x == other.x && y == other.y && width == other.width &&
           height == other.height;
  }

  /**
   * @brief Checks whether any pixel-coordinate field differs.
   * @param other Rectangle to compare.
   * @return Negation of operator==.
   * @throws Nothing.
   */
  bool operator!=(const PixelRect& other) const noexcept {
    return !(*this == other);
  }
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

  /**
   * @brief Compares both extent dimensions.
   * @param other Extent to compare.
   * @return True when width and height are identical.
   * @throws Nothing.
   */
  bool operator==(const PixelSize& other) const noexcept {
    return width == other.width && height == other.height;
  }

  /**
   * @brief Checks whether either extent dimension differs.
   * @param other Extent to compare.
   * @return Negation of operator==.
   * @throws Nothing.
   */
  bool operator!=(const PixelSize& other) const noexcept {
    return !(*this == other);
  }
};

}  // namespace ps
