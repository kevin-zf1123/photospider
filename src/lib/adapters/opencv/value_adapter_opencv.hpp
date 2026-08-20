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
 * @throws ReadyFenceAccessError or BufferAccessError when the Value is not
 *         synchronously host-readable.
 * @throws std::overflow_error when checked active-row byte arithmetic is
 *         unrepresentable.
 * @throws std::bad_alloc when owned ImageView facet metadata or temporary
 *         channel-data coordinate storage cannot be allocated.
 * @throws cv::Exception when OpenCV rejects the validated matrix header.
 * @note The caller keeps `value` alive and treats the matrix as read-only; the
 *       matrix header retains neither the Value nor a ReadLease. For an
 *       otherwise valid one-row matrix, a zero row stride becomes OpenCV
 *       `AUTO_STEP` and a negative stride becomes the active row byte count;
 *       only a positive padded stride retains a distinct source-step value.
 */
cv::Mat toCvMat(const Value& value);

/**
 * @brief Creates an ROI-scoped read-only OpenCV matrix for one input tile.
 * @param tile Borrowed tile whose Value and zero-based ROI remain live.
 * @return Matrix view over exactly `tile.roi`.
 * @throws std::runtime_error for a disconnected tile.
 * @throws std::invalid_argument or std::out_of_range for unsupported Value or
 *         ROI facts.
 * @throws ReadyFenceAccessError or BufferAccessError when the Value is not
 *         synchronously host-readable.
 * @throws std::overflow_error when checked active-row byte arithmetic is
 *         unrepresentable.
 * @throws std::bad_alloc when owned ImageView facet metadata or temporary
 *         channel-data coordinate storage cannot be allocated.
 * @throws cv::Exception when OpenCV rejects the validated matrix header.
 * @note The matrix header is constructed directly from the validated ROI,
 *       exact ROI start, and the applicable OpenCV row step. For an otherwise
 *       valid one-row matrix, a zero row stride becomes `AUTO_STEP` and a
 *       negative stride becomes the active row byte count; only a positive
 *       padded stride retains a distinct source-step value. The header retains
 *       neither the Value nor a ReadLease; callers must not mutate it and may
 *       use it only synchronously within the tile callback while the borrowed
 *       Value owner remains alive.
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
