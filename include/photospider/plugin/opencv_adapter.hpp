#pragma once

#include <opencv2/core.hpp>

#include "photospider/data/value.hpp"

/**
 * @file opencv_adapter.hpp
 * @brief Optional OpenCV adapters for ordinary DenseImage Values.
 */

namespace ps::plugin::opencv {

/**
 * @brief Creates a zero-copy read-only OpenCV view of one ordinary image.
 * @param value Ready host-readable Strided DenseTensor Value with a complete
 *        ImageFacet and OpenCV-compatible interleaved whole-byte storage.
 * @return Matrix borrowing the Value payload with data-window dimensions.
 * @throws std::invalid_argument for unsupported representation, axes, layout,
 *         element encoding, channel count, logical width/height above
 *         `INT_MAX`, or non-interleaved/multi-row nonpositive stride.
 * @throws ReadyFenceAccessError or BufferAccessError when the Value is not
 *         synchronously host-readable.
 * @throws std::overflow_error when checked active-row byte arithmetic is
 *         unrepresentable.
 * @throws std::bad_alloc when owned ImageView facet metadata or temporary
 *         channel-data coordinate storage cannot be allocated.
 * @throws cv::Exception when OpenCV rejects the validated matrix header.
 * @note The returned matrix must be treated as read-only and must not outlive
 *       `value`; the matrix header retains neither the Value nor a ReadLease.
 *       No sample/color conversion, transfer, mapping, or copy occurs.
 *       Complete logical extents are checked before any OpenCV header
 *       construction or signed narrowing.
 */
cv::Mat to_mat(const Value& value);

/**
 * @brief Uploads one ordinary DenseImage Value to an OpenCV unified matrix.
 * @param value Ready host-readable Value accepted by `to_mat`.
 * @return Read-only-by-contract UMat initialized from the borrowed matrix.
 * @throws std::invalid_argument for the same representation, axes, layout,
 *         element, channel, extent, or stride rejection as `to_mat`.
 * @throws ReadyFenceAccessError or BufferAccessError when the Value is not
 *         synchronously host-readable.
 * @throws std::overflow_error when checked active-row byte arithmetic is
 *         unrepresentable.
 * @throws std::bad_alloc when owned ImageView facet metadata or temporary
 *         channel-data coordinate storage cannot be allocated.
 * @throws cv::Exception when matrix-header construction or
 *         `getUMat(cv::ACCESS_READ)` fails.
 * @note The returned UMat may own provider storage after upload; this function
 *       first follows the complete `to_mat` path and translates no exception.
 *       It still performs no sample-domain or color conversion.
 */
cv::UMat to_umat(const Value& value);

/**
 * @brief Publishes one OpenCV matrix as a fresh immutable ordinary image.
 * @param matrix Nonempty two-dimensional OpenCV matrix.
 * @param image_facet Complete image interpretation whose x/y/channel axes and
 *        signed data window match `[rows, cols, channels]`.
 * @return Fresh Ready CPU Value with exact matrix scalar storage and copied
 *         active row bytes; data/display windows, channel/group, sample, and
 *         color facts are preserved from `image_facet`.
 * @throws std::invalid_argument for empty/unsupported matrix or inconsistent
 *         image metadata.
 * @throws std::overflow_error when layout or byte arithmetic is
 * unrepresentable.
 * @throws std::bad_alloc when descriptor, payload, or Value storage allocates.
 * @throws cv::Exception when OpenCV metadata access fails.
 * @note Row padding is zero-initialized and never interpreted as active data.
 *       The operation performs no normalization, clamp, rounding, or color
 *       conversion and retains no OpenCV owner.
 */
Value from_mat(const cv::Mat& matrix, ImageFacet image_facet);

/**
 * @brief Publishes one OpenCV unified matrix as a fresh immutable image.
 * @param matrix Nonempty unified matrix.
 * @param image_facet Complete interpretation matching matrix shape/channels.
 * @return Fresh Ready CPU Value copied from a read-only host mapping.
 * @throws The same contract exceptions as `from_mat`, plus cv::Exception when
 *         obtaining the OpenCV mapping fails.
 * @note The returned Value retains no UMat or mapping identity and performs no
 *       implicit sample/color conversion.
 */
Value from_umat(const cv::UMat& matrix, ImageFacet image_facet);

}  // namespace ps::plugin::opencv
