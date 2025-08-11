// FILE: custom_ops/threshold_op.cpp

#include "../include/plugin_api.hpp"
#include <opencv2/imgproc.hpp>
#include <string>

namespace ps { namespace ops {

// Helper to safely get a double from a YAML node
static double as_double_flexible(const YAML::Node& n, const std::string& key, double defv) {
    if (!n || !n[key]) return defv;
    try { if (n[key].IsScalar()) return n[key].as<double>(); return defv; } catch (...) { return defv; }
}

// Helper to safely get a string from a YAML node
static std::string as_str(const YAML::Node& n, const std::string& key, const std::string& defv = {}) {
    if (!n || !n[key]) return defv;
    try { return n[key].as<std::string>(); } catch (...) { return defv; }
}

/**
 * @brief (image_process:threshold) Applies a fixed-level threshold to an image.
 * This operation converts a grayscale image to a binary image.
 */
static NodeOutput op_threshold(const Node& node, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) {
        throw GraphError("threshold requires one valid input image");
    }

    const auto& P = node.runtime_parameters;

    // Get parameters
    double thresh_val = as_double_flexible(P, "thresh", 127.0);
    double max_val = as_double_flexible(P, "maxval", 255.0);
    std::string type_str = as_str(P, "type", "binary");

    int threshold_type_flag;
    if (type_str == "binary") {
        threshold_type_flag = cv::THRESH_BINARY;
    } else if (type_str == "binary_inv") {
        threshold_type_flag = cv::THRESH_BINARY_INV;
    } else {
        throw GraphError("threshold: invalid 'type' parameter. Use 'binary' or 'binary_inv'.");
    }

    // Ensure input is grayscale for thresholding
    cv::Mat src_gray;
    if (inputs[0].channels() == 3) {
        cv::cvtColor(inputs[0], src_gray, cv::COLOR_BGR2GRAY);
    } else if (inputs[0].channels() == 4) {
        cv::cvtColor(inputs[0], src_gray, cv::COLOR_BGRA2GRAY);
    } else {
        src_gray = inputs[0];
    }
    
    cv::Mat dst_image;
    cv::threshold(src_gray, dst_image, thresh_val, max_val, threshold_type_flag);

    NodeOutput result;
    result.image_matrix = dst_image;
    return result;
}

}} // namespace ps::ops

/**
 * @brief The required registration function for this plugin.
 */
extern "C" PLUGIN_API void register_photospider_ops() {
    auto& registry = ps::OpRegistry::instance();

    // Register our custom threshold operation
    registry.register_op("image_process", "threshold", ps::ops::op_threshold);
}