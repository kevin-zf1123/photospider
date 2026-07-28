#pragma once

#include "photospider/core/geometry.hpp"
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
 * @note Conversion occurs once at the current ImageBuffer/v2 boundary. The
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
