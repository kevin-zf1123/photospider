// FILE: src/ops.cpp

// MODIFIED: 包含 UMat 所需的头文件
#include <opencv2/core.hpp>
#include "ops.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>
#include <random>
#include <cmath>
#include <numeric>

namespace ps { namespace ops {

// --- Helper Functions ---
// [ ... as_double_flexible, as_int_flexible, as_str 保持不变 ... ]
static double as_double_flexible(const YAML::Node& n, const std::string& key, double defv) {
    if (!n || !n[key]) return defv;
    try { if (n[key].IsScalar()) return n[key].as<double>(); return defv; } catch (...) { return defv; }
}
static int as_int_flexible(const YAML::Node& n, const std::string& key, int defv) {
    if (!n || !n[key]) return defv;
    try { return n[key].as<int>(); } catch (...) { return defv; }
}
static std::string as_str(const YAML::Node& n, const std::string& key, const std::string& defv = {}) {
    if (!n || !n[key]) return defv;
    try { return n[key].as<std::string>(); } catch (...) { return defv; }
}


// NEW: UMat 版本的 channel normalizer
static void normalize_channels_for_mixing_umat(cv::UMat& img1, cv::UMat& img2) {
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

static NodeOutput op_image_source_path(const Node& node, const std::vector<const NodeOutput*>&) {
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
    
    cv::Mat float_img;
    if (img.depth() == CV_32F) {
        float_img = img;
    } else {
        double scale = 1.0;
        if (img.depth() == CV_8U) scale = 1.0 / 255.0;
        else if (img.depth() == CV_16U) scale = 1.0 / 65535.0;
        img.convertTo(float_img, CV_32F, scale);
    }

    NodeOutput result;
    result.image_matrix = float_img;
    result.image_umatrix = float_img.getUMat(cv::ACCESS_READ);
    return result;
}

static NodeOutput op_crop(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_umatrix.empty()) {
        throw GraphError("crop requires one valid input image");
    }
    cv::UMat u_src_image = inputs[0]->image_umatrix;
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
        final_x = static_cast<int>(rx * u_src_image.cols);
        final_y = static_cast<int>(ry * u_src_image.rows);
        final_width = static_cast<int>(rw * u_src_image.cols);
        final_height = static_cast<int>(rh * u_src_image.rows);
    } else { // "value" mode
        final_x = as_int_flexible(P, "x", -1);
        final_y = as_int_flexible(P, "y", -1);
        final_width = as_int_flexible(P, "width", -1);
        final_height = as_int_flexible(P, "height", -1);
        if (final_width <= 0 || final_height <= 0) {
            throw GraphError("crop 'width' and 'height' must be positive.");
        }
    }

    cv::UMat u_canvas = cv::UMat::zeros(final_height, final_width, u_src_image.type());
    cv::Rect source_rect(0, 0, u_src_image.cols, u_src_image.rows);
    cv::Rect crop_rect(final_x, final_y, final_width, final_height);
    cv::Rect intersection = source_rect & crop_rect;
    cv::Rect destination_roi(intersection.x - final_x, intersection.y - final_y, intersection.width, intersection.height);

    if (intersection.width > 0 && intersection.height > 0) {
        u_src_image(intersection).copyTo(u_canvas(destination_roi));
    }

    NodeOutput result;
    result.image_umatrix = u_canvas;
    return result;
}

static NodeOutput op_gaussian_blur(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_umatrix.empty()) throw GraphError("gaussian_blur requires one valid input image");
    
    cv::UMat u_input = inputs[0]->image_umatrix;
    cv::UMat u_output;

    const auto& P = node.runtime_parameters;
    int k = as_int_flexible(P, "ksize", 3);
    if (k > 0 && k % 2 == 0) k += 1;
    if (k <=0) k = 1;
    double sigmaX = as_double_flexible(P, "sigmaX", 0.0);

    cv::GaussianBlur(u_input, u_output, cv::Size(k, k), sigmaX);
    
    NodeOutput result;
    result.image_umatrix = u_output;
    return result;
}

