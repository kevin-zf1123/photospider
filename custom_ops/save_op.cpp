#include "plugin_api.hpp"
#include "adapter/buffer_adapter_opencv.hpp" // 引入适配器

ps::NodeOutput op_save(const ps::Node& node, const std::vector<const ps::NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_buffer.width == 0) {
        throw ps::GraphError(ps::GraphErrc::MissingDependency, "Save op requires an input image.");
    }

    const auto& P = node.runtime_parameters;
    std::string path;
    if (P["path"]) path = P["path"].as<std::string>();
    if (path.empty()) throw ps::GraphError(ps::GraphErrc::InvalidParameter, "Save op requires a 'path' parameter.");

    // 1. 从 ImageBuffer 获取 cv::Mat (这可能会触发从GPU下载)
    cv::Mat image_to_save = ps::toCvMat(inputs[0]->image_buffer);

    cv::Mat out_mat;
    // 假设保存为 16-bit PNG
    image_to_save.convertTo(out_mat, CV_16U, 65535.0);

    try {
        cv::imwrite(path, out_mat);
    } catch (const cv::Exception& ex) {
        throw ps::GraphError(ps::GraphErrc::Io, "Failed to save image to " + path + ": " + ex.what());
    }

    // Save 节点不产生图像输出，只产生副作用
    return ps::NodeOutput();
}

extern "C" PLUGIN_API void register_photospider_ops() {
    ps::OpRegistry::instance().register_op("io", "save", op_save);
}