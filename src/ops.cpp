// in: src/ops.cpp (REPLACE WITH THIS FINAL VERSION)
#include "kernel/ops.hpp"
#include "kernel/param_utils.hpp"
#include "adapter/buffer_adapter_opencv.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>
#include <random>
#include <cmath>
#include <numeric>
#include <mutex>

namespace ps { namespace ops {

// =============================================================================
// ==                         全局资源与辅助函数                           ==
// =============================================================================

// 全局互斥锁，用于保护所有并发的OpenCV GPU/CPU操作，防止底层库的资源竞争
static std::mutex g_opencv_op_mutex;
/*
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
*/
// =============================================================================
// ==                   类型一: MONOLITHIC (整体计算) 操作                      ==
// =============================================================================

static NodeOutput op_image_source_path(const Node& node, const std::vector<const NodeOutput*>&) {
    const auto& P = node.parameters;
    std::string path = as_str(P, "path");
    if (path.empty()) throw GraphError(GraphErrc::InvalidParameter, "image_source:path requires parameters.path");
    
    cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (img.empty()) throw GraphError(GraphErrc::Io, "Failed to read image: " + path);
    
    cv::Mat float_img;
    double scale = (img.depth() == CV_8U) ? 1.0/255.0 : ((img.depth() == CV_16U) ? 1.0/65535.0 : 1.0);
    img.convertTo(float_img, CV_32F, scale);

    NodeOutput result;
    result.image_buffer = fromCvMat(float_img);
    return result;
}

static NodeOutput op_constant_image(const Node& node, const std::vector<const NodeOutput*>&) {
    const auto& P = node.runtime_parameters;
    int width = as_int_flexible(P, "width", 256);
    int height = as_int_flexible(P, "height", 256);
    int value_int = as_int_flexible(P, "value", 0);
    int channels = as_int_flexible(P, "channels", 1);
    
    float value_float = static_cast<float>(value_int) / 255.0f;
    cv::Scalar fill_value(value_float, value_float, value_float, value_float);
    cv::Mat out_mat(height, width, CV_MAKETYPE(CV_32F, channels), fill_value);

    NodeOutput result;
    result.image_buffer = fromCvMat(out_mat);
    return result;
}

static NodeOutput op_perlin_noise(const Node& node, const std::vector<const NodeOutput*>&) {
    const auto& P = node.runtime_parameters;
    int width = as_int_flexible(P, "width", 256);
    int height = as_int_flexible(P, "height", 256);
    double scale = as_double_flexible(P, "grid_size", 1.0);
    int seed = as_int_flexible(P, "seed", -1);

    if (width <= 0 || height <= 0) throw GraphError(GraphErrc::InvalidParameter, "perlin_noise requires positive width and height");
    if (scale <= 0) throw GraphError(GraphErrc::InvalidParameter, "perlin_noise requires positive grid_size");

    std::vector<int> p(512);
    std::iota(p.begin(), p.begin() + 256, 0);
    
    std::mt19937 g;
    if (seed == -1) {
        g.seed(std::random_device{}());
    } else {
        g.seed(seed);
    }
    std::shuffle(p.begin(), p.begin() + 256, g);
    std::copy(p.begin(), p.begin() + 256, p.begin() + 256);

    auto fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    auto lerp = [](double t, double a, double b) { return a + t * (b - a); };
    auto grad = [](int hash, double x, double y) {
        switch(hash & 3) {
            case 0: return  x + y; case 1: return -x + y;
            case 2: return  x - y; case 3: return -x - y;
            default: return 0.0;
        }
    };
    auto noise = [&](double x, double y) {
        int X = static_cast<int>(floor(x)) & 255, Y = static_cast<int>(floor(y)) & 255;
        x -= floor(x); y -= floor(y);
        double u = fade(x), v = fade(y);
        int aa = p[p[X] + Y], ab = p[p[X] + Y + 1], ba = p[p[X + 1] + Y], bb = p[p[X + 1] + Y + 1];
        double res = lerp(v, lerp(u, grad(aa, x, y), grad(ba, x - 1, y)), lerp(u, grad(ab, x, y - 1), grad(bb, x - 1, y - 1)));
        return (res + 1.0) / 2.0;
    };

    cv::Mat noise_image(height, width, CV_32FC1);
    for (int i = 0; i < height; ++i) {
        for (int j = 0; j < width; ++j) {
            double nx = static_cast<double>(j) / width * scale;
            double ny = static_cast<double>(i) / height * scale;
            noise_image.at<float>(i, j) = static_cast<float>(noise(nx, ny));
        }
    }
    
    NodeOutput result;
    result.image_buffer = fromCvMat(noise_image);
    return result;
}

static NodeOutput op_convolve(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.size() < 2 || inputs[0]->image_buffer.width == 0 || inputs[1]->image_buffer.width == 0) {
        throw GraphError(GraphErrc::MissingDependency, "Convolve requires two input images: a source and a kernel.");
    }
    
    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
    
