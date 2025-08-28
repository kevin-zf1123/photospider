#include "plugin_api.hpp"
#include <opencv2/opencv.hpp>

using namespace ps;

// A thresholding operation.
// Pixels below the threshold are set to 0, and pixels above are set to a max value.

static double as_double_flexible(const YAML::Node& n, const std::string& key, double defv) {
    if (!n || !n[key]) return defv;
    try { if (n[key].IsScalar()) return n[key].as<double>(); return defv; } catch (...) { return defv; }
}

// --- MODIFIED: The function signature now matches the new OpFunc definition ---
static NodeOutput threshold_op(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_umatrix.empty()) {
        throw GraphError("Threshold operation requires one valid image input.");
    }

    // --- MODIFIED: Access the input UMat directly from the NodeOutput pointer ---
    const cv::UMat& u_input = inputs[0]->image_umatrix;
    cv::UMat u_output;

    const auto& P = node.runtime_parameters;
    double threshold = as_double_flexible(P, "threshold", 0.5);
    double max_value = as_double_flexible(P, "max_value", 1.0);

    // Note: cv::threshold works best on single-channel images.
    // For simplicity, this example doesn't handle multi-channel inputs specifically,
    // but a real implementation might convert to grayscale first or apply per-channel.
    int threshold_type = cv::THRESH_BINARY;
    if (P["inverse"] && P["inverse"].as<bool>(false)) {
        threshold_type = cv::THRESH_BINARY_INV;
    }

    cv::threshold(u_input, u_output, threshold, max_value, threshold_type);

    NodeOutput result;
    // --- MODIFIED: Store the resulting UMat in the output ---
    result.image_umatrix = u_output;
    return result;
}

// The registration function that Photospider will look for.
extern "C" PLUGIN_API void register_photospider_ops() {
    OpRegistry::instance().register_op("image_process", "threshold", threshold_op);
}