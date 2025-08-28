// FILE: custom_ops/invert_op_custom_example.cpp (FIXED)

#include "plugin_api.hpp"
#include <opencv2/core.hpp>

using namespace ps;

static NodeOutput invert_op(const Node&, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) {
        throw GraphError("Invert operation requires one input image.");
    }

    cv::UMat u_input = inputs[0].getUMat(cv::ACCESS_READ);
    cv::UMat u_output;
    cv::subtract(cv::Scalar::all(1.0), u_input, u_output);

    NodeOutput result;
    // MODIFIED: 添加 .clone() 进行深拷贝
    result.image_matrix = u_output.getMat(cv::ACCESS_READ).clone();
    return result;
}

extern "C" PLUGIN_API void register_photospider_ops() {
    OpRegistry::instance().register_op("image_process", "invert_example", invert_op);
}