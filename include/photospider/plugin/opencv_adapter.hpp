#pragma once

#include <opencv2/core.hpp>

#include "photospider/core/image_buffer.hpp"

/**
 * @file opencv_adapter.hpp
 * @brief Optional OpenCV adapter for public operation-plugin image values.
 *
 * Consumers opt into this header and its package component explicitly; the
 * core operation SDK itself has no OpenCV dependency.
 */

namespace ps::plugin::opencv {

/**
 * @brief Converts a CPU image descriptor to an OpenCV matrix view.
 * @param buffer Read-only image descriptor.
 * @return Matrix sharing the descriptor's CPU data storage.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error when no readable CPU data exists; every non-CPU
 * descriptor and opaque backend-only context requires a device-specific
 * adapter and is rejected.
 * @throws cv::Exception from OpenCV conversions.
 * @note The returned matrix must be treated as read-only. The source buffer
 * and its payload owner must outlive the matrix view; operation plugins must
 * not retain it beyond the callback that supplied the buffer.
 */
cv::Mat to_mat(const ImageBuffer& buffer);

/**
 * @brief Converts a read-only tile to an ROI-scoped OpenCV matrix view.
 * @param tile Borrowed input tile.
 * @return Matrix view covering tile.roi.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error for a missing buffer or payload.
 * @throws std::out_of_range for a negative or out-of-bounds ROI.
 * @throws cv::Exception for an invalid ROI or backend conversion.
 * @note Empty edge-aligned ROIs are valid. OpenCV cannot enforce pixel
 *       constness; callers must not mutate the returned view. The tile buffer
 *       and payload owner must outlive the matrix and normally remain valid
 *       only for the surrounding operation callback.
 */
cv::Mat to_mat(const InputTileView& tile);

/**
 * @brief Converts a writable tile to an ROI-scoped OpenCV matrix view.
 * @param tile Borrowed output tile.
 * @return Writable matrix view covering tile.roi.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error for a missing buffer or payload.
 * @throws std::out_of_range for a negative or out-of-bounds ROI.
 * @throws cv::Exception for an invalid ROI or backend conversion.
 * @note Empty edge-aligned ROIs are valid. Descriptor metadata remains const;
 * only pixels inside the returned view are writable. The tile buffer and
 * payload owner must outlive the returned view; do not retain it beyond the
 * callback.
 */
cv::Mat to_mat(const OutputTileView& tile);

/**
 * @brief Converts an image descriptor to an OpenCV unified matrix.
 * @param buffer Read-only image descriptor.
 * @return Unified matrix uploaded from CPU image data.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error when no readable CPU data exists; every non-CPU
 * descriptor and opaque backend-only context requires a device-specific
 * adapter and is rejected.
 * @throws cv::Exception from OpenCV conversions.
 * @note The source buffer and payload owner must outlive the returned unified
 * matrix, including CPU-upload views that may share external storage.
 */
cv::UMat to_umat(const ImageBuffer& buffer);

/**
 * @brief Converts a read-only tile to an ROI-scoped unified matrix.
 * @param tile Borrowed input tile.
 * @return Read-only-by-contract unified matrix view.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error for a missing buffer or payload.
 * @throws std::out_of_range for a negative or out-of-bounds ROI.
 * @throws cv::Exception for an invalid ROI or backend conversion.
 * @note Empty edge-aligned ROIs are valid. OpenCV cannot enforce pixel
 * constness; callers must not mutate the returned input view. The source tile
 * and payload owner must outlive the returned view; do not retain it beyond
 * the callback.
 */
cv::UMat to_umat(const InputTileView& tile);

/**
 * @brief Converts a writable tile to an ROI-scoped unified matrix.
 * @param tile Borrowed output tile.
 * @return Writable unified matrix view.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error for a missing buffer or payload.
 * @throws std::out_of_range for a negative or out-of-bounds ROI.
 * @throws cv::Exception for an invalid ROI or backend conversion.
 * @note Empty edge-aligned ROIs are valid. Descriptor metadata remains const;
 * only pixels inside the returned view are writable. The source tile and
 * payload owner must outlive the returned view; do not retain it beyond the
 * callback.
 */
cv::UMat to_umat(const OutputTileView& tile);

/**
 * @brief Wraps one OpenCV matrix in a shared CPU image descriptor.
 * @param matrix Matrix whose storage lifetime is retained or copied.
 * @return Completely empty descriptor for an empty matrix; otherwise a shared
 * image descriptor. Ordinary OpenCV-owned matrices remain zero-copy and
 * external raw-storage matrices are deep-copied.
 * @throws std::bad_alloc if clone or lifetime-control storage cannot allocate.
 * @throws std::runtime_error for unsupported matrix depth.
 * @throws cv::Exception when cloning fails.
 * @note The returned descriptor retains pixels through ImageBuffer::data, has
 * no backend context, and remains valid after both the matrix handle and any
 * external raw-storage owner are destroyed.
 */
ImageBuffer from_mat(const cv::Mat& matrix);

/**
 * @brief Wraps one OpenCV unified matrix in an image descriptor.
 * @param matrix Unified matrix whose context lifetime is retained.
 * @return Completely empty descriptor for an empty matrix; otherwise a CPU
 * image descriptor retaining both a read/write mapped data view and unified
 * context.
 * @throws std::bad_alloc if lifetime-control storage cannot be allocated.
 * @throws std::runtime_error for unsupported matrix depth.
 * @throws cv::Exception when mapping or retaining the unified matrix fails.
 * @note Mapping uses `cv::ACCESS_RW`, so a later OutputTile adapter may write
 * through ImageBuffer::data and synchronize those pixels back to the retained
 * UMat allocation. Data and context alias one host owner that destroys its Mat
 * mapping before its UMat; both aliases outlive the source handle.
 */
ImageBuffer from_umat(const cv::UMat& matrix);

}  // namespace ps::plugin::opencv
