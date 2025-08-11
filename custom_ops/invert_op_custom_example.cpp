// Include the plugin API and any other necessary headers from the core project.
#include "../include/plugin_api.hpp"
#include <opencv2/imgproc.hpp>

// Use the same namespace for consistency, though not strictly required.
namespace ps { namespace ops {

/**
 * @brief (image_process:invert) Inverts the colors of an image.
 * This is a custom operation defined in a separate shared library.
 */
static NodeOutput op_invert_colors(const Node&, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) {
        throw GraphError("invert_colors requires one valid input image");
    }

    cv::Mat source_image = inputs[0];
    cv::Mat inverted_image;

    // Invert needs a 3-channel BGR image, not BGRA.
    if (source_image.channels() == 4) {
        cv::cvtColor(source_image, source_image, cv::COLOR_BGRA2BGR);
    }
    
    // The bitwise_not operator is a simple and fast way to invert colors.
    cv::bitwise_not(source_image, inverted_image);

    NodeOutput result;
    result.image_matrix = inverted_image;
    return result;
}


}} // namespace ps::ops


/**
 * @brief The required registration function that the main application will call.
 */
extern "C" PLUGIN_API void register_photospider_ops() {
    // Get the singleton instance of the registry
    auto& registry = ps::OpRegistry::instance();

    // Register our custom operation(s)
    registry.register_op("image_process", "invert", ps::ops::op_invert_colors);
    
    // You could register more ops from this plugin here
    // registry.register_op("my_type", "my_subtype", my_other_op_func);
}