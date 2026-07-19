#pragma once

#include <cstdint>

#include "photospider/core/geometry.hpp"

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
 * @brief Constructs a rectangle from checked half-open pixel edges.
 * @param x0 Inclusive left coordinate.
 * @param y0 Inclusive top coordinate.
 * @param x1 Exclusive right coordinate.
 * @param y1 Exclusive bottom coordinate.
 * @return Positive-area PixelRect when every coordinate and dimension is
 *         representable by int; otherwise an empty rectangle.
 * @throws Nothing.
 * @note This is the common narrowing boundary for geometry derived with wide
 *       arithmetic.
 */
PixelRect rect_from_edges(std::int64_t x0, std::int64_t y0, std::int64_t x1,
                          std::int64_t y1) noexcept;

/**
 * @brief Checks whether a rectangle has no positive area.
 * @param rect Rectangle to inspect without endpoint arithmetic.
 * @return True when width or height is non-positive.
 * @throws Nothing.
 * @note Safe for arbitrary signed-int field values.
 */
bool is_rect_empty(const PixelRect& rect) noexcept;

/**
 * @brief Clips one rectangle to positive image bounds using wide arithmetic.
 * @param rect Candidate rectangle, including plugin-provided geometry.
 * @param bounds Positive image extent.
 * @return In-bounds intersection or an empty rectangle.
 * @throws Nothing.
 * @note Signed-int endpoint overflow is impossible because addition precedes
 * narrowing in signed 64-bit space.
 */
PixelRect clip_rect(const PixelRect& rect, const PixelSize& bounds) noexcept;

/**
 * @brief Intersects two arbitrary pixel rectangles with wide endpoints.
 * @param lhs First half-open rectangle.
 * @param rhs Second half-open rectangle.
 * @return Their positive-area intersection or an empty rectangle.
 * @throws Nothing.
 * @note Endpoint addition and final narrowing are checked through
 *       rect_from_edges.
 */
PixelRect intersect_rect(const PixelRect& lhs, const PixelRect& rhs) noexcept;

/**
 * @brief Translates one rectangle without signed-int overflow.
 * @param rect Source rectangle.
 * @param dx Horizontal offset in wide pixel coordinates.
 * @param dy Vertical offset in wide pixel coordinates.
 * @return Translated rectangle or empty when the result is not representable.
 * @throws Nothing.
 * @note Empty input remains empty; dimensions are preserved on success.
 */
PixelRect translate_rect(const PixelRect& rect, std::int64_t dx,
                         std::int64_t dy) noexcept;

/**
 * @brief Expands all rectangle sides by non-negative padding safely.
 * @param rect Source rectangle.
 * @param padding Pixels added to every side; non-positive values return rect.
 * @return Expanded rectangle, or empty when result is not int-representable.
 * @throws Nothing.
 * @note Callers normally clip the result to an image extent afterward.
 */
PixelRect expand_rect(const PixelRect& rect, int padding) noexcept;

/**
 * @brief Aligns rectangle edges outward to one positive pixel grid.
 * @param rect Source rectangle.
 * @param alignment Grid step; values at most one return rect unchanged.
 * @return Aligned rectangle, or empty when result is not int-representable.
 * @throws Nothing.
 * @note Arithmetic uses signed 64-bit intermediates.
 */
PixelRect align_rect(const PixelRect& rect, int alignment) noexcept;

/**
 * @brief Computes a safe bounding union of two rectangles.
 * @param a First rectangle or empty identity.
 * @param b Second rectangle or empty identity.
 * @return Bounding union, or empty when it is not int-representable.
 * @throws Nothing.
 * @note Endpoint sums are evaluated in signed 64-bit space.
 */
PixelRect merge_rect(const PixelRect& a, const PixelRect& b) noexcept;

/**
 * @brief Ceil-divides a positive extent by a positive factor.
 * @param size Source extent.
 * @param factor Positive downscale divisor.
 * @return Downscaled extent, or empty for invalid inputs.
 * @throws Nothing.
 * @note Pre-division addition uses signed 64-bit arithmetic.
 */
PixelSize scale_down_size(const PixelSize& size, int factor) noexcept;

/**
 * @brief Ceil-divides rectangle endpoints by a positive factor.
 * @param rect Source rectangle.
 * @param factor Positive downscale divisor.
 * @return Downscaled rectangle, or empty for invalid/unrepresentable input.
 * @throws Nothing.
 * @note Endpoint arithmetic uses signed 64-bit intermediates.
 */
PixelRect scale_down_rect(const PixelRect& rect, int factor) noexcept;

/**
 * @brief Multiplies rectangle endpoints by a positive factor safely.
 * @param rect Source rectangle.
 * @param factor Positive upscale multiplier.
 * @return Upscaled rectangle, or empty for invalid/unrepresentable input.
 * @throws Nothing.
 * @note No narrowing occurs until every endpoint and dimension is checked.
 */
PixelRect scale_up_rect(const PixelRect& rect, int factor) noexcept;

/**
 * @brief Expands an ROI by a halo and clips it to image bounds.
 * @param roi Source ROI.
 * @param halo_size Non-negative halo width.
 * @param bounds Destination image extent.
 * @return Safe in-bounds halo ROI or empty.
 * @throws Nothing.
 * @note Non-positive halo still clips roi to bounds.
 */
PixelRect calculate_halo(const PixelRect& roi, int halo_size,
                         const PixelSize& bounds) noexcept;

}  // namespace ps::compute
