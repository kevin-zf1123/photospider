// FILE: src/ops.cpp
//
// This file implements the core image processing and data generation functions (Operations, or "Ops").
// Each operation is a standalone C++ function that conforms to the `OpFunc` signature.
// An OpFunc takes a constant reference to a `Node` and a vector of input `cv::Mat` images,
// and it returns a `NodeOutput` struct, which contains the resulting image and any data outputs.
//
// To add a new operation:
// 1. Write a static function with the signature:
//    `static NodeOutput my_op_function(const Node& node, const std::vector<cv::Mat>& inputs)`
// 2. Inside the function, access parameters via `node.runtime_parameters`. This YAML::Node contains
//    both static parameters from the file and dynamic parameters from upstream nodes.
// 3. Access input images from the `inputs` vector. The order is determined by the `image_inputs`
//    list in the node's YAML definition.
// 4. Perform the operation and populate a `NodeOutput` struct.
// 5. Register the new function in `register_builtin()` at the bottom of this file, associating it
//    with a `type` and `subtype` string.
//

#include "ops.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

namespace ps { namespace ops {

// --- Helper Functions ---
// These static helpers provide robust, boilerplate-free access to YAML parameters.

// Safely gets a double from a YAML node, returning a default value on failure or if missing.
static double as_double_flexible(const YAML::Node& n, const std::string& key, double defv) {
    if (!n || !n[key]) return defv;
    try { if (n[key].IsScalar()) return n[key].as<double>(); return defv; } catch (...) { return defv; }
}

// Safely gets an integer from a YAML node, returning a default value on failure or if missing.
static int as_int_flexible(const YAML::Node& n, const std::string& key, int defv) {
    if (!n || !n[key]) return defv;
    try { return n[key].as<int>(); } catch (...) { return defv; }
}

// Safely gets a string from a YAML node, returning a default value on failure or if missing.
static std::string as_str(const YAML::Node& n, const std::string& key, const std::string& defv = {}) {
    if (!n || !n[key]) return defv;
    try { return n[key].as<std::string>(); } catch (...) { return defv; }
}

// Prepares two images for a mixing operation by promoting their channel counts to match.
// For example, a 1-channel grayscale image will be converted to 4-channel BGRA if the other is BGRA.
static void normalize_channels_for_mixing(cv::Mat& img1, cv::Mat& img2) {
    int c1 = img1.channels();
    int c2 = img2.channels();
    if (c1 == c2) return;

    if (c1 == 1 && c2 == 3) cv::cvtColor(img1, img1, cv::COLOR_GRAY2BGR);
    else if (c1 == 1 && c2 == 4) cv::cvtColor(img1, img1, cv::COLOR_GRAY2BGRA);
    else if (c2 == 1 && c1 == 3) cv::cvtColor(img2, img2, cv::COLOR_GRAY2BGR);
    else if (c2 == 1 && c1 == 4) cv::cvtColor(img2, img2, cv::COLOR_GRAY2BGRA);
    else if (c1 == 3 && c2 == 4) cv::cvtColor(img1, img1, cv::COLOR_BGR2BGRA);
    else if (c2 == 3 && c1 == 4) cv::cvtColor(img2, img2, cv::COLOR_BGR2BGRA);
}


// --- Operation Implementations ---

/**
 * @brief (image_source:path) Loads an image from a file.
 */
static NodeOutput op_image_source_path(const Node& node, const std::vector<cv::Mat>&) {
    const auto& P = node.parameters;
    std::string path = as_str(P, "path");
    if (path.empty()) throw GraphError("image_source:path requires parameters.path");
    
    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (img.empty()) throw GraphError("Failed to read image: " + path);

    if (P["resize"]) {
        int w = as_int_flexible(P, "width", 0);
        int h = as_int_flexible(P, "height", 0);
        if (w > 0 && h > 0) cv::resize(img, img, cv::Size(w, h));
    }
    
    NodeOutput result;
    result.image_matrix = img;
    return result;
}

/**
 * @brief (image_process:crop) Extracts a rectangular region of an image.
 */
static NodeOutput op_crop(const Node& node, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) {
        throw GraphError("crop requires one valid input image");
    }
    const cv::Mat& src_image = inputs[0];
    const auto& P = node.runtime_parameters;

