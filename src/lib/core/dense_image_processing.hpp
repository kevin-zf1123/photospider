#pragma once

#include <cstddef>

#include "photospider/core/geometry.hpp"
#include "photospider/data/value.hpp"

/**
 * @file dense_image_processing.hpp
 * @brief Declares dependency-neutral processing over ordinary image Values.
 */

namespace ps::dense_image_processing {

/**
 * @brief Copies one Ready host-readable ordinary image into a fresh Value.
 * @param source Valid unquantized whole-byte Strided DenseImage Value.
 * @return Fresh Ready CPU Value preserving complete logical image metadata.
 * @throws Value validation/access, arithmetic, or allocation failures.
 * @note Active samples are copied coordinate-by-coordinate; source padding,
 * allocation identity, revision, producer, fence, and placement are absent
 * from the new Value.
 */
Value clone(const Value& source);

/**
 * @brief Bilinearly resizes one ordinary image into a fresh CPU Value.
 * @param source Valid Ready host-readable ordinary image.
 * @param destination_size Positive destination data-window extent.
 * @return Fresh image with preserved scalar/sample/color/channel metadata and
 * the original signed data-window origin; the display window stays independent.
 * @throws std::invalid_argument for unsupported image/storage facts or extent.
 * @throws std::overflow_error when shape, layout, or address arithmetic fails.
 * @throws std::bad_alloc when metadata or output storage cannot allocate.
 * @note Interpolation changes neither sample domain nor color interpretation.
 * Integral results use nearest rounding with half cases away from zero and are
 * clamped only to the physical destination storage range.
 */
Value resize(const Value& source, const PixelSize& destination_size);

/**
 * @brief Resizes one source ROI into one destination ROI of a fresh image.
 * @param source Valid Ready host-readable ordinary image.
 * @param source_roi Nonempty zero-based source storage rectangle.
 * @param destination_size Positive full destination extent.
 * @param destination_roi Nonempty zero-based destination storage rectangle.
 * @return Fresh image whose selected destination ROI contains the bilinear
 * result and whose remaining allocation bytes are numeric zero.
 * @throws std::invalid_argument or std::out_of_range for malformed facts.
 * @throws std::overflow_error or std::bad_alloc for output construction.
 * @note The returned Value retains complete source image interpretation except
 * for the resized data-window extent. No hidden sample conversion occurs.
 */
Value resize_region(const Value& source, const PixelRect& source_roi,
                    const PixelSize& destination_size,
                    const PixelRect& destination_roi);

/**
 * @brief Crops or zero-pads one image into a fresh top-left-aligned extent.
 * @param source Valid Ready host-readable ordinary image.
 * @param destination_size Positive destination extent.
 * @return Fresh image containing the overlapping source prefix and zero
 * samples elsewhere.
 * @throws Value validation/access, arithmetic, or allocation failures.
 * @note No channel, sample, or color conversion occurs; signed data-window
 * origin and independent display-window metadata are preserved.
 */
Value crop_or_pad(const Value& source, const PixelSize& destination_size);

/**
 * @brief Converts one image among the maintained one/three/four-channel forms.
 * @param source Valid Ready host-readable ordinary image.
 * @param destination_channels Required channel count.
 * @return Fresh image with unchanged storage and extent.
 * @throws std::invalid_argument for unsupported counts, missing channel axis,
 * or channel-specific metadata that cannot be transformed without guessing.
 * @throws std::overflow_error or std::bad_alloc for output construction.
 * @note The algorithm has explicit positional B/G/R[/A] behavior matching the
 * built-in image-mixing operation. Because output channel identities change,
 * conversion is accepted only when channel schema, per-channel sample rules,
 * and color-group authority are absent.
 */
Value convert_channels(const Value& source, std::size_t destination_channels);

/**
 * @brief Computes exact aligned factor-four FP32 box averages.
 * @param source Ready host-readable FP32 ordinary image whose dimensions are
 * positive multiples of four.
 * @param destination_roi Nonempty zero-based rectangle in the downscaled image.
 * @return Fresh downscaled Value with selected pixels computed and all other
 * bytes initialized to numeric zero.
 * @throws std::invalid_argument or std::out_of_range for unsupported facts.
 * @throws std::runtime_error when the host floating environment cannot be
 * captured or switched to round-to-nearest.
 * @throws std::overflow_error or std::bad_alloc for output construction.
 * @note Each result accumulates sixteen binary32 values in binary64, multiplies
 * by exact 1/16, and rounds once to binary32. The complete caller floating
 * environment is restored on every exit.
 */
Value exact_box_average_factor_four(const Value& source,
                                    const PixelRect& destination_roi);

}  // namespace ps::dense_image_processing
