#include "plugin_api.hpp"
#include "ops.hpp"

namespace ps { namespace ops {

static NodeOutput op_save_image(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty() || (inputs[0]->image_matrix.empty() && inputs[0]->image_umatrix.empty())) {
        throw GraphError("save_image requires one valid input image");
    }

    cv::Mat image_to_save;
    if (!inputs[0]->image_umatrix.empty()) {
        image_to_save = inputs[0]->image_umatrix.getMat(cv::ACCESS_READ);
    } else {
        image_to_save = inputs[0]->image_matrix;
    }

    const auto& P = node.runtime_parameters;
    std::string path = P["path"].as<std::string>();

    cv::Mat out_mat;
    image_to_save.convertTo(out_mat, CV_8U, 255.0);
    cv::imwrite(path, out_mat);

    return {};
}

}}

extern "C" PLUGIN_API void register_photospider_ops() {
    ps::OpRegistry::instance().register_op("image_process", "save_image", ps::ops::op_save_image);
}