    std::string mode = as_str(P, "mode", "value");
    int final_x, final_y, final_width, final_height;

    if (mode == "ratio") {
        double rx = as_double_flexible(P, "x", -1.0);
        double ry = as_double_flexible(P, "y", -1.0);
        double rw = as_double_flexible(P, "width", -1.0);
        double rh = as_double_flexible(P, "height", -1.0);
        if (rx < 0.0 || ry < 0.0 || rw <= 0.0 || rh <= 0.0) {
             throw GraphError("crop in 'ratio' mode requires non-negative values for 'x', 'y', and positive values for 'width', 'height'.");
        }
        final_x = static_cast<int>(rx * src_image.cols);
        final_y = static_cast<int>(ry * src_image.rows);
        final_width = static_cast<int>(rw * src_image.cols);
        final_height = static_cast<int>(rh * src_image.rows);
    } else if (mode == "value") {
        final_x = as_int_flexible(P, "x", -1);
        final_y = as_int_flexible(P, "y", -1);
        final_width = as_int_flexible(P, "width", -1);
        final_height = as_int_flexible(P, "height", -1);
        if (final_width <= 0 || final_height <= 0) {
            throw GraphError("crop 'width' and 'height' must be positive.");
        }
    } else {
        throw GraphError("crop 'mode' parameter must be either 'value' or 'ratio'.");
    }

    cv::Mat canvas = cv::Mat::zeros(final_height, final_width, src_image.type());
    cv::Rect source_rect(0, 0, src_image.cols, src_image.rows);
    cv::Rect crop_rect(final_x, final_y, final_width, final_height);
    cv::Rect intersection = source_rect & crop_rect;
    cv::Rect destination_roi(intersection.x - final_x, intersection.y - final_y, intersection.width, intersection.height);

    if (intersection.width > 0 && intersection.height > 0) {
        src_image(intersection).copyTo(canvas(destination_roi));
    }

    NodeOutput result;
    result.image_matrix = canvas;
    return result;
}

/**
 * @brief (image_process:gaussian_blur) Applies a Gaussian blur filter.
 */
static NodeOutput op_gaussian_blur(const Node& node, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) throw GraphError("gaussian_blur requires one valid input image");
    const auto& P = node.runtime_parameters;
    int k = as_int_flexible(P, "ksize", 3);
    if (k % 2 == 0) k += 1;
    double sigmaX = as_double_flexible(P, "sigmaX", 0.0);
    cv::Mat out_mat;
    cv::GaussianBlur(inputs[0], out_mat, cv::Size(k, k), sigmaX);
    NodeOutput result;
    result.image_matrix = out_mat;
    return result;
}

/**
 * @brief (image_process:resize) Changes the dimensions of an image.
 */
static NodeOutput op_resize(const Node& node, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) throw GraphError("resize requires one valid input image");
    const auto& P = node.runtime_parameters;
    int width = as_int_flexible(P, "width", 0);
    int height = as_int_flexible(P, "height", 0);
    if (width <= 0 || height <= 0) throw GraphError("resize requires 'width' and 'height' parameters > 0.");
    std::string interp_str = as_str(P, "interpolation", "linear");
    int interpolation_flag = cv::INTER_LINEAR;
    if (interp_str == "cubic") interpolation_flag = cv::INTER_CUBIC;
    else if (interp_str == "nearest") interpolation_flag = cv::INTER_NEAREST;
    else if (interp_str == "area") interpolation_flag = cv::INTER_AREA;
    cv::Mat out_mat;
    cv::resize(inputs[0], out_mat, cv::Size(width, height), 0, 0, interpolation_flag);
    NodeOutput result;
    result.image_matrix = out_mat;
    return result;
}

