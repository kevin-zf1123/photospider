#include "compute/compute_geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ps::compute {
namespace {

/**
 * @brief Constructs a cv::Rect only from a representable half-open range.
 * @param x0 Inclusive left coordinate.
 * @param y0 Inclusive top coordinate.
 * @param x1 Exclusive right coordinate.
 * @param y1 Exclusive bottom coordinate.
 * @return Representable positive-area rectangle, otherwise empty.
 * @throws Nothing.
 * @note All geometry helpers use this final checked narrowing boundary.
 */
cv::Rect checked_rect(std::int64_t x0, std::int64_t y0, std::int64_t x1,
                      std::int64_t y1) noexcept {
  if (x1 <= x0 || y1 <= y0 || x0 < std::numeric_limits<int>::min() ||
      y0 < std::numeric_limits<int>::min() ||
      x1 > std::numeric_limits<int>::max() ||
      y1 > std::numeric_limits<int>::max() ||
      x1 - x0 > std::numeric_limits<int>::max() ||
      y1 - y0 > std::numeric_limits<int>::max()) {
    return cv::Rect();
  }
  return cv::Rect(static_cast<int>(x0), static_cast<int>(y0),
                  static_cast<int>(x1 - x0), static_cast<int>(y1 - y0));
}

}  // namespace

/** @copydoc is_rect_empty */
bool is_rect_empty(const cv::Rect& rect) noexcept {
  return rect.width <= 0 || rect.height <= 0;
}

/** @copydoc clip_rect */
cv::Rect clip_rect(const cv::Rect& rect, const cv::Size& bounds) noexcept {
  if (bounds.width <= 0 || bounds.height <= 0 || is_rect_empty(rect)) {
    return cv::Rect();
  }
  const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  return checked_rect(std::clamp<std::int64_t>(rect.x, 0, bounds.width),
                      std::clamp<std::int64_t>(rect.y, 0, bounds.height),
                      std::clamp<std::int64_t>(right, 0, bounds.width),
                      std::clamp<std::int64_t>(bottom, 0, bounds.height));
}

/** @copydoc expand_rect */
cv::Rect expand_rect(const cv::Rect& rect, int padding) noexcept {
  if (padding <= 0 || is_rect_empty(rect)) {
    return rect;
  }
  return checked_rect(
      static_cast<std::int64_t>(rect.x) - padding,
      static_cast<std::int64_t>(rect.y) - padding,
      static_cast<std::int64_t>(rect.x) + rect.width + padding,
      static_cast<std::int64_t>(rect.y) + rect.height + padding);
}

/** @copydoc align_rect */
cv::Rect align_rect(const cv::Rect& rect, int alignment) noexcept {
  if (is_rect_empty(rect) || alignment <= 1) {
    return rect;
  }
  const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  const std::int64_t x0 =
      (static_cast<std::int64_t>(rect.x) / alignment) * alignment;
  const std::int64_t y0 =
      (static_cast<std::int64_t>(rect.y) / alignment) * alignment;
  const std::int64_t x1 = ((right + alignment - 1) / alignment) * alignment;
  const std::int64_t y1 = ((bottom + alignment - 1) / alignment) * alignment;
  return checked_rect(x0, y0, x1, y1);
}

/** @copydoc merge_rect */
cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) noexcept {
  if (is_rect_empty(a)) {
    return b;
  }
  if (is_rect_empty(b)) {
    return a;
  }
  return checked_rect(std::min<std::int64_t>(a.x, b.x),
                      std::min<std::int64_t>(a.y, b.y),
                      std::max(static_cast<std::int64_t>(a.x) + a.width,
                               static_cast<std::int64_t>(b.x) + b.width),
                      std::max(static_cast<std::int64_t>(a.y) + a.height,
                               static_cast<std::int64_t>(b.y) + b.height));
}

/** @copydoc scale_down_size */
cv::Size scale_down_size(const cv::Size& size, int factor) noexcept {
  if (size.width <= 0 || size.height <= 0 || factor <= 0) {
    return cv::Size();
  }
  return cv::Size(
      static_cast<int>((static_cast<std::int64_t>(size.width) + factor - 1) /
                       factor),
      static_cast<int>((static_cast<std::int64_t>(size.height) + factor - 1) /
                       factor));
}

/** @copydoc scale_down_rect */
cv::Rect scale_down_rect(const cv::Rect& rect, int factor) noexcept {
  if (is_rect_empty(rect) || factor <= 0) {
    return cv::Rect();
  }
  const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  return checked_rect(static_cast<std::int64_t>(rect.x) / factor,
                      static_cast<std::int64_t>(rect.y) / factor,
                      (right + factor - 1) / factor,
                      (bottom + factor - 1) / factor);
}

/** @copydoc scale_up_rect */
cv::Rect scale_up_rect(const cv::Rect& rect, int factor) noexcept {
  if (is_rect_empty(rect) || factor <= 0) {
    return cv::Rect();
  }
  return checked_rect(
      static_cast<std::int64_t>(rect.x) * factor,
      static_cast<std::int64_t>(rect.y) * factor,
      (static_cast<std::int64_t>(rect.x) + rect.width) * factor,
      (static_cast<std::int64_t>(rect.y) + rect.height) * factor);
}

/** @copydoc calculate_halo */
cv::Rect calculate_halo(const cv::Rect& roi, int halo_size,
                        const cv::Size& bounds) noexcept {
  if (halo_size <= 0) {
    return clip_rect(roi, bounds);
  }
  return clip_rect(expand_rect(roi, halo_size), bounds);
}

}  // namespace ps::compute