static NodeOutput op_resize(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_umatrix.empty()) throw GraphError("resize requires one valid input image");
    
    cv::UMat u_input = inputs[0]->image_umatrix;
    cv::UMat u_output;

    const auto& P = node.runtime_parameters;
    int width = as_int_flexible(P, "width", 0);
    int height = as_int_flexible(P, "height", 0);
    if (width <= 0 || height <= 0) throw GraphError("resize requires 'width' and 'height' parameters > 0.");
    std::string interp_str = as_str(P, "interpolation", "linear");
    int interpolation_flag = cv::INTER_LINEAR;
    if (interp_str == "cubic") interpolation_flag = cv::INTER_CUBIC;
    else if (interp_str == "nearest") interpolation_flag = cv::INTER_NEAREST;
    else if (interp_str == "area") interpolation_flag = cv::INTER_AREA;

    cv::resize(u_input, u_output, cv::Size(width, height), 0, 0, interpolation_flag);
    
    NodeOutput result;
    result.image_umatrix = u_output;
    return result;
}

static NodeOutput op_add_weighted(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.size() < 2 || inputs[0]->image_umatrix.empty() || inputs[1]->image_umatrix.empty()) {
        throw GraphError("add_weighted requires two input images");
    }

    cv::UMat u_a_prep = inputs[0]->image_umatrix;
    cv::UMat u_b_prep = inputs[1]->image_umatrix;
    cv::UMat u_out_mat;

    const auto& P = node.runtime_parameters;
    const auto& map_param = P["channel_mapping"];

    double alpha = as_double_flexible(P, "alpha", 0.5);
    double beta  = as_double_flexible(P, "beta", 0.5);
    double gamma = as_double_flexible(P, "gamma", 0.0);
    std::string strategy = as_str(P, "merge_strategy", "resize");

    if (map_param) {
        if (strategy == "resize") {
            if (u_a_prep.size() != u_b_prep.size()) {
                cv::resize(u_b_prep, u_b_prep, u_a_prep.size());
            }
        }
        cv::Size out_size = (strategy == "crop") 
            ? cv::Size(std::min(u_a_prep.cols, u_b_prep.cols), std::min(u_a_prep.rows, u_b_prep.rows))
            : u_a_prep.size();

        if (out_size.width == 0 || out_size.height == 0) {
            return NodeOutput{};
        }

        std::vector<cv::UMat> a_ch, b_ch;
        cv::split(u_a_prep, a_ch);
        cv::split(u_b_prep, b_ch);

        int out_channels = std::max({4, u_a_prep.channels(), u_b_prep.channels()});
        std::vector<cv::UMat> out_ch;
        for (int i = 0; i < out_channels; ++i) {
            out_ch.push_back(cv::UMat(out_size, CV_32F, cv::Scalar(0)));
        }

        auto process_mapping = [&](const YAML::Node& mapping_node, const std::vector<cv::UMat>& src_ch, double weight) {
            if (!mapping_node) return;
            for (const auto& it : mapping_node) {
                int src_idx = it.first.as<int>();
                if (src_idx >= (int)src_ch.size()) continue;

                cv::UMat weighted_src;
                cv::multiply(src_ch[src_idx](cv::Rect(0,0,out_size.width, out_size.height)), cv::Scalar::all(weight), weighted_src);

                if (it.second.IsSequence()) {
                    for (const auto& dest_node : it.second) {
                        int dst_idx = dest_node.as<int>();
                        if (dst_idx >= (int)out_ch.size()) continue;
                        cv::add(out_ch[dst_idx], weighted_src, out_ch[dst_idx]);
                    }
                }
            }
        };

        process_mapping(map_param["input0"], a_ch, alpha);
        process_mapping(map_param["input1"], b_ch, beta);

        if (gamma != 0.0) {
            for (auto& chan : out_ch) {
                cv::add(chan, cv::Scalar::all(gamma), chan);
            }
        }

        bool alpha_mapped = false;
        if (out_channels == 4 && map_param) {
            auto is_alpha_targeted = [&](const YAML::Node& n) {
                if (!n) return false;
                for (const auto& it : n) {
                    if (it.second.IsSequence()) {
                        for (const auto& dest : it.second) if (dest.as<int>() == 3) return true;
                    }
                }
                return false;
            };
            if (is_alpha_targeted(map_param["input0"]) || is_alpha_targeted(map_param["input1"])) {
                alpha_mapped = true;
            }
        }

        if (out_channels == 4 && !alpha_mapped) {
            cv::UMat alpha1 = (u_a_prep.channels() == 4) ? a_ch[3] : cv::UMat(u_a_prep.size(), CV_32F, cv::Scalar(1.0));
            cv::UMat alpha2 = (u_b_prep.channels() == 4) ? b_ch[3] : cv::UMat(u_b_prep.size(), CV_32F, cv::Scalar(1.0));
            cv::max(alpha1(cv::Rect(0,0,out_size.width, out_size.height)), 
                    alpha2(cv::Rect(0,0,out_size.width, out_size.height)), 
                    out_ch[3]);
        }
        
        cv::UMat blended_result;
        cv::merge(out_ch, blended_result);
        
        if (strategy == "crop") {
            u_out_mat = cv::UMat(u_a_prep.size(), blended_result.type(), cv::Scalar::all(0));
            blended_result.copyTo(u_out_mat(cv::Rect(0,0,out_size.width, out_size.height)));
        } else {
            u_out_mat = blended_result;
        }

    } else {
        normalize_channels_for_mixing_umat(u_a_prep, u_b_prep);
        if (strategy == "crop") {
            u_out_mat = cv::UMat::zeros(u_a_prep.size(), u_a_prep.type());
            cv::Rect roi = cv::Rect(0, 0, std::min(u_a_prep.cols, u_b_prep.cols), std::min(u_a_prep.rows, u_b_prep.rows));
            
            cv::UMat merged_roi;
            cv::addWeighted(u_a_prep(roi), alpha, u_b_prep(roi), beta, gamma, merged_roi);
            merged_roi.copyTo(u_out_mat(roi));
        } else { // resize
            if (u_a_prep.size() != u_b_prep.size()) {
                cv::resize(u_b_prep, u_b_prep, u_a_prep.size());
            }
            cv::addWeighted(u_a_prep, alpha, u_b_prep, beta, gamma, u_out_mat);
        }
    }
    
    NodeOutput result;
    result.image_umatrix = u_out_mat;
    return result;
}