/**
 * @brief (image_mixing:add_weighted) Blends two images using weighted sum. Supports simple
 *        and advanced channel re-wiring modes.
 */
static NodeOutput op_add_weighted(const Node& node, const std::vector<cv::Mat>& inputs) {
    if (inputs.size() < 2 || inputs[0].empty() || inputs[1].empty()) throw GraphError("add_weighted requires two input images");
    const auto& P = node.runtime_parameters;
    const auto& map_param = P["channel_mapping"];

    double alpha = as_double_flexible(P, "alpha", 0.5);
    double beta  = as_double_flexible(P, "beta", 0.5);
    double gamma = as_double_flexible(P, "gamma", 0.0);
    std::string strategy = as_str(P, "merge_strategy", "resize");

    cv::Mat a_prep = inputs[0].clone();
    cv::Mat b_prep = inputs[1].clone();
    
    // Advanced path for channel re-wiring
    if (map_param) {
        std::vector<std::unordered_map<int, std::vector<int>>> mappings(2);
        
        auto parse_mapping = [&](const YAML::Node& mapping_node, int input_idx) {
            if (!mapping_node) return;
            for (const auto& it : mapping_node) {
                int src_idx = it.first.as<int>();
                if (it.second.IsScalar()) {
                    mappings[input_idx][src_idx].push_back(it.second.as<int>());
                } else if (it.second.IsSequence()) {
                    for (const auto& dest_node : it.second) {
                        mappings[input_idx][src_idx].push_back(dest_node.as<int>());
                    }
                }
            }
        };
        parse_mapping(map_param["input0"], 0);
        parse_mapping(map_param["input1"], 1);
        
        int out_channels = std::max({4, a_prep.channels(), b_prep.channels()});
        cv::Mat out_mat;
        
        std::vector<cv::Mat> a_ch, b_ch;
        cv::split(a_prep, a_ch);
        cv::split(b_prep, b_ch);

        // Logic split for crop vs resize
        if (strategy == "crop") {
            out_mat = cv::Mat::zeros(a_prep.size(), CV_MAKETYPE(a_prep.depth(), out_channels));
            cv::Rect roi_a(0, 0, a_prep.cols, a_prep.rows);
            cv::Rect roi_b(0, 0, b_prep.cols, b_prep.rows);
            cv::Rect intersection = roi_a & roi_b;

            if (intersection.width > 0 && intersection.height > 0) {
                std::vector<cv::Mat> intersection_channels;
                for (int i = 0; i < out_channels; ++i) {
                    intersection_channels.push_back(cv::Mat::zeros(intersection.size(), a_prep.depth()));
                }

                auto process_input_roi = [&](const std::vector<cv::Mat>& src_ch, const std::unordered_map<int, std::vector<int>>& mapping, double weight) {
                    for(const auto& pair : mapping) {
                        int src_idx = pair.first;
                        const auto& dest_indices = pair.second;
                        for (int dst_idx : dest_indices) {
                            if (src_idx >= src_ch.size() || dst_idx < 0 || dst_idx >= out_channels) continue;
                            cv::Mat weighted_src;
                            src_ch[src_idx](intersection).convertTo(weighted_src, intersection_channels[dst_idx].type(), weight);
                            intersection_channels[dst_idx] += weighted_src;
                        }
                    }
                };
                process_input_roi(a_ch, mappings[0], alpha);
                process_input_roi(b_ch, mappings[1], beta);

                if (gamma != 0.0) {
                    for (auto& chan : intersection_channels) chan += gamma;
                }
                
                bool alpha_mapped = false;
                if (out_channels == 4) {
                    auto is_alpha_targeted = [&](const YAML::Node& mapping_node) {
                        if (!mapping_node) return false;
                        for (const auto& it : mapping_node) {
                            if (it.second.IsScalar() && it.second.as<int>() == 3) return true;
                            if (it.second.IsSequence()) {
                                for (const auto& dest_node : it.second) { if (dest_node.as<int>() == 3) return true; }
                            }
                        }
                        return false;
                    };
                    if (is_alpha_targeted(map_param["input0"]) || is_alpha_targeted(map_param["input1"])) alpha_mapped = true;
                }
                if (out_channels == 4 && !alpha_mapped) {
                    cv::Mat alpha1 = (a_prep.channels() == 4) ? a_ch[3](intersection) : cv::Mat(intersection.size(), a_prep.depth(), cv::Scalar(255));
                    cv::Mat alpha2 = (b_prep.channels() == 4) ? b_ch[3](intersection) : cv::Mat(intersection.size(), b_prep.depth(), cv::Scalar(255));
                    cv::max(alpha1, alpha2, intersection_channels[3]);
                }

                cv::Mat blended_roi;
                cv::merge(intersection_channels, blended_roi);
                blended_roi.copyTo(out_mat(intersection));
            }

        } else { // "resize" strategy
            if (a_prep.size() != b_prep.size()) cv::resize(b_prep, b_prep, a_prep.size());
            std::vector<cv::Mat> final_channels;
            for (int i = 0; i < out_channels; ++i) final_channels.push_back(cv::Mat::zeros(a_prep.size(), a_prep.depth()));

            auto process_input = [&](const std::vector<cv::Mat>& src_ch, const std::unordered_map<int, std::vector<int>>& mapping, double weight) {
                for(const auto& pair : mapping) {
                    int src_idx = pair.first;
                    const auto& dest_indices = pair.second;
                    for (int dst_idx : dest_indices) {
                        if (src_idx >= src_ch.size() || dst_idx < 0 || dst_idx >= out_channels) continue;
                        cv::Mat weighted_src;
                        src_ch[src_idx].convertTo(weighted_src, final_channels[dst_idx].type(), weight);
                        final_channels[dst_idx] += weighted_src;
                    }
                }
            };
            process_input(a_ch, mappings[0], alpha);
            process_input(b_ch, mappings[1], beta);
            if (gamma != 0.0) { for (auto& chan : final_channels) chan += gamma; }
            
            bool alpha_mapped = false;
            if (out_channels == 4) {
                auto is_alpha_targeted = [&](const YAML::Node& mapping_node) {
                    if (!mapping_node) return false;
                    for (const auto& it : mapping_node) {
                        if (it.second.IsScalar() && it.second.as<int>() == 3) return true;
                        if (it.second.IsSequence()) {
                            for (const auto& dest_node : it.second) { if (dest_node.as<int>() == 3) return true; }
                        }
                    }
                    return false;
                };
                if (is_alpha_targeted(map_param["input0"]) || is_alpha_targeted(map_param["input1"])) alpha_mapped = true;
            }
            if (out_channels == 4 && !alpha_mapped) {
                cv::Mat alpha1 = (a_prep.channels() == 4) ? a_ch[3] : cv::Mat(a_prep.size(), a_prep.depth(), cv::Scalar(255));
                cv::Mat alpha2 = (b_prep.channels() == 4) ? b_ch[3] : cv::Mat(b_prep.size(), b_prep.depth(), cv::Scalar(255));
                cv::max(alpha1, alpha2, final_channels[3]);
            }
            cv::merge(final_channels, out_mat);
        }
        
        NodeOutput result;
        result.image_matrix = out_mat;
        return result;
    }

    // Fallback to simple logic if no channel mapping is specified
    normalize_channels_for_mixing(a_prep, b_prep);
    cv::Mat out_mat;
    if (strategy == "crop") {
        out_mat = cv::Mat::zeros(a_prep.size(), a_prep.type());
        cv::Rect roi = cv::Rect(0, 0, std::min(a_prep.cols, b_prep.cols), std::min(a_prep.rows, b_prep.rows));
        
        cv::Mat merged_roi;
        if (a_prep.channels() == 4) {
            std::vector<cv::Mat> a_ch, b_ch, out_ch(4);
            cv::split(a_prep(roi), a_ch);
            cv::split(b_prep(roi), b_ch);
            cv::addWeighted(a_ch[0], alpha, b_ch[0], beta, gamma, out_ch[0]);
            cv::addWeighted(a_ch[1], alpha, b_ch[1], beta, gamma, out_ch[1]);
            cv::addWeighted(a_ch[2], alpha, b_ch[2], beta, gamma, out_ch[2]);
            cv::max(a_ch[3], b_ch[3], out_ch[3]);
            cv::merge(out_ch, merged_roi);
        } else {
            cv::addWeighted(a_prep(roi), alpha, b_prep(roi), beta, gamma, merged_roi);
        }
        merged_roi.copyTo(out_mat(roi));
    } else { // resize
        if (a_prep.size() != b_prep.size()) cv::resize(b_prep, b_prep, a_prep.size());
        
        if (a_prep.channels() == 4) {
            std::vector<cv::Mat> a_ch, b_ch, out_ch(4);
            cv::split(a_prep, a_ch);
            cv::split(b_prep, b_ch);
            cv::addWeighted(a_ch[0], alpha, b_ch[0], beta, gamma, out_ch[0]);
            cv::addWeighted(a_ch[1], alpha, b_ch[1], beta, gamma, out_ch[1]);
            cv::addWeighted(a_ch[2], alpha, b_ch[2], beta, gamma, out_ch[2]);
            cv::max(a_ch[3], b_ch[3], out_ch[3]);
            cv::merge(out_ch, out_mat);
        } else {
            cv::addWeighted(a_prep, alpha, b_prep, beta, gamma, out_mat);
        }
    }
    
    NodeOutput result;
    result.image_matrix = out_mat;
    return result;
}