    cv::UMat u_src = toCvUMat(inputs[0]->image_buffer);
    cv::UMat u_kernel_img = toCvUMat(inputs[1]->image_buffer);

    if (u_kernel_img.channels() != 1) {
        throw GraphError(GraphErrc::InvalidParameter, "The kernel for convolve must be a single-channel image.");
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
    result.image_buffer = fromCvUMat(u_out_mat_float);
    return result;
}

static NodeOutput op_resize(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty()||inputs[0]->image_buffer.width==0) throw GraphError(GraphErrc::MissingDependency, "Resize requires an input image.");
    
    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
    cv::UMat u_input = toCvUMat(inputs[0]->image_buffer);

    const auto& P = node.runtime_parameters;
    int width = as_int_flexible(P, "width", 0), height = as_int_flexible(P, "height", 0);
    if(width<=0||height<=0) throw GraphError(GraphErrc::InvalidParameter, "Resize requires positive width and height.");
    std::string interp_str = as_str(P, "interpolation", "linear");
    int flag = (interp_str=="cubic")?cv::INTER_CUBIC:(interp_str=="nearest")?cv::INTER_NEAREST:(interp_str=="area")?cv::INTER_AREA:cv::INTER_LINEAR;
    cv::UMat u_output;
    cv::resize(u_input, u_output, cv::Size(width, height), 0, 0, flag);
    NodeOutput result; result.image_buffer=fromCvUMat(u_output); return result;
}

static NodeOutput op_crop(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty()||inputs[0]->image_buffer.width==0) throw GraphError(GraphErrc::MissingDependency, "Crop requires an input image.");

    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
    cv::UMat u_src = toCvUMat(inputs[0]->image_buffer);

    const auto& P = node.runtime_parameters;
    int x,y,w,h;
    if(as_str(P,"mode","value")=="ratio"){
        double rx=as_double_flexible(P,"x",-1), ry=as_double_flexible(P,"y",-1), rw=as_double_flexible(P,"width",-1), rh=as_double_flexible(P,"height",-1);
        if(rx<0||ry<0||rw<=0||rh<=0) throw GraphError(GraphErrc::InvalidParameter, "Crop ratio mode requires non-negative x,y and positive width,height.");
        x=rx*u_src.cols; y=ry*u_src.rows; w=rw*u_src.cols; h=rh*u_src.rows;
    }else{
        x=as_int_flexible(P,"x",-1); y=as_int_flexible(P,"y",-1); w=as_int_flexible(P,"width",-1); h=as_int_flexible(P,"height",-1);
        if(w<=0||h<=0) throw GraphError(GraphErrc::InvalidParameter, "Crop value mode requires positive width and height.");
    }
    cv::UMat u_canvas = cv::UMat::zeros(h,w,u_src.type());
    cv::Rect src_rect(0,0,u_src.cols,u_src.rows), crop_rect(x,y,w,h);
    cv::Rect intersect=src_rect&crop_rect;
    cv::Rect dst_roi(intersect.x-x,intersect.y-y,intersect.width,intersect.height);
    if(intersect.width>0&&intersect.height>0) u_src(intersect).copyTo(u_canvas(dst_roi));
    NodeOutput result; result.image_buffer=fromCvUMat(u_canvas); return result;
}

