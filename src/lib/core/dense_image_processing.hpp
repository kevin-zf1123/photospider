#pragma once

#include <cstddef>
#include <optional>

#include "photospider/core/geometry.hpp"
#include "photospider/data/value.hpp"

/**
 * @file dense_image_processing.hpp
 * @brief Declares dependency-neutral processing over ordinary image Values.
 */

namespace ps::dense_image_processing {

/**
 * @brief Closed spatial policy used by image-mixing input normalization.
 * @throws Nothing for ordinary enum operations.
 * @note The policy describes raw geometry behavior only; it does not authorize
 * sample conversion or imply a Sample Domain.
 */
enum class SizeNormalizationMode {
  /** @brief Spatial extent is unchanged. */
  Unchanged,
  /** @brief Every destination sample is bilinearly derived from source data. */
  Resize,
  /** @brief Top-left overlap is copied and uncovered destination samples zero.
   */
  CropOrPad,
};

/**
 * @brief Projects Sample Domain authority through image normalization.
 *
 * @param source_descriptor Valid unquantized native ordinary-image descriptor.
 * @param source_facet Valid ordinary-image source interpretation.
 * @param destination_size Positive normalized destination extent.
 * @param destination_channels Positive normalized destination channel count.
 * @param size_mode Exact raw spatial-normalization policy.
 * @return Unchanged source Sample Domain when every normalization-synthesized
 *         raw constant belongs to all applicable declared intervals;
 *         otherwise no Sample Domain.
 * @throws std::invalid_argument for malformed source facts, invalid
 * destination facts, or an inconsistent `Unchanged` extent.
 * @throws std::overflow_error when source bounds cannot form a bounded extent.
 * @throws std::bad_alloc when copying retained sample metadata fails.
 * @note The proof is payload-free and fail-closed. Crop/pad contributes zero
 * only when the destination extends beyond the source on at least one axis;
 * resize, pure crop, maintained one-to-three/four replication,
 * three/four-to-one reduction, and four-to-three reduction add no fixed
 * constant. Three-to-four conversion contributes the maintained raw
 * opaque-alpha value. The function never widens or synthesizes a replacement
 * declaration.
 */
std::optional<SampleDomainFacet> project_normalized_sample_domain(
    const DenseTensorDescriptor& source_descriptor,
    const ImageFacet& source_facet, const PixelSize& destination_size,
    std::size_t destination_channels, SizeNormalizationMode size_mode);

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
 * samples elsewhere. Sample Domain authority is retained only when any
 * synthesized zero belongs to every applicable declared interval.
 * @throws Value validation/access, arithmetic, or allocation failures.
 * @note No channel, sample, or color conversion occurs; signed data-window
 * origin and independent display-window metadata are preserved. An unsafe
 * declaration is omitted rather than widened or inferred from payload.
 */
Value crop_or_pad(const Value& source, const PixelSize& destination_size);

/**
 * @brief Converts one image among the maintained one/three/four-channel forms.
 * @param source Valid Ready host-readable ordinary image.
 * @param destination_channels Required channel count.
 * @return Fresh image with unchanged storage and extent. Sample Domain
 * authority is retained only when a synthesized opaque-alpha constant belongs
 * to every applicable declared interval.
 * @throws std::invalid_argument for unsupported counts, missing channel axis,
 * or channel-specific metadata that cannot be transformed without guessing.
 * @throws std::overflow_error or std::bad_alloc for output construction.
 * @note The algorithm has explicit positional B/G/R[/A] behavior matching the
 * built-in image-mixing operation. Because output channel identities change,
 * conversion is accepted only when channel schema, per-channel sample rules,
 * and color-group authority are absent. Positional replication/reduction adds
 * no fixed constant; three-to-four conversion adds the maintained raw opaque
 * value and omits an excluding uniform declaration.
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
