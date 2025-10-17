#include "plugin_api.hpp"
#include "adapter/buffer_adapter_opencv.hpp" // 引入适配器

// 这是一个 Monolithic 操作，因为它处理整个图像
ps::NodeOutput op_invert(const ps::Node&, const std::vector<const ps::NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_buffer.width == 0) {
        throw ps::GraphError(ps::GraphErrc::MissingDependency, "Invert op requires one valid input image.");
    }

    // 1. 使用适配器从 ImageBuffer 获取 UMat
    const cv::UMat& u_input = ps::toCvUMat(inputs[0]->image_buffer);

    cv::UMat u_output;
    // 2. 执行核心操作
    cv::subtract(cv::Scalar::all(1.0), u_input, u_output);

    ps::NodeOutput result;
    // 3. 使用适配器将结果 UMat 包装回 ImageBuffer
    result.image_buffer = ps::fromCvUMat(u_output);
    return result;
}

extern "C" PLUGIN_API void register_photospider_ops() {
    ps::OpRegistry::instance().register_op("image_process", "invert", op_invert);
}