static NodeOutput op_abs_diff(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.size() < 2 || inputs[0]->image_umatrix.empty() || inputs[1]->image_umatrix.empty()) throw GraphError("diff requires two input images");
    
    cv::UMat u_a_prep = inputs[0]->image_umatrix;
    cv::UMat u_b_prep = inputs[1]->image_umatrix;
    cv::UMat u_out_mat;

    std::string strategy = as_str(node.runtime_parameters, "merge_strategy", "resize");

    normalize_channels_for_mixing_umat(u_a_prep, u_b_prep);

    if (strategy == "crop") {
        u_out_mat = cv::UMat::zeros(u_a_prep.size(), u_a_prep.type());
        cv::Rect roi = cv::Rect(0, 0, std::min(u_a_prep.cols, u_b_prep.cols), std::min(u_a_prep.rows, u_b_prep.rows));
        
        cv::UMat diff_roi;
        cv::absdiff(u_a_prep(roi), u_b_prep(roi), diff_roi);
        diff_roi.copyTo(u_out_mat(roi));
    } else { // resize
        if (u_a_prep.size() != u_b_prep.size()) cv::resize(u_b_prep, u_b_prep, u_a_prep.size());
        cv::absdiff(u_a_prep, u_b_prep, u_out_mat);
    }
    NodeOutput result;
    result.image_umatrix = u_out_mat;
    return result;
}

static NodeOutput op_get_dimensions(const Node&, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty()) {
        throw GraphError("analyzer:get_dimensions requires one image input.");
    }
    const auto* input = inputs[0];
    if (input->image_umatrix.empty() && input->image_matrix.empty()) {
        throw GraphError("analyzer:get_dimensions input image is empty.");
    }

    NodeOutput out;
    if (!input->image_umatrix.empty()) {
        out.data["width"] = input->image_umatrix.cols;
        out.data["height"] = input->image_umatrix.rows;
    } else {
        out.data["width"] = input->image_matrix.cols;
        out.data["height"] = input->image_matrix.rows;
    }
    return out;
}
static NodeOutput op_divide(const Node& node, const std::vector<const NodeOutput*>&) {
    const auto& P = node.runtime_parameters;
    if (!P["operand1"] || !P["operand2"]) throw GraphError("math:divide requires 'operand1' and 'operand2'.");
    double op1 = P["operand1"].as<double>();
    double op2 = P["operand2"].as<double>();
    if (op2 == 0) throw GraphError("math:divide attempted to divide by zero.");
    NodeOutput out;
    out.data["result"] = op1 / op2;
    return out;
}

