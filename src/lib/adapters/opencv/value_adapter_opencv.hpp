#pragma once

#include <opencv2/core.hpp>

#include "compute/tile_task.hpp"
#include "photospider/data/value.hpp"

namespace ps {

/**
 * @brief Creates a zero-copy read-only OpenCV matrix over one image Value.
 * @param value Ready host-readable interleaved ordinary DenseImage Value.
 * @return Matrix borrowing `value` payload.
 * @throws std::invalid_argument for an unsupported Value or a logical width or
 *         height above OpenCV's `int` extent domain.
 * @throws ReadyFenceAccessError, BufferAccessError, or cv::Exception as
 *         documented by the public adapter.
 * @note The caller keeps `value` alive and treats the matrix as read-only.
 */
cv::Mat toCvMat(const Value& value);

/**
 * @brief Creates an ROI-scoped read-only OpenCV matrix for one input tile.
 * @param tile Borrowed tile whose Value and zero-based ROI remain live.
 * @return Matrix view over exactly `tile.roi`.
 * @throws std::runtime_error for a disconnected tile.
 * @throws std::invalid_argument or std::out_of_range for unsupported Value or
 *         ROI facts.
 * @throws ReadyFenceAccessError, BufferAccessError, or cv::Exception from the
 *         underlying Value adapter.
 * @note The matrix header is constructed directly from the validated ROI,
 *       original row stride, and exact ROI start. A representable tile may
 *       therefore view a Value whose complete logical extent exceeds OpenCV's
 *       matrix limit. OpenCV cannot enforce pixel constness; callers must not
 *       mutate it or outlive `tile.value`.
 */
cv::Mat toCvMat(const InputTile& tile);

/**
 * @brief Creates an ROI-scoped writable OpenCV matrix for one Host grant.
 * @param tile Borrowed immutable output plan and active grant.
 * @return Writable matrix covering the exact granted ROI.
 * @throws As documented by the Host output adapter implementation.
 * @note The matrix must retire before the grant.
 */
cv::Mat toCvMat(const OutputTile& tile);

/**
 * @brief Uploads one image Value to an OpenCV unified matrix.
 * @param value Value accepted by `toCvMat`.
 * @return Read-only-by-contract unified matrix.
 * @throws Validation, access, allocation, and cv::Exception failures unchanged.
 */
cv::UMat toCvUMat(const Value& value);

/**
 * @brief Uploads one input tile to an ROI-scoped unified matrix.
 * @param tile Tile accepted by `toCvMat(InputTile)`.
 * @return Read-only-by-contract unified ROI.
 * @throws Validation, access, allocation, and cv::Exception failures unchanged.
 */
cv::UMat toCvUMat(const InputTile& tile);

/**
 * @brief Creates a writable unified matrix over one Host output grant.
 * @param tile Tile accepted by `toCvMat(OutputTile)`.
 * @return Writable unified ROI.
 * @throws Validation, access, allocation, and cv::Exception failures unchanged.
 */
cv::UMat toCvUMat(const OutputTile& tile);

/**
 * @brief Copies one OpenCV matrix into a fresh image Value.
 * @param matrix Nonempty supported matrix.
 * @param image_facet Complete matching image interpretation.
 * @return Fresh Ready CPU Value.
 * @throws As documented by `plugin::opencv::from_mat`.
 */
Value fromCvMat(const cv::Mat& matrix, ImageFacet image_facet);

/**
 * @brief Copies one OpenCV unified matrix into a fresh image Value.
 * @param matrix Nonempty supported unified matrix.
 * @param image_facet Complete matching image interpretation.
 * @return Fresh Ready CPU Value.
 * @throws As documented by `plugin::opencv::from_umat`.
 */
Value fromCvUMat(const cv::UMat& matrix, ImageFacet image_facet);

}  // namespace ps