/**
 * @brief (image_mixing:diff) Computes the absolute difference between two images.
 */
static NodeOutput op_abs_diff(const Node& node, const std::vector<cv::Mat>& inputs) {
    if (inputs.size() < 2 || inputs[0].empty() || inputs[1].empty()) throw GraphError("diff requires two input images");
    std::string strategy = as_str(node.runtime_parameters, "merge_strategy", "resize");

    cv::Mat a_prep = inputs[0].clone();
    cv::Mat b_prep = inputs[1].clone();
    normalize_channels_for_mixing(a_prep, b_prep);
    cv::Mat out_mat;

    if (strategy == "crop") {
        out_mat = cv::Mat::zeros(a_prep.size(), a_prep.type());
        cv::Rect roi = cv::Rect(0, 0, std::min(a_prep.cols, b_prep.cols), std::min(a_prep.rows, b_prep.rows));
        
        cv::Mat diff_roi;
        if (a_prep.channels() == 4) {
            std::vector<cv::Mat> a_channels, b_channels;
            cv::split(a_prep(roi), a_channels);
            cv::split(b_prep(roi), b_channels);
            cv::Mat diff_b, diff_g, diff_r;
            cv::absdiff(a_channels[0], b_channels[0], diff_b);
            cv::absdiff(a_channels[1], b_channels[1], diff_g);
            cv::absdiff(a_channels[2], b_channels[2], diff_r);
            cv::Mat out_alpha;
            cv::max(a_channels[3], b_channels[3], out_alpha);
            std::vector<cv::Mat> out_channels = {diff_b, diff_g, diff_r, out_alpha};
            cv::merge(out_channels, diff_roi);
        } else {
            cv::absdiff(a_prep(roi), b_prep(roi), diff_roi);
        }
        diff_roi.copyTo(out_mat(roi));
    } else {
        if (a_prep.size() != b_prep.size()) cv::resize(b_prep, b_prep, a_prep.size());
        
        if (a_prep.channels() == 4) {
            std::vector<cv::Mat> a_channels, b_channels;
            cv::split(a_prep, a_channels);
            cv::split(b_prep, b_channels);
            cv::Mat diff_b, diff_g, diff_r;
            cv::absdiff(a_channels[0], b_channels[0], diff_b);
            cv::absdiff(a_channels[1], b_channels[1], diff_g);
            cv::absdiff(a_channels[2], b_channels[2], diff_r);
            cv::Mat out_alpha;
            cv::max(a_channels[3], b_channels[3], out_alpha);
            std::vector<cv::Mat> out_channels = {diff_b, diff_g, diff_r, out_alpha};
            cv::merge(out_channels, out_mat);
        } else {
            cv::absdiff(a_prep, b_prep, out_mat);
        }
    }
    NodeOutput result;
    result.image_matrix = out_mat;
    return result;
}

