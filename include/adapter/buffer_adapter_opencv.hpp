#pragma once

#include <opencv2/core.hpp>

#include "image_buffer.hpp"

namespace ps {

// --- 转换为 OpenCV 类型 ---

/**
 * @brief Converts a read-only input tile to an OpenCV matrix view.
 *
 * The returned matrix is scoped to tile.roi and shares storage with the
 * referenced ImageBuffer when CPU data is available. GPU-backed buffers follow
 * the ImageBuffer conversion rule and may download through the adapter.
 *
 * @param tile Input tile whose buffer and ROI should be viewed.
 * @return cv::Mat covering tile.roi.
 * @throws std::runtime_error when tile has no buffer or the buffer has no
 * readable CPU/GPU payload.
 * @note OpenCV does not enforce pixel-level constness on cv::Mat views; callers
 * must treat matrices returned from InputTile as read-only.
 */
cv::Mat toCvMat(const InputTile& tile);

/**
 * @brief Converts a writable output tile to an OpenCV matrix view.
 *
 * The returned matrix is scoped to tile.roi and shares storage with the
 * destination ImageBuffer. Operators use this overload for tile writes.
 *
 * @param tile Output tile whose buffer and ROI should be viewed.
 * @return cv::Mat covering tile.roi.
 * @throws std::runtime_error when tile has no buffer or the buffer has no
 * writable CPU/GPU payload.
 * @note The destination buffer lifetime is owned by the compute service.
 */
cv::Mat toCvMat(const OutputTile& tile);

/**
 * @brief 将 ImageBuffer 转换为 cv::Mat 视图。
 * @note 行为：
 *       1. 如果 Buffer 内有 CPU 数据 (data 非空)，则返回一个零拷贝的 cv::Mat
 * 视图。
 *       2. 如果 Buffer 内只有 UMat (context 非空)，则会从 GPU 下载数据到新的
 * cv::Mat 并返回 (涉及拷贝)。
 * @return 一个 cv::Mat 对象。
 */
cv::Mat toCvMat(const ImageBuffer& buffer);

/**
 * @brief Converts a read-only input tile to an OpenCV UMat view.
 *
 * @param tile Input tile whose buffer and ROI should be viewed.
 * @return cv::UMat covering tile.roi.
 * @throws std::runtime_error when tile has no buffer or no readable payload.
 * @note The returned UMat must be treated as read-only by tiled operators.
 */
cv::UMat toCvUMat(const InputTile& tile);

/**
 * @brief Converts a writable output tile to an OpenCV UMat view.
 *
 * @param tile Output tile whose buffer and ROI should be viewed.
 * @return cv::UMat covering tile.roi.
 * @throws std::runtime_error when tile has no buffer or no writable payload.
 * @note The destination buffer lifetime is owned by the compute service.
 */
cv::UMat toCvUMat(const OutputTile& tile);

/**
 * @brief 将 ImageBuffer 转换为 cv::UMat。
 * @note 行为：
 *       1. 如果 Buffer 内已有 UMat (context 非空)，则直接返回该 UMat (零拷贝)。
 *       2. 如果 Buffer 内只有 CPU 数据 (data 非空)，则会将数据上传到 GPU
 * 创建新的 UMat 并返回 (涉及拷贝)。
 * @return 一个 cv::UMat 对象。
 */
cv::UMat toCvUMat(const ImageBuffer& buffer);

// --- 从 OpenCV 类型转换 ---

/**
 * @brief 将一个 cv::Mat 包装为 ImageBuffer。
 * @note 这是一个零拷贝操作。返回的 ImageBuffer 会通过 std::shared_ptr
 *       共享 cv::Mat 的内存和引用计数，确保内存安全。
 * @param mat 输入的 cv::Mat。
 * @return 一个基于 CPU 的 ImageBuffer。
 */
ImageBuffer fromCvMat(const cv::Mat& mat);

/**
 * @brief 将一个 cv::UMat 包装为 ImageBuffer。
 * @note 这是一个零拷贝操作。返回的 ImageBuffer 会在其 context 字段中
 *       持有对 cv::UMat 的共享引用，确保 GPU 资源被正确管理。
 * @param umat 输入的 cv::UMat。
 * @return 一个基于 GPU (UMat) 的 ImageBuffer。
 */
ImageBuffer fromCvUMat(const cv::UMat& umat);

}  // namespace ps
