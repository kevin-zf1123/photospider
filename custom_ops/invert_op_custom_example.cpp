#include "plugin_api.hpp"
#include <opencv2/opencv.hpp>

using namespace ps;

// A simple example of an image processing operation that inverts the image.
// (Calculates 1.0 - pixel_value for each channel).

// --- MODIFIED: The function signature now matches the new OpFunc definition ---
static NodeOutput invert_op(const Node&, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_umatrix.empty()) {
        throw GraphError("Invert operation requires one valid image input.");
    }

    // --- MODIFIED: Access the input UMat directly from the NodeOutput pointer ---
    const cv::UMat& u_input = inputs[0]->image_umatrix;
    cv::UMat u_output;

    // Perform the inversion operation on the UMat
    cv::subtract(cv::Scalar::all(1.0), u_input, u_output);

    NodeOutput result;
    // --- MODIFIED: Store the resulting UMat in the output ---
    result.image_umatrix = u_output;
    return result;
}

// The registration function that Photospider will look for.
extern "C" PLUGIN_API void register_photospider_ops() {
    OpRegistry::instance().register_op("image_process", "invert_example", invert_op);
}