/**
 * @brief (analyzer:get_dimensions) Extracts the width and height of an image as data.
 */
static NodeOutput op_get_width(const Node&, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) throw GraphError("analyzer:get_width requires one image input.");
    NodeOutput out;
    out.data["width"] = inputs[0].cols;
    out.data["height"] = inputs[0].rows;
    return out;
}

/**
 * @brief (math:divide) Divides two numbers. Does not use image inputs.
 */
static NodeOutput op_divide(const Node& node, const std::vector<cv::Mat>&) {
    const auto& P = node.runtime_parameters;
    if (!P["operand1"] || !P["operand2"]) throw GraphError("math:divide requires 'operand1' and 'operand2'.");
    double op1 = P["operand1"].as<double>();
    double op2 = P["operand2"].as<double>();
    if (op2 == 0) throw GraphError("math:divide attempted to divide by zero.");
    NodeOutput out;
    out.data["result"] = op1 / op2;
    return out;
}

/**
 * @brief (image_process:extract_channel) Isolates a single channel from an image.
 */
static NodeOutput op_extract_channel(const Node& node, const std::vector<cv::Mat>& inputs) {
    if (inputs.empty() || inputs[0].empty()) {
        throw GraphError("extract_channel requires one valid input image");
    }
    const auto& P = node.runtime_parameters;
    std::string channel_str = as_str(P, "channel", "a");
    int channel_idx = -1;

    if (channel_str == "b" || channel_str == "0") channel_idx = 0;
    else if (channel_str == "g" || channel_str == "1") channel_idx = 1;
    else if (channel_str == "r" || channel_str == "2") channel_idx = 2;
    else if (channel_str == "a" || channel_str == "3") channel_idx = 3;
    else throw GraphError("extract_channel: invalid 'channel' parameter. Use b,g,r,a or 0,1,2,3.");

    if (inputs[0].channels() <= channel_idx) {
        std::ostringstream err;
        err << "extract_channel: image has only " << inputs[0].channels() << " channel(s), cannot extract index " << channel_idx;
        throw GraphError(err.str());
    }

    std::vector<cv::Mat> channels;
    cv::split(inputs[0], channels);

    NodeOutput result;
    result.image_matrix = channels[channel_idx];
    result.data["channel"] = channel_idx;
    return result;
}


/**
 * @brief Registers all the built-in operations with the OpRegistry singleton.
 *        This function is called once at application startup.
 */
void register_builtin() {
    auto& R = OpRegistry::instance();
    
    // Image Loading
    R.register_op("image_source", "path", op_image_source_path);
    
    // Image Processing
    R.register_op("image_process", "gaussian_blur", op_gaussian_blur);
    R.register_op("image_process", "resize", op_resize);
    R.register_op("image_process", "crop", op_crop);
    R.register_op("image_process", "extract_channel", op_extract_channel);
    
    // Image Mixing
    R.register_op("image_mixing", "add_weighted", op_add_weighted);
    R.register_op("image_mixing", "diff", op_abs_diff);
    
    // Data / Utility
    R.register_op("analyzer", "get_dimensions", op_get_width);
    R.register_op("math", "divide", op_divide);
}

}} // namespace ps::ops