static NodeOutput op_extract_channel(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_umatrix.empty()) {
        throw GraphError("extract_channel requires one valid input image");
    }
    cv::UMat u_input = inputs[0]->image_umatrix;

    const auto& P = node.runtime_parameters;
    std::string channel_str = as_str(P, "channel", "a");
    int channel_idx = -1;

    if (channel_str == "b" || channel_str == "0") channel_idx = 0;
    else if (channel_str == "g" || channel_str == "1") channel_idx = 1;
    else if (channel_str == "r" || channel_str == "2") channel_idx = 2;
    else if (channel_str == "a" || channel_str == "3") channel_idx = 3;
    else throw GraphError("extract_channel: invalid 'channel' parameter. Use b,g,r,a or 0,1,2,3.");

    if (u_input.channels() <= channel_idx) {
        std::ostringstream err;
        err << "extract_channel: image has only " << u_input.channels() << " channel(s), cannot extract index " << channel_idx;
        throw GraphError(err.str());
    }

    std::vector<cv::UMat> channels;
    cv::split(u_input, channels);

    NodeOutput result;
    result.image_umatrix = channels[channel_idx];
    result.data["channel"] = channel_idx;
    return result;
}

static NodeOutput op_perlin_noise(const Node& node, const std::vector<const NodeOutput*>&) {
    const auto& P = node.runtime_parameters;
    int width = as_int_flexible(P, "width", 256);
    int height = as_int_flexible(P, "height", 256);
    double scale = as_double_flexible(P, "grid_size", 1.0);
    if (width <= 0 || height <= 0) throw GraphError("perlin_noise requires positive width and height");
    if (scale <= 0) throw GraphError("perlin_noise requires positive grid_size");

    std::vector<int> p(512);
    std::iota(p.begin(), p.begin() + 256, 0);
    std::shuffle(p.begin(), p.begin() + 256, std::mt19937{1});
    std::copy(p.begin(), p.begin() + 256, p.begin() + 256);

    auto fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    auto lerp = [](double t, double a, double b) { return a + t * (b - a); };
    
    auto grad = [](int hash, double x, double y) {
        switch(hash & 3) {
            case 0: return  x + y;
            case 1: return -x + y;
            case 2: return  x - y;
            case 3: return -x - y;
            default: return 0.0;
        }
    };

    auto noise = [&](double x, double y) {
        int X = static_cast<int>(floor(x)) & 255;
        int Y = static_cast<int>(floor(y)) & 255;
        x -= floor(x);
        y -= floor(y);
        double u = fade(x);
        double v = fade(y);
        int aa = p[p[X] + Y];
        int ab = p[p[X] + Y + 1];
        int ba = p[p[X + 1] + Y];
        int bb = p[p[X + 1] + Y + 1];
        
        double res = lerp(v, lerp(u, grad(aa, x, y), grad(ba, x - 1, y)),
                             lerp(u, grad(ab, x, y - 1), grad(bb, x - 1, y - 1)));
        
        return (res + 1.0) / 2.0;
    };

    cv::Mat noise_image(height, width, CV_32FC1);
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            double nx = static_cast<double>(j) / static_cast<double>(width) * scale;
            double ny = static_cast<double>(i) / static_cast<double>(height) * scale;
            noise_image.at<float>(i, j) = static_cast<float>(noise(nx, ny));
        }
    }
    
    NodeOutput result;
    result.image_matrix = noise_image;
    result.image_umatrix = noise_image.getUMat(cv::ACCESS_READ);
    return result;
}
static NodeOutput op_constant_image(const Node& node, const std::vector<const NodeOutput*>&) {
    const auto& P = node.runtime_parameters;
    int width = as_int_flexible(P, "width", 0);
    int height = as_int_flexible(P, "height", 0);
    int value_int = as_int_flexible(P, "value", 0);
    int channels = as_int_flexible(P, "channels", 1);
    if (width <= 0 || height <= 0) throw GraphError("image_generator:constant requires positive 'width' and 'height'.");
    float value_float = static_cast<float>(value_int) / 255.0f;
    cv::Scalar fill_value(value_float, value_float, value_float, value_float);
    cv::Mat out_mat(height, width, CV_MAKETYPE(CV_32F, channels), fill_value);
    NodeOutput result;
    result.image_matrix = out_mat;
    result.image_umatrix = out_mat.getUMat(cv::ACCESS_READ);
    return result;
}

