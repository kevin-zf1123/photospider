#pragma once

/**
 * @file image_buffer_processing.hpp
 * @brief Declares dependency-neutral private image-buffer processing.
 */

#include "photospider/core/image_buffer.hpp"

namespace ps::image_processing {

/**
 * @brief Deep-copies one addressable CPU image buffer.
 *
 * @param source Valid CPU image descriptor to copy.
 * @return Independent aligned CPU descriptor with the same active pixels,
 *         dimensions, channels, and scalar type.
 * @throws std::invalid_argument if source is malformed, empty, non-CPU, or has
 *         no addressable owned payload.
 * @throws std::overflow_error if allocation or copy arithmetic is
 *         unrepresentable.
 * @throws std::bad_alloc if destination or alias-safe staging allocation fails.
 * @throws std::exception if the selected build-time adapter cannot copy the
 *         image.
 * @note CMake selects exactly one implementation. The returned buffer never
 *       aliases source storage or retains provider-specific context.
 */
ImageBuffer clone_cpu_image_buffer(const ImageBuffer& source);

/**
 * @brief Resizes one addressable CPU image into a new aligned CPU buffer.
 *
 * @param source Valid nonempty CPU image descriptor.
 * @param destination_size Positive destination extent.
 * @return Independent CPU image with source channels/type and requested extent.
 * @throws std::invalid_argument if the source or destination extent is invalid
 *         or the source is not addressable CPU storage.
 * @throws std::overflow_error if allocation arithmetic is unrepresentable.
 * @throws std::bad_alloc if processing storage cannot be allocated.
 * @throws std::exception if the selected build-time implementation fails.
 * @note The default OpenCV-enabled product uses `INTER_LINEAR`; the
 *       dependency-disabled implementation uses the kernel's deterministic
 *       bilinear scalar path.
 */
ImageBuffer resize_cpu_image_buffer(const ImageBuffer& source,
                                    const PixelSize& destination_size);

/**
 * @brief Converts one addressable CPU image to a supported channel count.
 *
 * @param source Valid nonempty CPU image descriptor.
 * @param destination_channels Required channel count.
 * @return Independent CPU image with the source extent/type and requested
 *         channels.
 * @throws std::invalid_argument if the source is invalid or the conversion is
 *         not one of 1-to-3/4, 3/4-to-1, 4-to-3, or 3-to-4.
 * @throws std::overflow_error if allocation arithmetic is unrepresentable.
 * @throws std::bad_alloc if processing storage cannot be allocated.
 * @throws std::exception if the selected build-time implementation fails.
 * @note Equal channel counts still return an independent deep copy.
 */
ImageBuffer convert_cpu_image_buffer_channels(const ImageBuffer& source,
                                              int destination_channels);

/**
 * @brief Resizes one source ROI and writes it into one destination ROI.
 *
 * @param source Valid nonempty CPU source image.
 * @param source_roi Source rectangle to sample.
 * @param destination Valid nonempty writable CPU destination image.
 * @param destination_roi Destination rectangle receiving the resized pixels.
 * @return Nothing.
 * @throws std::invalid_argument if descriptors are malformed, non-CPU,
 *         non-addressable, or have different channel/scalar formats.
 * @throws std::out_of_range if either ROI is empty, negative, or outside its
 *         descriptor.
 * @throws std::overflow_error if intermediate allocation/copy arithmetic is
 *         unrepresentable.
 * @throws std::bad_alloc if intermediate storage cannot be allocated.
 * @throws std::exception if the selected build-time implementation fails.
 * @note Pixels outside destination_roi and all row padding remain unchanged.
 */
void resize_cpu_image_buffer_region(const ImageBuffer& source,
                                    const PixelRect& source_roi,
                                    const ImageBuffer& destination,
                                    const PixelRect& destination_roi);

}  // namespace ps::image_processing
