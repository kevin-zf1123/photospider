#include "compute/compute_geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ps::compute {

/** @copydoc rect_from_edges */
PixelRect rect_from_edges(std::int64_t x0, std::int64_t y0, std::int64_t x1,
                          std::int64_t y1) noexcept {
  if (x1 <= x0 || y1 <= y0 || x0 < std::numeric_limits<int>::min() ||
      y0 < std::numeric_limits<int>::min() ||
      x1 > std::numeric_limits<int>::max() ||
      y1 > std::numeric_limits<int>::max() ||
      x1 - x0 > std::numeric_limits<int>::max() ||
      y1 - y0 > std::numeric_limits<int>::max()) {
    return PixelRect{};
  }
  return PixelRect{static_cast<int>(x0), static_cast<int>(y0),
                   static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
}

/** @copydoc is_rect_empty */
bool is_rect_empty(const PixelRect& rect) noexcept {
  return rect.width <= 0 || rect.height <= 0;
}

/** @copydoc clip_rect */
PixelRect clip_rect(const PixelRect& rect, const PixelSize& bounds) noexcept {
  if (bounds.width <= 0 || bounds.height <= 0 || is_rect_empty(rect)) {
    return PixelRect{};
  }
  const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  return rect_from_edges(std::clamp<std::int64_t>(rect.x, 0, bounds.width),
                         std::clamp<std::int64_t>(rect.y, 0, bounds.height),
                         std::clamp<std::int64_t>(right, 0, bounds.width),
                         std::clamp<std::int64_t>(bottom, 0, bounds.height));
}

/** @copydoc intersect_rect */
PixelRect intersect_rect(const PixelRect& lhs, const PixelRect& rhs) noexcept {
  if (is_rect_empty(lhs) || is_rect_empty(rhs)) {
    return PixelRect{};
  }
  return rect_from_edges(
      std::max<std::int64_t>(lhs.x, rhs.x),
      std::max<std::int64_t>(lhs.y, rhs.y),
      std::min(static_cast<std::int64_t>(lhs.x) + lhs.width,
               static_cast<std::int64_t>(rhs.x) + rhs.width),
      std::min(static_cast<std::int64_t>(lhs.y) + lhs.height,
               static_cast<std::int64_t>(rhs.y) + rhs.height));
}

/** @copydoc translate_rect */
PixelRect translate_rect(const PixelRect& rect, std::int64_t dx,
                         std::int64_t dy) noexcept {
  if (is_rect_empty(rect)) {
    return PixelRect{};
  }
  constexpr std::int64_t kMinCoordinate = std::numeric_limits<int>::min();
  constexpr std::int64_t kMaxCoordinate = std::numeric_limits<int>::max();
  if (dx < kMinCoordinate - rect.x || dx > kMaxCoordinate - rect.x ||
      dy < kMinCoordinate - rect.y || dy > kMaxCoordinate - rect.y) {
    return PixelRect{};
  }
  const std::int64_t x0 = static_cast<std::int64_t>(rect.x) + dx;
  const std::int64_t y0 = static_cast<std::int64_t>(rect.y) + dy;
  return rect_from_edges(x0, y0, x0 + rect.width, y0 + rect.height);
}

/** @copydoc expand_rect */
PixelRect expand_rect(const PixelRect& rect, int padding) noexcept {
  if (padding <= 0 || is_rect_empty(rect)) {
    return rect;
  }
  return rect_from_edges(
      static_cast<std::int64_t>(rect.x) - padding,
      static_cast<std::int64_t>(rect.y) - padding,
      static_cast<std::int64_t>(rect.x) + rect.width + padding,
      static_cast<std::int64_t>(rect.y) + rect.height + padding);
}

/** @copydoc align_rect */
PixelRect align_rect(const PixelRect& rect, int alignment) noexcept {
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
  return rect_from_edges(x0, y0, x1, y1);
}

/** @copydoc merge_rect */
PixelRect merge_rect(const PixelRect& a, const PixelRect& b) noexcept {
  if (is_rect_empty(a)) {
    return b;
  }
  if (is_rect_empty(b)) {
    return a;
  }
  return rect_from_edges(std::min<std::int64_t>(a.x, b.x),
                         std::min<std::int64_t>(a.y, b.y),
                         std::max(static_cast<std::int64_t>(a.x) + a.width,
                                  static_cast<std::int64_t>(b.x) + b.width),
                         std::max(static_cast<std::int64_t>(a.y) + a.height,
                                  static_cast<std::int64_t>(b.y) + b.height));
}

/** @copydoc scale_down_size */
PixelSize scale_down_size(const PixelSize& size, int factor) noexcept {
  if (size.width <= 0 || size.height <= 0 || factor <= 0) {
    return PixelSize{};
  }
  return PixelSize{
      static_cast<int>((static_cast<std::int64_t>(size.width) + factor - 1) /
                       factor),
      static_cast<int>((static_cast<std::int64_t>(size.height) + factor - 1) /
                       factor)};
}

/** @copydoc scale_down_rect */
PixelRect scale_down_rect(const PixelRect& rect, int factor) noexcept {
  if (is_rect_empty(rect) || factor <= 0) {
    return PixelRect{};
  }
  const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  return rect_from_edges(static_cast<std::int64_t>(rect.x) / factor,
                         static_cast<std::int64_t>(rect.y) / factor,
                         (right + factor - 1) / factor,
                         (bottom + factor - 1) / factor);
}

/** @copydoc scale_up_rect */
PixelRect scale_up_rect(const PixelRect& rect, int factor) noexcept {
  if (is_rect_empty(rect) || factor <= 0) {
    return PixelRect{};
  }
  return rect_from_edges(
      static_cast<std::int64_t>(rect.x) * factor,
      static_cast<std::int64_t>(rect.y) * factor,
      (static_cast<std::int64_t>(rect.x) + rect.width) * factor,
      (static_cast<std::int64_t>(rect.y) + rect.height) * factor);
}

/** @copydoc calculate_halo */
PixelRect calculate_halo(const PixelRect& roi, int halo_size,
                         const PixelSize& bounds) noexcept {
  if (halo_size <= 0) {
    return clip_rect(roi, bounds);
  }
  return clip_rect(expand_rect(roi, halo_size), bounds);
}

}  // namespace ps::compute
