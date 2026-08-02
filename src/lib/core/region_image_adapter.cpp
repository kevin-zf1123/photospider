#include "core/region_image_adapter.hpp"  // NOLINT(build/include_subdir)

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <variant>

namespace ps::region_image_adapter {
namespace {

/**
 * @brief Converts a checked signed 64-bit coordinate to current int.
 * @param value Coordinate or extent to convert.
 * @param diagnostic Stable overflow diagnostic.
 * @return Exact int value.
 * @throws std::overflow_error when value is outside int range.
 */
int checked_int(std::int64_t value, const char* diagnostic) {
  if (value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    throw std::overflow_error(diagnostic);
  }
  return static_cast<int>(value);
}

/**
 * @brief Computes one nonnegative signed-coordinate span without overflow.
 *
 * @param begin Inclusive signed endpoint.
 * @param end Exclusive signed endpoint no smaller than begin.
 * @param diagnostic Stable PixelRect overflow diagnostic.
 * @return Exact span represented as current int extent.
 * @throws std::invalid_argument when endpoints are inverted.
 * @throws std::overflow_error when the mathematical span exceeds int.
 * @note Conversion to uint64_t and unsigned subtraction are defined modulo
 *       2^64. Because an ordered int64_t span is at most UINT64_MAX, that
 *       subtraction is the exact mathematical difference even across zero.
 */
int checked_extent(std::int64_t begin, std::int64_t end,
                   const char* diagnostic) {
  if (end < begin) {
    throw std::invalid_argument(
        "ImageRect projection rejects inverted endpoints.");
  }
  const std::uint64_t extent =
      static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(begin);
  if (extent > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error(diagnostic);
  }
  return static_cast<int>(extent);
}

}  // namespace

/** @copydoc from_pixel_rect */
RegionSet from_pixel_rect(const PixelRect& rect, RegionDomainKey domain) {
  if (!domain.valid()) {
    throw std::invalid_argument(
        "PixelRect conversion requires a nonzero Region domain.");
  }
  if (rect.width < 0 || rect.height < 0) {
    throw std::invalid_argument(
        "PixelRect conversion rejects negative extents.");
  }
  const std::int64_t x_end = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t y_end = static_cast<std::int64_t>(rect.y) + rect.height;
  return RegionSet::from_image_rect({domain, rect.x, x_end, rect.y, y_end});
}

/** @copydoc to_pixel_rect */
PixelRect to_pixel_rect(const RegionSet& region) {
  if (region.is_empty()) {
    return PixelRect{};
  }
  if (region.is_whole()) {
    throw std::invalid_argument(
        "Whole Region requires explicit finite image bounds.");
  }
  if (region.atoms().size() != 1U ||
      !std::holds_alternative<ImageRect>(region.atoms().front())) {
    throw std::invalid_argument(
        "Current PixelRect edge accepts exactly one ImageRect atom.");
  }
  const ImageRect& rect = std::get<ImageRect>(region.atoms().front());
  if (!(rect.domain == image_region_domain())) {
    throw std::invalid_argument(
        "Current PixelRect edge rejects non-built-in image domains.");
  }
  if (rect.x_end < rect.x_begin || rect.y_end < rect.y_begin) {
    throw std::invalid_argument(
        "ImageRect projection rejects inverted endpoints.");
  }
  return PixelRect{
      checked_int(rect.x_begin, "ImageRect x origin exceeds PixelRect int."),
      checked_int(rect.y_begin, "ImageRect y origin exceeds PixelRect int."),
      checked_extent(rect.x_begin, rect.x_end,
                     "ImageRect width exceeds PixelRect int."),
      checked_extent(rect.y_begin, rect.y_end,
                     "ImageRect height exceeds PixelRect int.")};
}

/** @copydoc exact_result_to_pixel_rect */
PixelRect exact_result_to_pixel_rect(const RegionOperationResult& result) {
  if (result.status() != RegionOperationStatus::Exact ||
      !result.region().has_value()) {
    throw std::invalid_argument(
        "PixelRect projection requires an exact Region outcome.");
  }
  return to_pixel_rect(*result.region());
}

}  // namespace ps::region_image_adapter