static NodeOutput op_extract_channel(const Node& node, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty()||inputs[0]->image_buffer.width==0) throw GraphError(GraphErrc::MissingDependency, "Extract channel requires an input image.");

    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
    cv::UMat u_input = toCvUMat(inputs[0]->image_buffer);

    std::string ch_str=as_str(node.runtime_parameters, "channel", "a");
    int ch_idx=-1;
    if(ch_str=="b"||ch_str=="0")ch_idx=0; else if(ch_str=="g"||ch_str=="1")ch_idx=1; else if(ch_str=="r"||ch_str=="2")ch_idx=2; else if(ch_str=="a"||ch_str=="3")ch_idx=3;
    if(ch_idx<0||u_input.channels()<=ch_idx) throw GraphError(GraphErrc::InvalidParameter, "Invalid or unavailable channel for extraction.");
    std::vector<cv::UMat> channels;
    cv::split(u_input, channels);
    NodeOutput result; result.image_buffer=fromCvUMat(channels[ch_idx]); return result;
}

static NodeOutput op_get_dimensions(const Node&, const std::vector<const NodeOutput*>& inputs) {
    if (inputs.empty()) throw GraphError(GraphErrc::MissingDependency, "analyzer:get_dimensions requires an image input.");
    const auto& input_buffer = inputs[0]->image_buffer;
    if (input_buffer.width == 0 || input_buffer.height == 0) throw GraphError(GraphErrc::MissingDependency, "analyzer:get_dimensions input image is empty.");

    NodeOutput out;
    out.data["width"] = input_buffer.width;
    out.data["height"] = input_buffer.height;
    return out;
}

static NodeOutput op_divide(const Node& node, const std::vector<const NodeOutput*>&) {
    const auto& P = node.runtime_parameters;
    if (!P["operand1"] || !P["operand2"]) throw GraphError(GraphErrc::InvalidParameter, "math:divide requires 'operand1' and 'operand2'.");
    double op1 = P["operand1"].as<double>();
    double op2 = P["operand2"].as<double>();
    if (op2 == 0) throw GraphError(GraphErrc::InvalidParameter, "math:divide attempted to divide by zero.");
    
    NodeOutput out;
    out.data["result"] = op1 / op2;
    return out;
}


// =============================================================================
// ==                       类型二: TILED (分块计算) 操作                         ==
// =============================================================================
static void op_curve_transform_tiled(const Node& node, const Tile& output_tile, const std::vector<Tile>& input_tiles) {
    // 修正: 加锁保护所有OpenCV操作
    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

    if (input_tiles.empty()) throw GraphError(GraphErrc::MissingDependency, "curve_transform requires one input tile.");

    cv::Mat input_mat = toCvMat(input_tiles[0]);
    cv::Mat output_mat = toCvMat(output_tile);

    const auto& P = node.runtime_parameters;
    double k = as_double_flexible(P, "k", 1.0);

    cv::Mat temp;
    cv::multiply(input_mat, cv::Scalar::all(k), temp);
    cv::add(cv::Scalar::all(1.0), temp, temp);
    cv::divide(1.0, temp, output_mat);
}

static void op_gaussian_blur_tiled(const Node& node, const Tile& output_tile, const std::vector<Tile>& input_tiles) {
    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

    if (input_tiles.empty()) {
        throw GraphError(GraphErrc::MissingDependency, "gaussian_blur requires one input tile with halo.");
    }

    const Tile& input_tile_with_halo = input_tiles[0];
    cv::Mat input_mat = toCvMat(input_tile_with_halo);
    cv::Mat output_mat = toCvMat(output_tile);

    const auto& P = node.runtime_parameters;
    int k = as_int_flexible(P, "ksize", 3);
    if (k > 0 && k % 2 == 0) k++;
    if (k <= 0) k = 1;
    double sigmaX = as_double_flexible(P, "sigmaX", 0.0);
    
    cv::Mat blurred_large_tile;
    // 使用 BORDER_REPLICATE 模式处理边缘，这对于小图像尤其重要
    cv::GaussianBlur(input_mat, blurred_large_tile, cv::Size(k, k), sigmaX, 0, cv::BORDER_REPLICATE);

    // --- 核心修复逻辑 ---
    // 旧的、错误的 valid_roi 计算方式:
    // int halo_size = k / 2;
    // cv::Rect valid_roi(halo_size, halo_size, output_mat.cols, output_mat.rows);

    // 新的、健壮的 valid_roi 计算方式：
    // 计算输出ROI在带光环的输入ROI中的相对偏移
    int offset_x = output_tile.roi.x - input_tile_with_halo.roi.x;
    int offset_y = output_tile.roi.y - input_tile_with_halo.roi.y;

    // 创建正确的 valid_roi
    cv::Rect valid_roi(offset_x, offset_y, output_mat.cols, output_mat.rows);

    // 安全检查，确保 valid_roi 不会超出模糊后图块的边界
    if (valid_roi.x < 0 || valid_roi.y < 0 ||
        valid_roi.x + valid_roi.width > blurred_large_tile.cols ||
        valid_roi.y + valid_roi.height > blurred_large_tile.rows) {
        throw std::runtime_error("Tiled Gaussian Blur: Catastrophic logic error, calculated valid ROI is still out of bounds.");
    }
    
    blurred_large_tile(valid_roi).copyTo(output_mat);
    // --- 修复结束 ---
}

