#include "adapter/buffer_adapter_opencv.hpp"
#include <stdexcept>

namespace ps {

// 内部辅助函数：将我们的 DataType 转换为 OpenCV 的类型标识符
static int toCvType(ps::DataType type, int channels) {
    switch (type) {
        case ps::DataType::UINT8:   return CV_8UC(channels);
        case ps::DataType::INT8:    return CV_8SC(channels);
        case ps::DataType::UINT16:  return CV_16UC(channels);
        case ps::DataType::INT16:   return CV_16SC(channels);
        case ps::DataType::FLOAT32: return CV_32FC(channels);
        case ps::DataType::FLOAT64: return CV_64FC(channels);
    }
    throw std::runtime_error("Unsupported data type for OpenCV conversion");
}

// 内部辅助函数：从 OpenCV 类型标识符转换回我们的 DataType
static ps::DataType fromCvType(int cv_type) {
    switch (CV_MAT_DEPTH(cv_type)) {
        case CV_8U:  return ps::DataType::UINT8;
        case CV_8S:  return ps::DataType::INT8;
        case CV_16U: return ps::DataType::UINT16;
        case CV_16S: return ps::DataType::INT16;
        case CV_32F: return ps::DataType::FLOAT32;
        case CV_64F: return ps::DataType::FLOAT64;
        default: throw std::runtime_error("Unsupported cv::Mat depth for ImageBuffer conversion");
    }
}

// 实现：将 ImageBuffer 转换为 cv::Mat 视图
cv::Mat toCvMat(const ImageBuffer& buffer) {
    if (buffer.device != ps::Device::CPU || !buffer.data) {
        // 当前版本只支持在CPU内存上的 buffer
        // 如果 data 为空，也无法创建视图
        throw std::runtime_error("toCvMat: Buffer is not on CPU or has no data.");
    }
    int type = toCvType(buffer.type, buffer.channels);
    
    // 使用 cv::Mat 的高级构造函数，它接受一个外部数据指针。
    // 这个构造函数创建的 cv::Mat 不会管理内存的生命周期，它仅仅是一个“视图”。
    return cv::Mat(buffer.height, buffer.width, type, buffer.data.get(), buffer.step);
}

// 实现：将 Tile 转换为 cv::Mat 视图
cv::Mat toCvMat(const Tile& tile) {
    if (!tile.buffer) {
        throw std::runtime_error("toCvMat: Tile has no associated buffer.");
    }
    // 1. 先获取整个 buffer 的 Mat 视图
    cv::Mat full_mat = toCvMat(*tile.buffer);
    // 2. 然后返回该视图的一个 ROI (Region of Interest)，这同样是零拷贝的。
    return full_mat(tile.roi);
}

// 实现：将 cv::Mat 包装为 ImageBuffer
ImageBuffer fromCvMat(const cv::Mat& mat) {
    ImageBuffer buffer;
    buffer.width = mat.cols;
    buffer.height = mat.rows;
    buffer.channels = mat.channels();
    buffer.type = fromCvType(mat.type());
    buffer.device = ps::Device::CPU;
    buffer.step = mat.step;

    // 关键部分：内存所有权共享
    // 我们创建一个 std::shared_ptr，它指向 cv::Mat 的数据区。
    // 同时，我们提供一个自定义的删除器 (deleter lambda)。
    //
    // 这个删除器的绝妙之处在于：
    // 1. 它捕获了原始的 cv::Mat 对象 (mat_ref)。这会增加 cv::Mat 的内部引用计数。
    // 2. 当删除器被调用时（即我们的 shared_ptr 引用计数归零时），它什么都不做。
    // 3. 当删除器本身被销毁时，它捕获的 mat_ref 也会被销毁，从而正确地减少 OpenCV 的引用计数。
    //
    // 最终效果是：只要我们的 ImageBuffer 还存在，原始 cv::Mat 的数据就不会被释放。
    buffer.data = std::shared_ptr<void>(mat.data, [mat_ref = mat](void*){ 
        // The captured mat_ref keeps the underlying cv::Mat data alive.
        // When this lambda is destroyed (because the shared_ptr's ref count is zero),
        // mat_ref is also destroyed, properly decrementing OpenCV's ref count.
    });

    // 如果 cv::Mat 是一个 UMat 的代理，我们也可以把 UMat 的句柄存入 context
    if (mat.u) {
        // 注意：这里需要更复杂的生命周期管理，但基本思路是相似的
        // 暂时简化处理
    }

    return buffer;
}

} // namespace ps