static NodeOutput op_convolve(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.size() < 2 || inputs[0]->image_umatrix.empty() || inputs[1]->image_umatrix.empty()) {
        throw GraphError("convolve requires two input images: a source and a kernel.");
    }
    cv::UMat u_src = inputs[0]->image_umatrix;
    cv::UMat u_kernel_img = inputs[1]->image_umatrix;

    if (u_kernel_img.channels() != 1) {
        throw GraphError("The kernel for convolve must be a single-channel image.");
    }
    
    const auto& P = node.runtime_parameters;
    std::string padding_mode = as_str(P, "padding", "replicate");
    bool take_absolute = as_int_flexible(P, "absolute", 1) != 0;
    bool h_and_v = as_int_flexible(P, "horizontal_and_vertical", 0) != 0;
    int border_type = cv::BORDER_REPLICATE;
    if (padding_mode == "zero") border_type = cv::BORDER_CONSTANT;

    cv::UMat u_out_mat_float;

    if (h_and_v) {
        cv::UMat u_gx, u_gy, u_kernel_vertical;
        cv::filter2D(u_src, u_gx, CV_32F, u_kernel_img, cv::Point(-1,-1), 0, border_type);
        cv::transpose(u_kernel_img, u_kernel_vertical);
        cv::filter2D(u_src, u_gy, CV_32F, u_kernel_vertical, cv::Point(-1,-1), 0, border_type);
        cv::magnitude(u_gx, u_gy, u_out_mat_float);

    } else {
        cv::filter2D(u_src, u_out_mat_float, CV_32F, u_kernel_img, cv::Point(-1,-1), 0, border_type);
        if (take_absolute) {
            cv::absdiff(u_out_mat_float, cv::Scalar::all(0), u_out_mat_float);
        }
    }
    
    NodeOutput result;
    result.image_umatrix = u_out_mat_float;
    return result;
}

static NodeOutput op_curve_transform(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty() || inputs[0]->image_umatrix.empty()) {
        throw GraphError("curve_transform requires one input image.");
    }

    cv::UMat u_input = inputs[0]->image_umatrix;
    cv::UMat u_multiplied, u_added, u_out_mat;

    const auto& P = node.runtime_parameters;
    double k = as_double_flexible(P, "k", 1.0);

    cv::multiply(u_input, cv::Scalar::all(k), u_multiplied);
    cv::add(cv::Scalar::all(1.0), u_multiplied, u_added);
    cv::divide(1.0, u_added, u_out_mat);
    
    NodeOutput result;
    result.image_umatrix = u_out_mat;
    return result;
}

static NodeOutput op_multiply(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.size() < 2 || inputs[0]->image_umatrix.empty() || inputs[1]->image_umatrix.empty()) {
        throw GraphError("image_mixing:multiply requires two input images.");
    }
    cv::UMat u_a_prep = inputs[0]->image_umatrix;
    cv::UMat u_b_prep = inputs[1]->image_umatrix;
    cv::UMat u_out_mat_float;

    const auto& P = node.runtime_parameters;
    double scale = as_double_flexible(P, "scale", 1.0);
    std::string strategy = as_str(P, "merge_strategy", "resize");

    normalize_channels_for_mixing_umat(u_a_prep, u_b_prep);

    if (strategy == "crop") {
        u_out_mat_float = cv::UMat::zeros(u_a_prep.size(), u_a_prep.type());
        cv::Rect roi(0, 0, std::min(u_a_prep.cols, u_b_prep.cols), std::min(u_a_prep.rows, u_b_prep.rows));
        
        cv::UMat result_roi;
        cv::multiply(u_a_prep(roi), u_b_prep(roi), result_roi, scale);
        result_roi.copyTo(u_out_mat_float(roi));
    } else { // "resize"
        if (u_a_prep.size() != u_b_prep.size()) {
            cv::resize(u_b_prep, u_b_prep, u_a_prep.size());
        }
        cv::multiply(u_a_prep, u_b_prep, u_out_mat_float, scale);
    }

    NodeOutput result;
    result.image_umatrix = u_out_mat_float;
    return result;
}

void register_builtin() {
    auto& R = OpRegistry::instance();
    R.register_op("image_source", "path", op_image_source_path);
    R.register_op("image_generator", "perlin_noise", op_perlin_noise);
    R.register_op("image_generator", "constant", op_constant_image);
    R.register_op("image_process", "gaussian_blur", op_gaussian_blur);
    R.register_op("image_process", "resize", op_resize);
    R.register_op("image_process", "crop", op_crop);
    R.register_op("image_process", "extract_channel", op_extract_channel);
    R.register_op("image_process", "convolve", op_convolve);
    R.register_op("image_process", "curve_transform", op_curve_transform);
    R.register_op("image_mixing", "add_weighted", op_add_weighted);
    R.register_op("image_mixing", "diff", op_abs_diff);
    R.register_op("image_mixing", "multiply", op_multiply);
    R.register_op("analyzer", "get_dimensions", op_get_dimensions);
    R.register_op("math", "divide", op_divide);
}

}} // namespace ps::ops