#pragma once

#include "photospider/core/geometry.hpp"
#include "photospider/data/image_metadata.hpp"
#include "photospider/data/region.hpp"

namespace ps::region_image_adapter {

/**
 * @brief Converts one current PixelRect edge value into logical ImageRect.
 *
 * @param rect Current two-dimensional origin/extent rectangle.
 * @param domain Explicit logical image domain for the new atom.
 * @return Canonical Empty for zero area, otherwise one exact ImageRect clause.
 * @throws std::invalid_argument for a negative extent or invalid domain.
 * @throws std::bad_alloc when Region storage cannot allocate.
 * @note Conversion occurs once at a current private ImageBuffer edge. The
 *       PixelRect is not retained as logical authority.
 */
RegionSet from_pixel_rect(const PixelRect& rect,
                          RegionDomainKey domain = image_region_domain());

/**
 * @brief Projects one exact finite ImageRect Region to current PixelRect.
 *
 * @param region Canonical exact Region to project.
 * @return Empty PixelRect for canonical Empty, otherwise the exact rectangle.
 * @throws std::invalid_argument for Whole, TensorSlice, a multi-atom clause,
 *         a non-built-in image domain, or invalid ImageRect semantics.
 * @throws std::overflow_error when origin or extent cannot fit current int
 *         PixelRect representation.
 * @note The returned value is callback-local physical/transport geometry and
 *       must not replace the source Region as planning or validity authority.
 */
PixelRect to_pixel_rect(const RegionSet& region);

/**
 * @brief Converts one zero-based storage ROI into a logical image Region.
 *
 * @param storage_rect Zero-based ROI relative to data_window storage.
 * @param data_window Signed logical payload bounds supplying the origin.
 * @return Canonical Empty for zero area, otherwise one exact logical
 * ImageRect translated by the data-window origin.
 * @throws std::invalid_argument when the bounds, extent, origin, or containment
 * contract is invalid.
 * @throws std::overflow_error when checked coordinate or extent arithmetic is
 * unrepresentable.
 * @throws std::bad_alloc when Region storage cannot allocate.
 * @note Pixel access remains storage-relative. The returned Region is the only
 * value that may be retained as logical work or validity metadata.
 */
RegionSet from_storage_pixel_rect(const PixelRect& storage_rect,
                                  const ImageBounds& data_window);

/**
 * @brief Clips and projects one logical image Region into storage coordinates.
 *
 * @param region Canonical Empty, Whole, or one built-in logical ImageRect.
 * @param data_window Signed logical payload bounds supplying the origin and
 * finite clip.
 * @return Zero-based PixelRect relative to data_window storage. Empty and a
 * disjoint ImageRect return an empty rectangle; Whole returns the full window.
 * @throws std::invalid_argument for malformed bounds, TensorSlice, a
 * multi-atom clause, a foreign domain, or invalid ImageRect semantics.
 * @throws std::overflow_error when a storage offset or extent cannot fit the
 * current int PixelRect representation.
 * @throws std::bad_alloc when Region clipping storage cannot allocate.
 * @note The Region remains logical authority. This projection performs
 * containment/clip before subtracting the signed data-window origin and must
 * only be used for callback-local storage, tiling, or transport geometry.
 */
PixelRect to_storage_pixel_rect(const RegionSet& region,
                                const ImageBounds& data_window);

/**
 * @brief Projects only an exact Region operation outcome to PixelRect.
 *
 * @param result Typed Region algebra or propagation outcome.
 * @return Exact finite PixelRect projection.
 * @throws std::invalid_argument for conservative, unknown, unsupported, or
 *         too-complex outcomes and for non-projectable Region values.
 * @throws std::overflow_error when exact ImageRect coordinates exceed int.
 * @note No failure or approximation status is reinterpreted as Empty/Whole.
 */
PixelRect exact_result_to_pixel_rect(const RegionOperationResult& result);

}  // namespace ps::region_image_adapter
