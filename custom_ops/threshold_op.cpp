// FILE: custom_ops/threshold_op.cpp (FIXED)

#include "plugin_api.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

using namespace ps;

static NodeOutput threshold_op(const Node& node, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) {
        throw GraphError("Threshold operation requires one input image.");
    }

    const auto& P = node.runtime_parameters;
    double threshold_value = 0.5;
    double max_value = 1.0;
    if (P["threshold"]) {
        threshold_value = P["threshold"].as<double>();
    }
    if (P["max_value"]) {
        max_value = P["max_value"].as<double>();
    }

    cv::UMat u_input = inputs[0].getUMat(cv::ACCESS_READ);
    cv::UMat u_output;
    cv::threshold(u_input, u_output, threshold_value, max_value, cv::THRESH_BINARY);

    NodeOutput result;
    // MODIFIED: 添加 .clone() 进行深拷贝
    result.image_matrix = u_output.getMat(cv::ACCESS_READ).clone();
    return result;
}

extern "C" PLUGIN_API void register_photospider_ops() {
    OpRegistry::instance().register_op("image_process", "threshold", threshold_op);
}