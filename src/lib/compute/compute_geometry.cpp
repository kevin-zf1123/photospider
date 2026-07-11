#include "kernel/services/compute-service/compute_geometry.hpp"

#include <algorithm>

namespace ps::compute {

bool is_rect_empty(const cv::Rect& rect) {
  return rect.width <= 0 || rect.height <= 0;
}

cv::Rect clip_rect(const cv::Rect& rect, const cv::Size& bounds) {
  if (bounds.width <= 0 || bounds.height <= 0)
    return cv::Rect();
  const int x0 = std::clamp(rect.x, 0, bounds.width);
  const int y0 = std::clamp(rect.y, 0, bounds.height);
  const int x1 = std::clamp(rect.x + rect.width, 0, bounds.width);
  const int y1 = std::clamp(rect.y + rect.height, 0, bounds.height);
  if (x1 <= x0 || y1 <= y0)
    return cv::Rect();
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

cv::Rect expand_rect(const cv::Rect& rect, int padding) {
  if (padding <= 0 || is_rect_empty(rect))
    return rect;
  const int x0 = rect.x - padding;
  const int y0 = rect.y - padding;
  const int x1 = rect.x + rect.width + padding;
  const int y1 = rect.y + rect.height + padding;
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

cv::Rect align_rect(const cv::Rect& rect, int alignment) {
  if (is_rect_empty(rect) || alignment <= 1)
    return rect;
  const int x0 = (rect.x / alignment) * alignment;
  const int y0 = (rect.y / alignment) * alignment;
  const int x1 =
      ((rect.x + rect.width + alignment - 1) / alignment) * alignment;
  const int y1 =
      ((rect.y + rect.height + alignment - 1) / alignment) * alignment;
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) {
  if (is_rect_empty(a))
    return b;
  if (is_rect_empty(b))
    return a;
  const int x0 = std::min(a.x, b.x);
  const int y0 = std::min(a.y, b.y);
  const int x1 = std::max(a.x + a.width, b.x + b.width);
  const int y1 = std::max(a.y + a.height, b.y + b.height);
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

cv::Size scale_down_size(const cv::Size& size, int factor) {
  if (size.width <= 0 || size.height <= 0 || factor <= 0)
    return cv::Size();
  return cv::Size((size.width + factor - 1) / factor,
                  (size.height + factor - 1) / factor);
}

cv::Rect scale_down_rect(const cv::Rect& rect, int factor) {
  if (is_rect_empty(rect) || factor <= 0)
    return cv::Rect();
  const int x0 = rect.x / factor;
  const int y0 = rect.y / factor;
  const int x1 = (rect.x + rect.width + factor - 1) / factor;
  const int y1 = (rect.y + rect.height + factor - 1) / factor;
  return cv::Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

cv::Rect scale_up_rect(const cv::Rect& rect, int factor) {
  if (is_rect_empty(rect) || factor <= 0)
    return cv::Rect();
  const int x0 = rect.x * factor;
  const int y0 = rect.y * factor;
  const int x1 = (rect.x + rect.width) * factor;
  const int y1 = (rect.y + rect.height) * factor;
  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

cv::Rect calculate_halo(const cv::Rect& roi, int halo_size,
                        const cv::Size& bounds) {
  if (halo_size <= 0)
    return roi;
  const int x = std::max(0, roi.x - halo_size);
  const int y = std::max(0, roi.y - halo_size);
  const int right = std::min(bounds.width, roi.x + roi.width + halo_size);
  const int bottom = std::min(bounds.height, roi.y + roi.height + halo_size);
  return cv::Rect(x, y, right - x, bottom - y);
}

}  // namespace ps::compute
