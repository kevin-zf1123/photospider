#pragma once

#include <opencv2/core.hpp>

namespace ps::compute {

constexpr int kRtDownscaleFactor = 4;
constexpr int kRtTileSize = 16;
constexpr int kHpAlignment = kRtDownscaleFactor * kRtTileSize;
constexpr int kHpMacroTileSize = 256;
constexpr int kHpMicroTileSize = 64;

bool is_rect_empty(const cv::Rect& rect);
cv::Rect clip_rect(const cv::Rect& rect, const cv::Size& bounds);
cv::Rect expand_rect(const cv::Rect& rect, int padding);
cv::Rect align_rect(const cv::Rect& rect, int alignment);
cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b);
cv::Size scale_down_size(const cv::Size& size, int factor);
cv::Rect scale_down_rect(const cv::Rect& rect, int factor);
cv::Rect scale_up_rect(const cv::Rect& rect, int factor);
cv::Rect calculate_halo(const cv::Rect& roi, int halo_size,
                        const cv::Size& bounds);

}  // namespace ps::compute