static void op_add_weighted_tiled(const Node& node, const Tile& output_tile, const std::vector<Tile>& input_tiles) {
    // 修正: 加锁保护所有OpenCV操作
    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

    if (input_tiles.size() < 2) throw GraphError(GraphErrc::MissingDependency, "add_weighted requires two input tiles.");
    
    cv::Mat input_a = toCvMat(input_tiles[0]);
    cv::Mat input_b = toCvMat(input_tiles[1]);
    cv::Mat output = toCvMat(output_tile);

    const auto& P = node.runtime_parameters;
    double alpha = as_double_flexible(P, "alpha", 0.5);
    double beta  = as_double_flexible(P, "beta", 0.5);
    double gamma = as_double_flexible(P, "gamma", 0.0);

    cv::addWeighted(input_a, alpha, input_b, beta, gamma, output);
}

static void op_abs_diff_tiled(const Node& node, const Tile& output_tile, const std::vector<Tile>& input_tiles) {
    // 修正: 加锁保护所有OpenCV操作
    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

    if (input_tiles.size() < 2) throw GraphError(GraphErrc::MissingDependency, "diff requires two input tiles.");
    
    cv::Mat input_a = toCvMat(input_tiles[0]);
    cv::Mat input_b = toCvMat(input_tiles[1]);
    cv::Mat output = toCvMat(output_tile);
    cv::absdiff(input_a, input_b, output);
}

static void op_multiply_tiled(const Node& node, const Tile& output_tile, const std::vector<Tile>& input_tiles) {
    // 修正: 加锁保护所有OpenCV操作
    std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

    if (input_tiles.size() < 2) throw GraphError(GraphErrc::MissingDependency, "multiply requires two input tiles.");

    cv::Mat input_a = toCvMat(input_tiles[0]);
    cv::Mat input_b = toCvMat(input_tiles[1]);
    cv::Mat output = toCvMat(output_tile);
    
    const auto& P = node.runtime_parameters;
    double scale = as_double_flexible(P, "scale", 1.0);
    
    cv::multiply(input_a, input_b, output, scale);
}

// --- 注册所有操作 ---
void register_builtin() {
    auto& R = OpRegistry::instance();

    // 注册 Monolithic 操作
    R.register_op("image_source", "path", MonolithicOpFunc(op_image_source_path));
    R.register_op("image_generator", "constant", MonolithicOpFunc(op_constant_image));
    R.register_op("image_generator", "perlin_noise", MonolithicOpFunc(op_perlin_noise));
    R.register_op("analyzer", "get_dimensions", MonolithicOpFunc(op_get_dimensions));
    R.register_op("math", "divide", MonolithicOpFunc(op_divide));
    R.register_op("image_process", "convolve", MonolithicOpFunc(op_convolve));
    R.register_op("image_process", "resize", MonolithicOpFunc(op_resize)); 
    R.register_op("image_process", "crop", MonolithicOpFunc(op_crop)); 
    R.register_op("image_process", "extract_channel", MonolithicOpFunc(op_extract_channel)); 

    // 注册 Tiled 操作
    R.register_op("image_process", "gaussian_blur", TileOpFunc(op_gaussian_blur_tiled), {TileSizePreference::MACRO});
    R.register_op("image_process", "curve_transform", TileOpFunc(op_curve_transform_tiled), {TileSizePreference::MACRO});
    R.register_op("image_mixing", "add_weighted", TileOpFunc(op_add_weighted_tiled), {TileSizePreference::MACRO});
    R.register_op("image_mixing", "diff", TileOpFunc(op_abs_diff_tiled), {TileSizePreference::MACRO});
    R.register_op("image_mixing", "multiply", TileOpFunc(op_multiply_tiled), {TileSizePreference::MACRO});

}

}} // namespace ps::ops