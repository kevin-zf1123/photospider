#pragma once

#include <opencv2/core.hpp>

namespace ps::compute {

/** @brief Fixed real-time downscale factor between HP and RT geometry. */
constexpr int kRtDownscaleFactor = 4;
/** @brief Fixed real-time tile edge in pixels. */
constexpr int kRtTileSize = 16;
/** @brief HP alignment matching one RT tile after downscale. */
constexpr int kHpAlignment = kRtDownscaleFactor * kRtTileSize;
/** @brief Preferred HP macro-tile edge in pixels. */
constexpr int kHpMacroTileSize = 256;
/** @brief Preferred HP micro-tile edge in pixels. */
constexpr int kHpMicroTileSize = 64;

/**
 * @brief Checks whether a rectangle has no positive area.
 * @param rect Rectangle to inspect without endpoint arithmetic.
 * @return True when width or height is non-positive.
 * @throws Nothing.
 * @note Safe for arbitrary signed-int field values.
 */
bool is_rect_empty(const cv::Rect& rect) noexcept;

/**
 * @brief Clips one rectangle to positive image bounds using wide arithmetic.
 * @param rect Candidate rectangle, including plugin-provided geometry.
 * @param bounds Positive image extent.
 * @return In-bounds intersection or an empty rectangle.
 * @throws Nothing.
 * @note Signed-int endpoint overflow is impossible because addition precedes
 * narrowing in signed 64-bit space.
 */
cv::Rect clip_rect(const cv::Rect& rect, const cv::Size& bounds) noexcept;

/**
 * @brief Expands all rectangle sides by non-negative padding safely.
 * @param rect Source rectangle.
 * @param padding Pixels added to every side; non-positive values return rect.
 * @return Expanded rectangle, or empty when result is not int-representable.
 * @throws Nothing.
 * @note Callers normally clip the result to an image extent afterward.
 */
cv::Rect expand_rect(const cv::Rect& rect, int padding) noexcept;

/**
 * @brief Aligns rectangle edges outward to one positive pixel grid.
 * @param rect Source rectangle.
 * @param alignment Grid step; values at most one return rect unchanged.
 * @return Aligned rectangle, or empty when result is not int-representable.
 * @throws Nothing.
 * @note Arithmetic uses signed 64-bit intermediates.
 */
cv::Rect align_rect(const cv::Rect& rect, int alignment) noexcept;

/**
 * @brief Computes a safe bounding union of two rectangles.
 * @param a First rectangle or empty identity.
 * @param b Second rectangle or empty identity.
 * @return Bounding union, or empty when it is not int-representable.
 * @throws Nothing.
 * @note Endpoint sums are evaluated in signed 64-bit space.
 */
cv::Rect merge_rect(const cv::Rect& a, const cv::Rect& b) noexcept;

/**
 * @brief Ceil-divides a positive extent by a positive factor.
 * @param size Source extent.
 * @param factor Positive downscale divisor.
 * @return Downscaled extent, or empty for invalid inputs.
 * @throws Nothing.
 * @note Pre-division addition uses signed 64-bit arithmetic.
 */
cv::Size scale_down_size(const cv::Size& size, int factor) noexcept;

/**
 * @brief Ceil-divides rectangle endpoints by a positive factor.
 * @param rect Source rectangle.
 * @param factor Positive downscale divisor.
 * @return Downscaled rectangle, or empty for invalid/unrepresentable input.
 * @throws Nothing.
 * @note Endpoint arithmetic uses signed 64-bit intermediates.
 */
cv::Rect scale_down_rect(const cv::Rect& rect, int factor) noexcept;

/**
 * @brief Multiplies rectangle endpoints by a positive factor safely.
 * @param rect Source rectangle.
 * @param factor Positive upscale multiplier.
 * @return Upscaled rectangle, or empty for invalid/unrepresentable input.
 * @throws Nothing.
 * @note No narrowing occurs until every endpoint and dimension is checked.
 */
cv::Rect scale_up_rect(const cv::Rect& rect, int factor) noexcept;

/**
 * @brief Expands an ROI by a halo and clips it to image bounds.
 * @param roi Source ROI.
 * @param halo_size Non-negative halo width.
 * @param bounds Destination image extent.
 * @return Safe in-bounds halo ROI or empty.
 * @throws Nothing.
 * @note Non-positive halo still clips roi to bounds.
 */
cv::Rect calculate_halo(const cv::Rect& roi, int halo_size,
                        const cv::Size& bounds) noexcept;

}  // namespace ps::compute
