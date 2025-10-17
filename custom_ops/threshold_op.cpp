#include "plugin_api.hpp"
#include "adapter/buffer_adapter_opencv.hpp" // 引入适配器

// 辅助函数
static double as_double_flexible(const YAML::Node& n, const std::string& key, double defv) {
    if (!n || !n[key]) return defv;
    try { if (n[key].IsScalar()) return n[key].as<double>(); return defv; } catch (...) { return defv; }
}

static std::string as_str(const YAML::Node& n, const std::string& key, const std::string& defv) {
    if (!n || !n[key]) return defv;
    try { return n[key].as<std::string>(); } catch (...) { return defv; }
}

ps::NodeOutput op_threshold(const ps::Node& node, const std::vector<const ps::NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_buffer.width == 0) {
        throw ps::GraphError(ps::GraphErrc::MissingDependency, "Threshold op requires one valid input image.");
    }

    // 1. 从 ImageBuffer 获取 UMat
    const cv::UMat& u_input = ps::toCvUMat(inputs[0]->image_buffer);

    const auto& P = node.runtime_parameters;
    double thresh = as_double_flexible(P, "thresh", 0.5);
    double maxval = as_double_flexible(P, "maxval", 1.0);
    std::string type_str = as_str(P, "type", "binary");

    int threshold_type = cv::THRESH_BINARY;
    if (type_str == "binary_inv") threshold_type = cv::THRESH_BINARY_INV;
    // ... 其他类型

    cv::UMat u_output;
    cv::threshold(u_input, u_output, thresh, maxval, threshold_type);

    ps::NodeOutput result;
    // 2. 将结果包装回 ImageBuffer
    result.image_buffer = ps::fromCvUMat(u_output);
    return result;
}

extern "C" PLUGIN_API void register_photospider_ops() {
    ps::OpRegistry::instance().register_op("image_process", "threshold", op_threshold);
}