#include "adapter/buffer_adapter_opencv.hpp"
#include <stdexcept>

namespace ps {

// --- 内部辅助函数 (不变) ---
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

// --- 实现：智能转换函数 ---

// 将 ImageBuffer 转换为 cv::Mat
cv::Mat toCvMat(const ImageBuffer& buffer) {
    // 优先路径：如果已有 CPU 数据，直接创建视图 (零拷贝)
    if (buffer.data) {
        int type = toCvType(buffer.type, buffer.channels);
        return cv::Mat(buffer.height, buffer.width, type, buffer.data.get(), buffer.step);
    }
    
    // 回退路径：如果只有 GPU 数据 (UMat in context)，从 GPU 下载 (涉及拷贝)
    if (buffer.context) {
        auto umat_ptr = std::static_pointer_cast<cv::UMat>(buffer.context);
        return umat_ptr->getMat(cv::ACCESS_READ);
    }

    throw std::runtime_error("toCvMat: Buffer has no data on CPU or GPU.");
}

cv::Mat toCvMat(const Tile& tile) {
    if (!tile.buffer) {
        throw std::runtime_error("toCvMat: Tile has no associated buffer.");
    }
    // 先获取整个 buffer 的 Mat (可能会触发下载)
    cv::Mat full_mat = toCvMat(*tile.buffer);
    // 返回 ROI 视图 (零拷贝)
    return full_mat(tile.roi);
}

// 将 ImageBuffer 转换为 cv::UMat
cv::UMat toCvUMat(const ImageBuffer& buffer) {
    // 优先路径：如果已有 UMat，直接返回 (零拷贝)
    if (buffer.context) {
        return *std::static_pointer_cast<cv::UMat>(buffer.context);
    }

    // 回退路径：如果只有 CPU 数据，上传到 GPU (涉及拷贝)
    if (buffer.data) {
        int type = toCvType(buffer.type, buffer.channels);
        cv::Mat temp_mat_view(buffer.height, buffer.width, type, buffer.data.get(), buffer.step);
        return temp_mat_view.getUMat(cv::ACCESS_READ);
    }

    throw std::runtime_error("toCvUMat: Buffer has no data on CPU or GPU.");
}

cv::UMat toCvUMat(const Tile& tile) {
    if (!tile.buffer) {
        throw std::runtime_error("toCvUMat: Tile has no associated buffer.");
    }
    // 先获取整个 buffer 的 UMat (可能会触发上传)
    cv::UMat full_umat = toCvUMat(*tile.buffer);
    // 返回 ROI 视图 (零拷贝)
    return full_umat(tile.roi);
}


// 从 cv::Mat 创建 ImageBuffer
ps::ImageBuffer fromCvMat(const cv::Mat& mat) {
    ps::ImageBuffer buffer;
    buffer.width = mat.cols;
    buffer.height = mat.rows;
    buffer.channels = mat.channels();
    buffer.type = fromCvType(mat.type());
    buffer.device = ps::Device::CPU; // 暂时都标记为 CPU
    buffer.step = mat.step;

    // 关键：这个 lambda 捕获了 mat 的一个副本 (mat_ref)。
    // 只要 buffer.data 这个 shared_ptr 存在，mat_ref 就会存在，
    // 从而保证了原始 mat 的引用计数不会降为 0，内存也就不会被释放。
    // 这对于 Mat 和 UMat 都有效。
    buffer.data = std::shared_ptr<void>(mat.data, [mat_ref = mat](void*){ 
        // 删除器什么都不做，mat_ref 的析构会自动处理引用计数
    });

    // 关于 context：它的目的是携带 API 特定的句柄。
    // 对于 UMat，如果你想保留 UMat 对象本身，可以这样做：
    if (mat.u != nullptr) {
        // 创建一个 shared_ptr 来管理一个 cv::UMat 对象的副本。
        // 这个副本和原始 mat 共享底层的图像数据。
        buffer.context = std::make_shared<cv::UMat>(mat.getUMat(cv::ACCESS_READ));
    }
    
    return buffer;
}

// 从 cv::UMat 创建 ImageBuffer
ImageBuffer fromCvUMat(const cv::UMat& umat) {
    ImageBuffer buffer;
    buffer.width = umat.cols;
    buffer.height = umat.rows;
    buffer.channels = umat.channels();
    buffer.type = fromCvType(umat.type());
    buffer.device = ps::Device::CPU; // UMat 是一个抽象，后端可能是 OpenCL，但从我们的角度看它仍由 CPU 调度
    buffer.step = umat.step;
    
    // CPU 数据指针初始为空，因为数据主要在 GPU 上
    buffer.data = nullptr;

    // 关键：在 context 中存储一个指向 UMat 的共享指针。
    // 我们在堆上创建一个 UMat 的拷贝（这个拷贝很轻量，只复制句柄和元数据），
    // 然后用 shared_ptr 管理它的生命周期。
    // 当 shared_ptr 被销毁时，这个 UMat 对象也被销毁，从而正确减少 GPU 资源的引用计数。
    buffer.context = std::make_shared<cv::UMat>(umat);

    return buffer;
}


} // namespace ps