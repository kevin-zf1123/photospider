#pragma once

#include <opencv2/core.hpp>

#include "compute/image_buffer.hpp"

namespace ps {

// --- 转换为 OpenCV 类型 ---

/**
 * @brief Converts a read-only input tile to an OpenCV matrix view.
 *
 * The returned matrix is scoped to tile.roi and shares storage with the
 * referenced ImageBuffer when CPU data is available. Every non-CPU descriptor,
 * including one with a non-null data field, requires its device adapter.
 *
 * @param tile Input tile whose buffer and ROI should be viewed.
 * @return cv::Mat covering tile.roi.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error when tile has no buffer or the buffer has no
 * readable data payload.
 * @throws cv::Exception when the ROI is invalid.
 * @note OpenCV does not enforce pixel-level constness on cv::Mat views; callers
 * must treat matrices returned from InputTile as read-only.
 */
cv::Mat toCvMat(const InputTile& tile);

/**
 * @brief Converts a writable output tile to an OpenCV matrix view.
 *
 * The returned matrix covers tile.roi and writes through the active checked
 * Host grant using the immutable output plan's row stride. Operators use this
 * overload only during the grant's callback lifetime.
 *
 * @param tile Output tile whose plan, grant, and ROI should be viewed.
 * @return cv::Mat covering tile.roi.
 * @throws std::invalid_argument when plan element facts or channels cannot be
 * represented by OpenCV, or the ROI does not exactly match the grant.
 * @throws std::runtime_error when tile has no plan or active grant.
 * @throws std::logic_error when the grant is retired or revoked.
 * @throws std::overflow_error when validation arithmetic is unrepresentable.
 * @throws cv::Exception when the ROI is invalid.
 * @note The returned matrix borrows grant spans and must be destroyed before
 * grant retirement. No compatibility ImageBuffer becomes output authority.
 */
cv::Mat toCvMat(const OutputTile& tile);

/**
 * @brief Converts a CPU image descriptor to a read-only cv::Mat.
 * @param buffer CPU image descriptor with an owned data pointer.
 * @return Zero-copy matrix view over the descriptor's CPU data pointer.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error when no CPU payload is present; every non-CPU
 * descriptor and opaque backend-only context is intentionally rejected.
 * @throws cv::Exception when matrix construction fails.
 * @note The returned matrix borrows storage retained by buffer and must not
 *       outlive that descriptor's shared payload handles. OpenCV does not
 *       enforce const pixel access.
 */
cv::Mat toCvMat(const ImageBuffer& buffer);

/**
 * @brief Converts a read-only input tile to an OpenCV UMat view.
 *
 * @param tile Input tile whose buffer and ROI should be viewed.
 * @return cv::UMat covering tile.roi.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error when tile has no buffer or no readable payload.
 * @throws cv::Exception when the ROI or upload fails.
 * @note The returned UMat must be treated as read-only by tiled operators and
 * uses `cv::ACCESS_READ` for CPU-backed upload.
 */
cv::UMat toCvUMat(const InputTile& tile);

/**
 * @brief Converts a writable output tile to an OpenCV UMat view.
 *
 * @param tile Output tile whose plan, grant, and ROI should be viewed.
 * @return cv::UMat covering tile.roi.
 * @throws std::invalid_argument when plan element facts or channels cannot be
 * represented by OpenCV, or the ROI does not exactly match the grant.
 * @throws std::runtime_error when tile has no plan or active grant.
 * @throws std::logic_error when the grant is retired or revoked.
 * @throws std::overflow_error when validation arithmetic is unrepresentable.
 * @throws cv::Exception when the ROI or upload fails.
 * @note The returned UMat must complete all writes before grant retirement;
 * CPU-backed mapping uses `cv::ACCESS_WRITE`.
 */
cv::UMat toCvUMat(const OutputTile& tile);

/**
 * @brief Converts an image descriptor to a read-only-by-contract cv::UMat.
 * @param buffer CPU image descriptor with an owned data pointer.
 * @return Unified matrix uploaded from the CPU image data.
 * @throws std::invalid_argument when the descriptor is malformed or channels
 * are outside OpenCV's supported positive range.
 * @throws std::runtime_error when no CPU payload is present; every non-CPU
 * descriptor and opaque backend-only context is intentionally rejected.
 * @throws cv::Exception when matrix construction or backend upload fails.
 * @note Returned UMat ownership is independent as a lightweight OpenCV handle,
 *       while its payload remains sourced from buffer data.
 */
cv::UMat toCvUMat(const ImageBuffer& buffer);

// OpenCV-to-ImageBuffer conversions.

/**
 * @brief Wraps a cv::Mat in a shared CPU image descriptor.
 * @param mat Matrix whose storage and metadata are retained or copied.
 * @return Completely empty descriptor for an empty matrix, otherwise a CPU
 * ImageBuffer sharing ordinary OpenCV-owned storage or owning a deep copy of
 * external raw storage, without an opaque backend context.
 * @throws std::runtime_error when the matrix depth is unsupported.
 * @throws std::bad_alloc when clone or lifetime-control storage cannot
 * allocate.
 * @throws cv::Exception when cloning fails.
 * @note A `mat.u == nullptr` raw-storage header is cloned because copying its
 *       header cannot extend the external owner's lifetime. Ordinary owning
 *       matrices remain zero-copy.
 */
ImageBuffer fromCvMat(const cv::Mat& mat);

/**
 * @brief Wraps a cv::UMat in a shared backend image descriptor.
 * @param umat Unified matrix whose lightweight handle is copied and retained.
 * @return Completely empty descriptor for an empty matrix, otherwise a CPU
 * ImageBuffer with an owned read/write mapped data view and retained UMat
 * context.
 * @throws std::runtime_error when the matrix depth is unsupported.
 * @throws std::bad_alloc when context lifetime-control storage cannot allocate.
 * @throws cv::Exception from OpenCV metadata or handle copying.
 * @note Mapping uses `cv::ACCESS_RW`; data and context alias one host owner
 * whose Mat member is destroyed before its UMat member. This representation
 * supports OutputTile write-through and remains valid after the source handle
 * dies.
 */
ImageBuffer fromCvUMat(const cv::UMat& umat);

}  // namespace ps
