#pragma once

#include "image_buffer.hpp"
#include <opencv2/core.hpp>

namespace ps {

// 将 ImageBuffer (或其一部分 Tile) 转换为 cv::Mat
// 这个转换是“零拷贝”的，返回的 cv::Mat 只是一个视图 (View)，它不拥有内存。
cv::Mat toCvMat(const Tile& tile);
cv::Mat toCvMat(const ImageBuffer& buffer);

// 将一个 cv::Mat 包装成 ImageBuffer。
// 这个过程也是“零拷贝”的。返回的 ImageBuffer 会通过 std::shared_ptr
// 共享 cv::Mat 的内存和引用计数，确保内存安全。
ImageBuffer fromCvMat(const cv::Mat& mat);

} // namespace ps