#include "core/region_image_adapter.hpp"  // NOLINT(build/include_subdir)

#include <algorithm>
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

/**
 * @brief Adds one nonnegative storage offset to a logical coordinate.
 *
 * @param origin Signed logical data-window origin.
 * @param offset Nonnegative zero-based storage offset.
 * @param diagnostic Stable overflow diagnostic.
 * @return Exact translated logical coordinate.
 * @throws std::invalid_argument when offset is negative.
 * @throws std::overflow_error when the mathematical sum exceeds int64_t.
 */
std::int64_t checked_coordinate_add(std::int64_t origin, int offset,
                                    const char* diagnostic) {
  if (offset < 0) {
    throw std::invalid_argument(
        "Storage PixelRect requires a nonnegative origin.");
  }
  if (origin > std::numeric_limits<std::int64_t>::max() - offset) {
    throw std::overflow_error(diagnostic);
  }
  return origin + offset;
}

/**
 * @brief Converts one ordered signed-coordinate distance to current int.
 *
 * @param begin Inclusive lower coordinate.
 * @param end Coordinate no smaller than begin.
 * @param diagnostic Stable overflow diagnostic.
 * @return Exact nonnegative distance represented as int.
 * @throws std::invalid_argument when endpoints are inverted.
 * @throws std::overflow_error when the distance exceeds int.
 * @note Unsigned subtraction preserves the exact mathematical distance across
 * zero after ordering has been established.
 */
int checked_distance(std::int64_t begin, std::int64_t end,
                     const char* diagnostic) {
  return checked_extent(begin, end, diagnostic);
}

/**
 * @brief Validates one finite image data window and returns its int extent.
 *
 * @param data_window Candidate signed logical payload window.
 * @return Zero-based PixelSize matching the exact window span.
 * @throws std::invalid_argument for empty or reversed bounds.
 * @throws std::overflow_error when either span exceeds PixelSize int.
 */
PixelSize checked_storage_size(const ImageBounds& data_window) {
  const PixelSize result{
      checked_extent(data_window.x_begin, data_window.x_end,
                     "Image data-window width exceeds PixelRect int."),
      checked_extent(data_window.y_begin, data_window.y_end,
                     "Image data-window height exceeds PixelRect int.")};
  if (result.width <= 0 || result.height <= 0) {
    throw std::invalid_argument(
        "Storage PixelRect conversion requires a nonempty data window.");
  }
  return result;
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

/** @copydoc from_storage_pixel_rect */
RegionSet from_storage_pixel_rect(const PixelRect& storage_rect,
                                  const ImageBounds& data_window) {
  if (storage_rect.width < 0 || storage_rect.height < 0) {
    throw std::invalid_argument(
        "Storage PixelRect conversion rejects negative extents.");
  }
  const PixelSize storage_size = checked_storage_size(data_window);
  const std::int64_t storage_x_end =
      static_cast<std::int64_t>(storage_rect.x) + storage_rect.width;
  const std::int64_t storage_y_end =
      static_cast<std::int64_t>(storage_rect.y) + storage_rect.height;
  if (storage_rect.x < 0 || storage_rect.y < 0 ||
      storage_x_end > storage_size.width ||
      storage_y_end > storage_size.height) {
    throw std::invalid_argument(
        "Storage PixelRect exceeds its image data window.");
  }
  if (storage_rect.width == 0 || storage_rect.height == 0) {
    return RegionSet::empty();
  }
  const std::int64_t x_begin = checked_coordinate_add(
      data_window.x_begin, storage_rect.x,
      "Storage PixelRect x origin translation overflowed.");
  const std::int64_t x_end = checked_coordinate_add(
      x_begin, storage_rect.width,
      "Storage PixelRect x endpoint translation overflowed.");
  const std::int64_t y_begin = checked_coordinate_add(
      data_window.y_begin, storage_rect.y,
      "Storage PixelRect y origin translation overflowed.");
  const std::int64_t y_end = checked_coordinate_add(
      y_begin, storage_rect.height,
      "Storage PixelRect y endpoint translation overflowed.");
  return RegionSet::from_image_rect(
      {image_region_domain(), x_begin, x_end, y_begin, y_end});
}

/** @copydoc to_storage_pixel_rect */
PixelRect to_storage_pixel_rect(const RegionSet& region,
                                const ImageBounds& data_window) {
  const PixelSize storage_size = checked_storage_size(data_window);
  if (region.is_empty()) {
    return PixelRect{};
  }
  if (region.is_whole()) {
    return PixelRect{0, 0, storage_size.width, storage_size.height};
  }
  if (region.atoms().size() != 1U ||
      !std::holds_alternative<ImageRect>(region.atoms().front())) {
    throw std::invalid_argument(
        "Storage PixelRect projection accepts exactly one ImageRect atom.");
  }
  const ImageRect& rect = std::get<ImageRect>(region.atoms().front());
  if (!(rect.domain == image_region_domain())) {
    throw std::invalid_argument(
        "Storage PixelRect projection rejects non-built-in image domains.");
  }
  if (rect.x_end < rect.x_begin || rect.y_end < rect.y_begin) {
    throw std::invalid_argument(
        "Storage PixelRect projection rejects inverted ImageRect endpoints.");
  }
  const std::int64_t x_begin = std::max(rect.x_begin, data_window.x_begin);
  const std::int64_t x_end = std::min(rect.x_end, data_window.x_end);
  const std::int64_t y_begin = std::max(rect.y_begin, data_window.y_begin);
  const std::int64_t y_end = std::min(rect.y_end, data_window.y_end);
  if (x_end <= x_begin || y_end <= y_begin) {
    return PixelRect{};
  }
  return PixelRect{
      checked_distance(data_window.x_begin, x_begin,
                       "ImageRect storage x offset exceeds PixelRect int."),
      checked_distance(data_window.y_begin, y_begin,
                       "ImageRect storage y offset exceeds PixelRect int."),
      checked_extent(x_begin, x_end,
                     "ImageRect storage width exceeds PixelRect int."),
      checked_extent(y_begin, y_end,
                     "ImageRect storage height exceeds PixelRect int.")};
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
