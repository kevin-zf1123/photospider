// in: src/ops.cpp (REPLACE WITH THIS FINAL VERSION)
#include "kernel/ops.hpp"

#include <array>
#include <cmath>
#include <initializer_list>
#include <mutex>
#include <numeric>
#include <opencv2/core/ocl.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "graph_model.hpp"
#include "kernel/param_utils.hpp"

namespace ps {
namespace ops {

// =============================================================================
// ==                         全局资源与辅助函数                           ==
// =============================================================================

// 全局互斥锁，用于保护所有并发的OpenCV GPU/CPU操作，防止底层库的资源竞争
static std::mutex g_opencv_op_mutex;

static cv::Rect expand_roi(const cv::Rect& roi, int padding) {
  if (padding <= 0 || roi.width <= 0 || roi.height <= 0) {
    return roi;
  }
  return cv::Rect(roi.x - padding, roi.y - padding, roi.width + padding * 2,
                  roi.height + padding * 2);
}

static std::array<double, 9> multiply_matrix(const std::array<double, 9>& lhs,
                                             const std::array<double, 9>& rhs) {
  std::array<double, 9> out{};
  auto idx = [](int row, int col) { return row * 3 + col; };
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 3; ++k) {
        sum += lhs[idx(r, k)] * rhs[idx(k, c)];
      }
      out[idx(r, c)] = sum;
    }
  }
  return out;
}

static std::array<double, 9> make_scale_matrix(double sx, double sy) {
  return {sx, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, 1.0};
}

static std::array<double, 9> make_translation_matrix(double tx, double ty) {
  return {1.0, 0.0, tx, 0.0, 1.0, ty, 0.0, 0.0, 1.0};
}

static std::optional<int> node_param_int(const Node& node,
                                         const std::string& key) {
  if (node.runtime_parameters && node.runtime_parameters[key]) {
    try {
      return node.runtime_parameters[key].as<int>();
    } catch (...) {
      return std::nullopt;
    }
  }
  if (node.parameters && node.parameters[key]) {
    try {
      return node.parameters[key].as<int>();
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

static std::optional<double> node_param_double(const Node& node,
                                               const std::string& key) {
  if (node.runtime_parameters && node.runtime_parameters[key]) {
    try {
      return node.runtime_parameters[key].as<double>();
    } catch (...) {
      return std::nullopt;
    }
  }
  if (node.parameters && node.parameters[key]) {
    try {
      return node.parameters[key].as<double>();
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

static std::optional<std::string> node_param_string(const Node& node,
                                                    const std::string& key) {
  if (node.runtime_parameters && node.runtime_parameters[key]) {
    try {
      return node.runtime_parameters[key].as<std::string>();
    } catch (...) {
      return std::nullopt;
    }
  }
  if (node.parameters && node.parameters[key]) {
    try {
      return node.parameters[key].as<std::string>();
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

static cv::Size cached_image_size(const std::optional<NodeOutput>& output_opt) {
  if (!output_opt)
    return cv::Size();
  const auto& buf = output_opt->image_buffer;
  if (buf.width <= 0 || buf.height <= 0)
    return cv::Size();
  return cv::Size(buf.width, buf.height);
}

// Best-effort input extent without forcing execution; tries runtime hints,
// then parent cached outputs.
static cv::Size infer_input_size_hint(const Node& node,
                                      const GraphModel& graph) {
  if (node.last_input_size_hp && node.last_input_size_hp->width > 0 &&
      node.last_input_size_hp->height > 0) {
    return *node.last_input_size_hp;
  }
  for (const auto& input : node.image_inputs) {
    if (input.from_node_id < 0)
      continue;
    auto it = graph.nodes.find(input.from_node_id);
    if (it == graph.nodes.end())
      continue;
    const Node& parent = it->second;
    cv::Size size = cached_image_size(parent.cached_output_high_precision);
    if (size.width <= 0 || size.height <= 0)
      size = cached_image_size(parent.cached_output);
    if (size.width > 0 && size.height > 0)
      return size;
  }
  return cv::Size();
}

static int infer_radius_from_params(
    const Node& node, std::initializer_list<const char*> radius_keys,
    std::initializer_list<const char*> size_keys, int fallback = 0) {
  int radius = std::max(0, fallback);
  auto try_update = [&](std::optional<int> candidate) {
    if (candidate.has_value()) {
      radius = std::max(radius, std::max(0, *candidate));
    }
  };
  for (const char* key : radius_keys) {
    try_update(node_param_int(node, key));
  }
  for (const char* key : size_keys) {
    auto val = node_param_int(node, key);
    if (val.has_value()) {
      int computed = std::max(0, (*val - 1) / 2);
      radius = std::max(radius, computed);
    }
  }
  return radius;
}

static cv::Rect gaussian_blur_dirty_roi(const Node& node,
                                        const cv::Rect& downstream_roi,
                                        const GraphModel&) {
  int radius =
      infer_radius_from_params(node, {"radius", "kernel_radius"}, {"ksize"});
  if (radius <= 0)
    radius = 1;
  return expand_roi(downstream_roi, radius);
}

static cv::Rect convolve_dirty_roi(const Node& node,
                                   const cv::Rect& downstream_roi,
                                   const GraphModel&) {
  int radius = infer_radius_from_params(node, {"kernel_radius", "radius"},
                                        {"ksize", "kernel_size"});
  if (radius <= 0)
    radius = 1;
  return expand_roi(downstream_roi, radius);
}

static int interpolation_padding(const Node& node) {
  std::string interp =
      node_param_string(node, "interpolation").value_or("linear");
  if (interp == "nearest")
    return 0;
  if (interp == "cubic")
    return 2;
  if (interp == "area")
    return 1;
  return 1;  // linear and default
}

static cv::Rect resize_dirty_roi(const Node& node,
                                 const cv::Rect& downstream_roi,
                                 const GraphModel& graph) {
  if (downstream_roi.width <= 0 || downstream_roi.height <= 0) {
    return cv::Rect();
  }

  auto maybe_out_w = node_param_int(node, "width");
  auto maybe_out_h = node_param_int(node, "height");
  if (!maybe_out_w || !maybe_out_h || *maybe_out_w <= 0 || *maybe_out_h <= 0) {
    return cv::Rect();
  }

  const cv::Size input_size = infer_input_size_hint(node, graph);
  if (input_size.width <= 0 || input_size.height <= 0) {
    return cv::Rect();
  }

  const double scale_x =
      static_cast<double>(input_size.width) / static_cast<double>(*maybe_out_w);
  const double scale_y = static_cast<double>(input_size.height) /
                         static_cast<double>(*maybe_out_h);

  const int pad = interpolation_padding(node);
  const double left = static_cast<double>(downstream_roi.x) * scale_x -
                      static_cast<double>(pad);
  const double top = static_cast<double>(downstream_roi.y) * scale_y -
                     static_cast<double>(pad);
  const double right =
      static_cast<double>(downstream_roi.x + downstream_roi.width) * scale_x +
      static_cast<double>(pad);
  const double bottom =
      static_cast<double>(downstream_roi.y + downstream_roi.height) * scale_y +
      static_cast<double>(pad);

  int x0 = static_cast<int>(std::floor(left));
  int y0 = static_cast<int>(std::floor(top));
  int x1 = static_cast<int>(std::ceil(right));
  int y1 = static_cast<int>(std::ceil(bottom));

  cv::Rect upstream_roi(x0, y0, x1 - x0, y1 - y0);
  return upstream_roi & cv::Rect(0, 0, input_size.width, input_size.height);
}

static cv::Rect crop_dirty_roi(const Node& node, const cv::Rect& downstream_roi,
                               const GraphModel& graph) {
  if (downstream_roi.width <= 0 || downstream_roi.height <= 0) {
    return cv::Rect();
  }

  const cv::Size input_size = infer_input_size_hint(node, graph);
  const bool have_input_size = input_size.width > 0 && input_size.height > 0;
  const cv::Rect input_bounds =
      have_input_size ? cv::Rect(0, 0, input_size.width, input_size.height)
                      : cv::Rect();

  std::string mode = as_str(node.runtime_parameters, "mode", "");
  if (mode.empty())
    mode = as_str(node.parameters, "mode", "value");
  if (mode.empty())
    mode = "value";

  cv::Rect crop_rect;
  if (mode == "ratio") {
    if (!have_input_size) {
      return cv::Rect();
    }
    auto rx_opt = node_param_double(node, "x");
    auto ry_opt = node_param_double(node, "y");
    auto rw_opt = node_param_double(node, "width");
    auto rh_opt = node_param_double(node, "height");
    if (!rx_opt || !ry_opt || !rw_opt || !rh_opt) {
      return cv::Rect();
    }
    double rx = *rx_opt;
    double ry = *ry_opt;
    double rw = *rw_opt;
    double rh = *rh_opt;
    if (rx < 0.0 || ry < 0.0 || rw <= 0.0 || rh <= 0.0) {
      return cv::Rect();
    }
    int x = static_cast<int>(rx * input_size.width);
    int y = static_cast<int>(ry * input_size.height);
    int w = static_cast<int>(rw * input_size.width);
    int h = static_cast<int>(rh * input_size.height);
    if (w <= 0 || h <= 0) {
      return cv::Rect();
    }
    crop_rect = cv::Rect(x, y, w, h) & input_bounds;
    if (crop_rect.width <= 0 || crop_rect.height <= 0) {
      return cv::Rect();
    }
  } else {
    auto w_opt = node_param_int(node, "width");
    auto h_opt = node_param_int(node, "height");
    if (!w_opt || !h_opt || *w_opt <= 0 || *h_opt <= 0) {
      return cv::Rect();
    }
    int x = node_param_int(node, "x").value_or(0);
    int y = node_param_int(node, "y").value_or(0);
    crop_rect = cv::Rect(x, y, *w_opt, *h_opt);
    if (have_input_size) {
      crop_rect = crop_rect & input_bounds;
      if (crop_rect.width <= 0 || crop_rect.height <= 0) {
        return cv::Rect();
      }
    }
  }

  cv::Rect output_bounds(0, 0, crop_rect.width, crop_rect.height);
  cv::Rect valid_downstream_roi = downstream_roi & output_bounds;
  if (valid_downstream_roi.width <= 0 || valid_downstream_roi.height <= 0) {
    return cv::Rect();
  }

  cv::Rect upstream_roi(crop_rect.x + valid_downstream_roi.x,
                        crop_rect.y + valid_downstream_roi.y,
                        valid_downstream_roi.width,
                        valid_downstream_roi.height);
  return upstream_roi;
}

static cv::Rect gaussian_blur_forward_roi(const Node& node,
                                          const cv::Rect& upstream_roi,
                                          const GraphModel& graph,
                                          const cv::Size& parent_size,
                                          const cv::Size&) {
  (void)graph;
  if (upstream_roi.width <= 0 || upstream_roi.height <= 0) {
    return cv::Rect();
  }
  int radius =
      infer_radius_from_params(node, {"radius", "kernel_radius"}, {"ksize"});
  if (radius <= 0)
    radius = 1;
  return expand_roi(
      upstream_roi & cv::Rect(0, 0, parent_size.width, parent_size.height),
      radius);
}

static cv::Rect convolve_forward_roi(const Node& node,
                                     const cv::Rect& upstream_roi,
                                     const GraphModel& graph,
                                     const cv::Size& parent_size,
                                     const cv::Size&) {
  (void)graph;
  if (upstream_roi.width <= 0 || upstream_roi.height <= 0) {
    return cv::Rect();
  }
  int radius = infer_radius_from_params(node, {"kernel_radius", "radius"},
                                        {"ksize", "kernel_size"});
  if (radius <= 0)
    radius = 1;
  return expand_roi(
      upstream_roi & cv::Rect(0, 0, parent_size.width, parent_size.height),
      radius);
}

static cv::Rect resize_forward_roi(const Node& node,
                                   const cv::Rect& upstream_roi,
                                   const GraphModel&,
                                   const cv::Size& parent_size,
                                   const cv::Size& child_size) {
  if (upstream_roi.width <= 0 || upstream_roi.height <= 0) {
    return cv::Rect();
  }
  if (parent_size.width <= 0 || parent_size.height <= 0 ||
      child_size.width <= 0 || child_size.height <= 0) {
    return cv::Rect();
  }
  const double scale_x = static_cast<double>(child_size.width) /
                         static_cast<double>(parent_size.width);
  const double scale_y = static_cast<double>(child_size.height) /
                         static_cast<double>(parent_size.height);
  const int pad = interpolation_padding(node);

  const double left =
      static_cast<double>(upstream_roi.x) * scale_x - static_cast<double>(pad);
  const double top =
      static_cast<double>(upstream_roi.y) * scale_y - static_cast<double>(pad);
  const double right =
      static_cast<double>(upstream_roi.x + upstream_roi.width) * scale_x +
      static_cast<double>(pad);
  const double bottom =
      static_cast<double>(upstream_roi.y + upstream_roi.height) * scale_y +
      static_cast<double>(pad);

  int x0 = static_cast<int>(std::floor(left));
  int y0 = static_cast<int>(std::floor(top));
  int x1 = static_cast<int>(std::ceil(right));
  int y1 = static_cast<int>(std::ceil(bottom));

  return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

static cv::Rect crop_forward_roi(const Node& node, const cv::Rect& upstream_roi,
                                 const GraphModel&, const cv::Size& parent_size,
                                 const cv::Size&) {
  if (upstream_roi.width <= 0 || upstream_roi.height <= 0) {
    return cv::Rect();
  }
  std::string mode = node_param_string(node, "mode").value_or("value");
  cv::Rect crop_rect;
  if (mode == "ratio") {
    if (parent_size.width <= 0 || parent_size.height <= 0)
      return cv::Rect();
    auto rx = node_param_double(node, "x");
    auto ry = node_param_double(node, "y");
    auto rw = node_param_double(node, "width");
    auto rh = node_param_double(node, "height");
    if (!rx || !ry || !rw || !rh)
      return cv::Rect();
    int x = static_cast<int>(*rx * parent_size.width);
    int y = static_cast<int>(*ry * parent_size.height);
    int w = static_cast<int>(*rw * parent_size.width);
    int h = static_cast<int>(*rh * parent_size.height);
    if (w <= 0 || h <= 0)
      return cv::Rect();
    crop_rect = cv::Rect(x, y, w, h);
  } else {
    int x = node_param_int(node, "x").value_or(0);
    int y = node_param_int(node, "y").value_or(0);
    auto w = node_param_int(node, "width");
    auto h = node_param_int(node, "height");
    if (!w || !h || *w <= 0 || *h <= 0)
      return cv::Rect();
    crop_rect = cv::Rect(x, y, *w, *h);
  }

  cv::Rect intersect = upstream_roi &
                       cv::Rect(0, 0, parent_size.width, parent_size.height) &
                       crop_rect;
  if (intersect.width <= 0 || intersect.height <= 0) {
    return cv::Rect();
  }
  return cv::Rect(intersect.x - crop_rect.x, intersect.y - crop_rect.y,
                  intersect.width, intersect.height);
}

static cv::Rect identity_forward_roi(const Node&, const cv::Rect& upstream_roi,
                                     const GraphModel&, const cv::Size&,
                                     const cv::Size&) {
  return upstream_roi;
}

[[maybe_unused]] static cv::Rect conservative_dirty_roi(
    const Node& node, const cv::Rect& downstream_roi, const GraphModel&) {
  if (downstream_roi.width <= 0 || downstream_roi.height <= 0) {
    return cv::Rect();
  }
  int max_disp = 0;
  if (auto val = node_param_int(node, "max_displacement")) {
    max_disp = std::max(max_disp, *val);
  }
  if (auto brush = node_param_int(node, "radius")) {
    max_disp = std::max(max_disp, *brush);
  }
  if (max_disp <= 0) {
    return downstream_roi;
  }
  return expand_roi(downstream_roi, max_disp);
}

// =============================================================================
// ==                   类型一: MONOLITHIC (整体计算) 操作 ==
// =============================================================================

static NodeOutput op_image_source_path(const Node& node,
                                       const std::vector<const NodeOutput*>&) {
  const auto& P = node.parameters;
  std::string path = as_str(P, "path");
  if (path.empty())
    throw GraphError(GraphErrc::InvalidParameter,
                     "image_source:path requires parameters.path");

  cv::Mat img = cv::imread(path, cv::IMREAD_UNCHANGED);
  if (img.empty())
    throw GraphError(GraphErrc::Io, "Failed to read image: " + path);

  cv::Mat float_img;
  double scale = (img.depth() == CV_8U)
                     ? 1.0 / 255.0
                     : ((img.depth() == CV_16U) ? 1.0 / 65535.0 : 1.0);
  img.convertTo(float_img, CV_32F, scale);

  NodeOutput result;
  result.image_buffer = fromCvMat(float_img);
  return result;
}

static NodeOutput op_constant_image(const Node& node,
                                    const std::vector<const NodeOutput*>&) {
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

static NodeOutput op_perlin_noise(const Node& node,
                                  const std::vector<const NodeOutput*>&) {
  const auto& P = node.runtime_parameters;
  int width = as_int_flexible(P, "width", 256);
  int height = as_int_flexible(P, "height", 256);
  double scale = as_double_flexible(P, "grid_size", 1.0);
  int seed = as_int_flexible(P, "seed", -1);

  if (width <= 0 || height <= 0)
    throw GraphError(GraphErrc::InvalidParameter,
                     "perlin_noise requires positive width and height");
  if (scale <= 0)
    throw GraphError(GraphErrc::InvalidParameter,
                     "perlin_noise requires positive grid_size");

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
    switch (hash & 3) {
      case 0:
        return x + y;
      case 1:
        return -x + y;
      case 2:
        return x - y;
      case 3:
        return -x - y;
      default:
        return 0.0;
    }
  };
  auto noise = [&](double x, double y) {
    int X = static_cast<int>(floor(x)) & 255,
        Y = static_cast<int>(floor(y)) & 255;
    x -= floor(x);
    y -= floor(y);
    double u = fade(x), v = fade(y);
    int aa = p[p[X] + Y], ab = p[p[X] + Y + 1], ba = p[p[X + 1] + Y],
        bb = p[p[X + 1] + Y + 1];
    double res = lerp(v, lerp(u, grad(aa, x, y), grad(ba, x - 1, y)),
                      lerp(u, grad(ab, x, y - 1), grad(bb, x - 1, y - 1)));
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

static NodeOutput op_convolve(const Node& node,
                              const std::vector<const NodeOutput*>& inputs) {
  // Defensive checks against cold-start/invalid inputs
  if (inputs.size() < 2 || inputs[0]->image_buffer.width == 0 ||
      inputs[0]->image_buffer.height == 0 ||
      inputs[1]->image_buffer.width == 0 ||
      inputs[1]->image_buffer.height == 0) {
    throw GraphError(
        GraphErrc::MissingDependency,
        "Convolve requires two non-empty input images (src and kernel).");
  }

  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

  // Use Mat-only pipeline to avoid UMat/OpenCL first-use quirks under parallel
  // cold-start
  cv::Mat src = toCvMat(inputs[0]->image_buffer);
  cv::Mat kernel = toCvMat(inputs[1]->image_buffer);

  if (src.empty())
    throw GraphError(GraphErrc::MissingDependency,
                     "Convolve source image is empty.");
  if (kernel.empty())
    throw GraphError(GraphErrc::MissingDependency,
                     "Convolve kernel image is empty.");

  // Normalize types: grayscale kernel, float32 compute
  if (kernel.channels() != 1) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "The kernel for convolve must be single-channel.");
  }
  if (src.depth() != CV_32F)
    src.convertTo(src, CV_32F);
  if (kernel.depth() != CV_32F)
    kernel.convertTo(kernel, CV_32F);

  // Optional border/taking absolute and dual-direction gradient
  const auto& P = node.runtime_parameters;
  std::string padding_mode = as_str(P, "padding", "replicate");
  bool take_absolute = as_int_flexible(P, "absolute", 1) != 0;
  bool h_and_v = as_int_flexible(P, "horizontal_and_vertical", 0) != 0;

  int border_type = cv::BORDER_REPLICATE;
  if (padding_mode == "zero")
    border_type = cv::BORDER_CONSTANT;

  cv::Mat out_f32;
  try {
    if (h_and_v) {
      cv::Mat gx, gy, kT;
      cv::filter2D(src, gx, CV_32F, kernel, cv::Point(-1, -1), 0, border_type);
      cv::transpose(kernel, kT);
      cv::filter2D(src, gy, CV_32F, kT, cv::Point(-1, -1), 0, border_type);
      cv::magnitude(gx, gy, out_f32);
      if (take_absolute)
        cv::absdiff(out_f32, cv::Scalar::all(0), out_f32);
    } else {
      cv::filter2D(src, out_f32, CV_32F, kernel, cv::Point(-1, -1), 0,
                   border_type);
      if (take_absolute)
        cv::absdiff(out_f32, cv::Scalar::all(0), out_f32);
    }
  } catch (const cv::Exception& e) {
    throw GraphError(GraphErrc::ComputeError,
                     std::string("Convolve failed: ") + e.what());
  }

  NodeOutput result;
  result.image_buffer = fromCvMat(out_f32);
  return result;
}

static NodeOutput op_resize(const Node& node,
                            const std::vector<const NodeOutput*>& inputs) {
  if (inputs.empty() || inputs[0]->image_buffer.width == 0)
    throw GraphError(GraphErrc::MissingDependency,
                     "Resize requires an input image.");

  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
  cv::UMat u_input = toCvUMat(inputs[0]->image_buffer);

  const auto& P = node.runtime_parameters;
  int width = as_int_flexible(P, "width", 0),
      height = as_int_flexible(P, "height", 0);
  if (width <= 0 || height <= 0)
    throw GraphError(GraphErrc::InvalidParameter,
                     "Resize requires positive width and height.");
  std::string interp_str = as_str(P, "interpolation", "linear");
  int flag = (interp_str == "cubic")     ? cv::INTER_CUBIC
             : (interp_str == "nearest") ? cv::INTER_NEAREST
             : (interp_str == "area")    ? cv::INTER_AREA
                                         : cv::INTER_LINEAR;
  cv::UMat u_output;
  cv::resize(u_input, u_output, cv::Size(width, height), 0, 0, flag);
  NodeOutput result;
  result.image_buffer = fromCvUMat(u_output);
  const auto& in_space = inputs[0]->space;
  result.space = in_space;
  const int in_w = inputs[0]->image_buffer.width;
  const int in_h = inputs[0]->image_buffer.height;
  double scale_x =
      (in_w > 0) ? static_cast<double>(width) / static_cast<double>(in_w) : 1.0;
  double scale_y = (in_h > 0)
                       ? static_cast<double>(height) / static_cast<double>(in_h)
                       : 1.0;
  if (scale_x <= 0.0)
    scale_x = 1.0;
  if (scale_y <= 0.0)
    scale_y = 1.0;
  result.space.global_scale_x = in_space.global_scale_x * scale_x;
  result.space.global_scale_y = in_space.global_scale_y * scale_y;
  auto scale_mat = make_scale_matrix(scale_x, scale_y);
  auto inv_scale_mat = make_scale_matrix(1.0 / scale_x, 1.0 / scale_y);
  result.space.transform_matrix =
      multiply_matrix(scale_mat, in_space.transform_matrix);
  result.space.inverse_matrix =
      multiply_matrix(in_space.inverse_matrix, inv_scale_mat);
  result.space.local_inverse_matrix = inv_scale_mat;
  if (in_space.absolute_roi.width > 0 && in_space.absolute_roi.height > 0) {
    result.space.absolute_roi = in_space.absolute_roi;
  }
  return result;
}

static NodeOutput op_crop(const Node& node,
                          const std::vector<const NodeOutput*>& inputs) {
  if (inputs.empty() || inputs[0]->image_buffer.width == 0)
    throw GraphError(GraphErrc::MissingDependency,
                     "Crop requires an input image.");

  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
  cv::UMat u_src = toCvUMat(inputs[0]->image_buffer);

  const auto& P = node.runtime_parameters;
  int x, y, w, h;
  if (as_str(P, "mode", "value") == "ratio") {
    double rx = as_double_flexible(P, "x", -1),
           ry = as_double_flexible(P, "y", -1),
           rw = as_double_flexible(P, "width", -1),
           rh = as_double_flexible(P, "height", -1);
    if (rx < 0 || ry < 0 || rw <= 0 || rh <= 0)
      throw GraphError(GraphErrc::InvalidParameter,
                       "Crop ratio mode requires non-negative x,y and positive "
                       "width,height.");
    x = rx * u_src.cols;
    y = ry * u_src.rows;
    w = rw * u_src.cols;
    h = rh * u_src.rows;
  } else {
    x = as_int_flexible(P, "x", -1);
    y = as_int_flexible(P, "y", -1);
    w = as_int_flexible(P, "width", -1);
    h = as_int_flexible(P, "height", -1);
    if (w <= 0 || h <= 0)
      throw GraphError(GraphErrc::InvalidParameter,
                       "Crop value mode requires positive width and height.");
  }
  cv::UMat u_canvas = cv::UMat::zeros(h, w, u_src.type());
  cv::Rect src_rect(0, 0, u_src.cols, u_src.rows), crop_rect(x, y, w, h);
  cv::Rect intersect = src_rect & crop_rect;
  cv::Rect dst_roi(intersect.x - x, intersect.y - y, intersect.width,
                   intersect.height);
  if (intersect.width > 0 && intersect.height > 0)
    u_src(intersect).copyTo(u_canvas(dst_roi));
  NodeOutput result;
  result.image_buffer = fromCvUMat(u_canvas);
  const auto& in_space = inputs[0]->space;
  result.space = in_space;
  auto translation =
      make_translation_matrix(-static_cast<double>(x), -static_cast<double>(y));
  auto inv_translation =
      make_translation_matrix(static_cast<double>(x), static_cast<double>(y));
  result.space.transform_matrix =
      multiply_matrix(translation, in_space.transform_matrix);
  result.space.inverse_matrix =
      multiply_matrix(in_space.inverse_matrix, inv_translation);
  result.space.local_inverse_matrix = inv_translation;
  result.space.global_scale_x = in_space.global_scale_x;
  result.space.global_scale_y = in_space.global_scale_y;
  if (in_space.absolute_roi.width > 0 && in_space.absolute_roi.height > 0) {
    cv::Rect parent_world = in_space.absolute_roi;
    cv::Rect world_request(parent_world.x + x, parent_world.y + y, w, h);
    cv::Rect clipped = world_request & parent_world;
    if (clipped.width > 0 && clipped.height > 0) {
      result.space.absolute_roi = clipped;
    } else {
      result.space.absolute_roi = world_request;
    }
  } else {
    result.space.absolute_roi = cv::Rect(x, y, w, h);
  }
  return result;
}

static NodeOutput op_extract_channel(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  if (inputs.empty() || inputs[0]->image_buffer.width == 0)
    throw GraphError(GraphErrc::MissingDependency,
                     "Extract channel requires an input image.");

  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
  cv::UMat u_input = toCvUMat(inputs[0]->image_buffer);

  std::string ch_str = as_str(node.runtime_parameters, "channel", "a");
  int ch_idx = -1;
  if (ch_str == "b" || ch_str == "0")
    ch_idx = 0;
  else if (ch_str == "g" || ch_str == "1")
    ch_idx = 1;
  else if (ch_str == "r" || ch_str == "2")
    ch_idx = 2;
  else if (ch_str == "a" || ch_str == "3")
    ch_idx = 3;
  if (ch_idx < 0 || u_input.channels() <= ch_idx)
    throw GraphError(GraphErrc::InvalidParameter,
                     "Invalid or unavailable channel for extraction.");
  std::vector<cv::UMat> channels;
  cv::split(u_input, channels);
  NodeOutput result;
  result.image_buffer = fromCvUMat(channels[ch_idx]);
  return result;
}

static NodeOutput op_get_dimensions(
    const Node&, const std::vector<const NodeOutput*>& inputs) {
  if (inputs.empty())
    throw GraphError(GraphErrc::MissingDependency,
                     "analyzer:get_dimensions requires an image input.");
  const auto& input_buffer = inputs[0]->image_buffer;
  if (input_buffer.width == 0 || input_buffer.height == 0)
    throw GraphError(GraphErrc::MissingDependency,
                     "analyzer:get_dimensions input image is empty.");

  NodeOutput out;
  out.data["width"] = input_buffer.width;
  out.data["height"] = input_buffer.height;
  return out;
}

static NodeOutput op_divide(const Node& node,
                            const std::vector<const NodeOutput*>&) {
  const auto& P = node.runtime_parameters;
  if (!P["operand1"] || !P["operand2"])
    throw GraphError(GraphErrc::InvalidParameter,
                     "math:divide requires 'operand1' and 'operand2'.");
  double op1 = P["operand1"].as<double>();
  double op2 = P["operand2"].as<double>();
  if (op2 == 0)
    throw GraphError(GraphErrc::InvalidParameter,
                     "math:divide attempted to divide by zero.");

  NodeOutput out;
  out.data["result"] = op1 / op2;
  return out;
}

// =============================================================================
// ==                       类型二: TILED (分块计算) 操作 ==
// =============================================================================
static void op_curve_transform_tiled(const Node& node, const Tile& output_tile,
                                     const std::vector<Tile>& input_tiles) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

  if (input_tiles.empty())
    throw GraphError(GraphErrc::MissingDependency,
                     "curve_transform requires one input tile.");

  cv::Mat input_mat = toCvMat(input_tiles[0]);
  cv::Mat output_mat = toCvMat(output_tile);

  const auto& P = node.runtime_parameters;
  double k = as_double_flexible(P, "k", 1.0);

  cv::Mat temp;
  cv::multiply(input_mat, cv::Scalar::all(k), temp);
  cv::add(cv::Scalar::all(1.0), temp, temp);
  cv::divide(1.0, temp, output_mat);
}

static void op_gaussian_blur_tiled(const Node& node, const Tile& output_tile,
                                   const std::vector<Tile>& input_tiles) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

  if (input_tiles.empty()) {
    throw GraphError(GraphErrc::MissingDependency,
                     "gaussian_blur requires one input tile with halo.");
  }

  const Tile& input_tile_with_halo = input_tiles[0];
  cv::Mat input_mat = toCvMat(input_tile_with_halo);
  cv::Mat output_mat = toCvMat(output_tile);

  const auto& P = node.runtime_parameters;
  int k = as_int_flexible(P, "ksize", 3);
  if (k > 0 && k % 2 == 0)
    k++;
  if (k <= 0)
    k = 1;
  double sigmaX = as_double_flexible(P, "sigmaX", 0.0);

  cv::Mat blurred_large_tile;
  cv::GaussianBlur(input_mat, blurred_large_tile, cv::Size(k, k), sigmaX, 0,
                   cv::BORDER_REPLICATE);

  int offset_x = output_tile.roi.x - input_tile_with_halo.roi.x;
  int offset_y = output_tile.roi.y - input_tile_with_halo.roi.y;

  cv::Rect valid_roi(offset_x, offset_y, output_mat.cols, output_mat.rows);

  if (valid_roi.x < 0 || valid_roi.y < 0 ||
      valid_roi.x + valid_roi.width > blurred_large_tile.cols ||
      valid_roi.y + valid_roi.height > blurred_large_tile.rows) {
    throw std::runtime_error(
        "Tiled Gaussian Blur: Catastrophic logic error, calculated valid ROI "
        "is out of bounds.");
  }

  blurred_large_tile(valid_roi).copyTo(output_mat);
}

// New: Monolithic Gaussian Blur (restores milestone-1 performance for
// full-image blur)
static NodeOutput op_gaussian_blur_monolithic(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
  if (inputs.empty()) {
    throw GraphError(GraphErrc::MissingDependency,
                     "gaussian_blur requires one input image.");
  }
  const auto& in_buf = inputs[0]->image_buffer;
  cv::Mat input_mat = toCvMat(in_buf);

  const auto& P = node.runtime_parameters;
  int k = as_int_flexible(P, "ksize", 3);
  if (k > 0 && k % 2 == 0)
    k++;
  if (k <= 0)
    k = 1;
  double sigmaX = as_double_flexible(P, "sigmaX", 0.0);

  cv::Mat output_mat;
  cv::GaussianBlur(input_mat, output_mat, cv::Size(k, k), sigmaX, 0,
                   cv::BORDER_REPLICATE);

  NodeOutput result;
  result.image_buffer = fromCvMat(output_mat);
  return result;
}

static void op_add_weighted_tiled(const Node& node, const Tile& output_tile,
                                  const std::vector<Tile>& input_tiles) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

  if (input_tiles.size() < 2)
    throw GraphError(GraphErrc::MissingDependency,
                     "add_weighted requires two input tiles.");

  cv::Mat input_a = toCvMat(input_tiles[0]);
  cv::Mat input_b = toCvMat(input_tiles[1]);
  cv::Mat output = toCvMat(output_tile);

  // [健壮性修复] 添加断言
  if (input_a.size() != input_b.size() ||
      input_a.channels() != input_b.channels()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "add_weighted tiled inputs must have same dimensions "
                     "after merge strategy.");
  }

  const auto& P = node.runtime_parameters;
  double alpha = as_double_flexible(P, "alpha", 0.5);
  double beta = as_double_flexible(P, "beta", 0.5);
  double gamma = as_double_flexible(P, "gamma", 0.0);

  // Optional per-channel mapping: parameters.channel_mapping.input0 / input1: {
  // src_channel: [dst_channels] }
  YAML::Node chmap = P["channel_mapping"];
  bool has_mapping = chmap && (chmap.IsMap());

  if (!has_mapping) {
    // Default: weighted blend per channel
    cv::addWeighted(input_a, alpha, input_b, beta, gamma, output);
    return;
  }

  // Prepare output planes
  int in_ch = input_a.channels();
  int out_ch = in_ch;

  auto infer_max_dest = [&](const YAML::Node& n) -> int {
    int m = -1;
    if (!n || !n.IsMap())
      return -1;
    for (auto it = n.begin(); it != n.end(); ++it) {
      // Use value copies of YAML::Node to avoid lifetime issues with
      // temporaries
      YAML::Node dsts = it->second;
      if (dsts && dsts.IsSequence()) {
        for (size_t i = 0; i < dsts.size(); ++i) {
          try {
            m = std::max(m, dsts[i].as<int>());
          } catch (...) {
          }
        }
      }
    }
    return m;
  };
  // Avoid passing temporaries into lambdas that iterate; store as lvalues first
  // to prevent any stack lifetime ambiguity observed under ASan with yaml-cpp
  // temporaries.
  YAML::Node ch_input0 = chmap["input0"];
  YAML::Node ch_input1 = chmap["input1"];

  int max0 = infer_max_dest(ch_input0);
  int max1 = infer_max_dest(ch_input1);
  int maxd = std::max({max0, max1, in_ch - 1});
  out_ch = std::max(out_ch, maxd + 1);

  // Split inputs into planes
  std::vector<cv::Mat> A, B;
  cv::split(input_a, A);
  cv::split(input_b, B);
  // Ensure plane count
  if ((int)A.size() < out_ch)
    A.resize(out_ch, cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1));
  if ((int)B.size() < out_ch)
    B.resize(out_ch, cv::Mat::zeros(input_b.rows, input_b.cols, CV_32FC1));

  // Init out planes: default weighted result; mapped destinations will be
  // overridden later
  std::vector<cv::Mat> O(out_ch);
  for (int c = 0; c < out_ch; ++c) {
    O[c] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    if (c < in_ch) {
      cv::addWeighted(A[c], alpha, B[c], beta, gamma, O[c]);
    } else {
      if (gamma != 0.0)
        O[c] = cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(gamma));
    }
  }

  // Build covered destination set; override defaults for covered channels
  std::vector<char> covered(out_ch, 0);
  auto mark_covered = [&](const YAML::Node& n) {
    if (!n || !n.IsMap())
      return;
    for (auto it = n.begin(); it != n.end(); ++it) {
      YAML::Node dsts = it->second;
      if (!dsts || !dsts.IsSequence())
        continue;
      for (size_t i = 0; i < dsts.size(); ++i) {
        int d = -1;
        try {
          d = dsts[i].as<int>();
        } catch (...) {
          continue;
        }
        if (d >= 0 && d < out_ch)
          covered[d] = 1;
      }
    }
  };
  mark_covered(ch_input0);
  mark_covered(ch_input1);
  for (int d = 0; d < out_ch; ++d) {
    if (covered[d]) {
      if (gamma != 0.0)
        O[d] = cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(gamma));
      else
        O[d] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    }
  }

  auto apply_map = [&](const YAML::Node& n,
                       const std::vector<cv::Mat>& src_planes, double w) {
    if (!n || !n.IsMap())
      return;
    for (auto it = n.begin(); it != n.end(); ++it) {
      int src = -1;
      try {
        src = it->first.as<int>();
      } catch (...) {
        continue;
      }
      YAML::Node dsts = it->second;
      if (!dsts || !dsts.IsSequence())
        continue;
      for (size_t i = 0; i < dsts.size(); ++i) {
        int d = -1;
        try {
          d = dsts[i].as<int>();
        } catch (...) {
          continue;
        }
        if (src < 0 || src >= (int)src_planes.size())
          continue;
        if (d < 0 || d >= (int)O.size())
          continue;
        // O[d] := O[d] + w*src
        cv::add(O[d], src_planes[src] * w, O[d]);
      }
    }
  };

  apply_map(ch_input0, A, alpha);
  apply_map(ch_input1, B, beta);

  // Optional alpha_strategy for dest alpha channel
  std::string alpha_strategy = as_str(P, "alpha_strategy", "weighted");
  if ((alpha_strategy != "weighted") && out_ch >= 4) {
    int aidx = 3;
    if (alpha_strategy == "max") {
      O[aidx] =
          cv::max(A.size() > 3 ? A[3] : O[aidx], B.size() > 3 ? B[3] : O[aidx]);
    } else if (alpha_strategy == "copy0") {
      if ((int)A.size() > 3)
        O[aidx] = A[3].clone();
    } else if (alpha_strategy == "copy1") {
      if ((int)B.size() > 3)
        O[aidx] = B[3].clone();
    } else if (alpha_strategy == "set1") {
      O[aidx] = cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(1.0));
    } else if (alpha_strategy == "set0") {
      O[aidx] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    }
  }

  cv::merge(O, output);
}

static void op_abs_diff_tiled(const Node& node, const Tile& output_tile,
                              const std::vector<Tile>& input_tiles) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

  if (input_tiles.size() < 2)
    throw GraphError(GraphErrc::MissingDependency,
                     "diff requires two input tiles.");

  cv::Mat input_a = toCvMat(input_tiles[0]);
  cv::Mat input_b = toCvMat(input_tiles[1]);
  cv::Mat output = toCvMat(output_tile);

  // [健壮性修复] 添加断言
  if (input_a.size() != input_b.size() ||
      input_a.channels() != input_b.channels()) {
    throw GraphError(
        GraphErrc::InvalidParameter,
        "diff tiled inputs must have same dimensions after merge strategy.");
  }

  // Handle alpha separately to avoid transparent result when subtracting alpha
  const auto& P = node.runtime_parameters;
  std::string alpha_strategy = as_str(
      P, "alpha_strategy", "copy0");  // copy0|copy1|max|min|diff|set1|set0

  int ch = input_a.channels();
  if (ch < 2) {
    cv::absdiff(input_a, input_b, output);
    return;
  }

  std::vector<cv::Mat> Aa, Bb, Oo;
  cv::split(input_a, Aa);
  cv::split(input_b, Bb);
  Oo.resize(ch);

  int color_channels = (ch == 4) ? 3 : ch;
  for (int c = 0; c < color_channels; ++c) {
    cv::absdiff(Aa[c], Bb[c], Oo[c]);
  }
  if (ch == 4) {
    if (alpha_strategy == "copy0") {
      Oo[3] = Aa[3].clone();
    } else if (alpha_strategy == "copy1") {
      Oo[3] = Bb[3].clone();
    } else if (alpha_strategy == "max") {
      Oo[3] = cv::max(Aa[3], Bb[3]);
    } else if (alpha_strategy == "min") {
      Oo[3] = cv::min(Aa[3], Bb[3]);
    } else if (alpha_strategy == "diff") {
      cv::absdiff(Aa[3], Bb[3], Oo[3]);
    } else if (alpha_strategy == "set1") {
      Oo[3] = cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(1.0));
    } else if (alpha_strategy == "set0") {
      Oo[3] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    } else {
      // default copy0
      Oo[3] = Aa[3].clone();
    }
  }
  cv::merge(Oo, output);
}

static void op_multiply_tiled(const Node& node, const Tile& output_tile,
                              const std::vector<Tile>& input_tiles) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);

  if (input_tiles.size() < 2)
    throw GraphError(GraphErrc::MissingDependency,
                     "multiply requires two input tiles.");

  cv::Mat input_a = toCvMat(input_tiles[0]);
  cv::Mat input_b = toCvMat(input_tiles[1]);
  cv::Mat output = toCvMat(output_tile);

  // [健壮性修复] 添加断言
  if (input_a.size() != input_b.size() ||
      input_a.channels() != input_b.channels()) {
    throw GraphError(GraphErrc::InvalidParameter,
                     "multiply tiled inputs must have same dimensions after "
                     "merge strategy.");
  }

  const auto& P = node.runtime_parameters;
  double scale = as_double_flexible(P, "scale", 1.0);

  cv::multiply(input_a, input_b, output, scale);
}

// -------------------- Monolithic image_mixing implementations
// --------------------

static void normalize_to_base(cv::Mat& current_mat, const cv::Mat& base_mat,
                              const std::string& strategy) {
  // Resize or crop/pad current_mat to match base_mat's size
  if (current_mat.size() != base_mat.size()) {
    if (strategy == "resize") {
      cv::resize(current_mat, current_mat, base_mat.size(), 0, 0,
                 cv::INTER_LINEAR);
    } else if (strategy == "crop") {
      cv::Rect roi(0, 0, std::min(current_mat.cols, base_mat.cols),
                   std::min(current_mat.rows, base_mat.rows));
      cv::Mat canvas =
          cv::Mat::zeros(base_mat.rows, base_mat.cols, current_mat.type());
      current_mat(roi).copyTo(canvas(roi));
      current_mat = canvas;
    } else {
      throw GraphError(GraphErrc::InvalidParameter,
                       "Unsupported merge_strategy '" + strategy +
                           "' for monolithic image_mixing.");
    }
  }
  // Match channel count to base
  if (current_mat.channels() != base_mat.channels()) {
    int bc = base_mat.channels();
    if (current_mat.channels() == 1 && (bc == 3 || bc == 4)) {
      std::vector<cv::Mat> planes(bc, current_mat);
      cv::merge(planes, current_mat);
    } else if ((current_mat.channels() == 3 || current_mat.channels() == 4) &&
               bc == 1) {
      cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2GRAY);
    } else if (current_mat.channels() == 4 && bc == 3) {
      cv::cvtColor(current_mat, current_mat, cv::COLOR_BGRA2BGR);
    } else if (current_mat.channels() == 3 && bc == 4) {
      cv::cvtColor(current_mat, current_mat, cv::COLOR_BGR2BGRA);
    } else {
      throw GraphError(
          GraphErrc::InvalidParameter,
          "Unsupported channel conversion for monolithic image_mixing: " +
              std::to_string(current_mat.channels()) + " -> " +
              std::to_string(bc));
    }
  }
}

static NodeOutput op_add_weighted_monolithic(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
  if (inputs.size() < 2)
    throw GraphError(GraphErrc::MissingDependency,
                     "add_weighted requires two input images.");

  cv::Mat input_a = toCvMat(inputs[0]->image_buffer);
  cv::Mat input_b = toCvMat(inputs[1]->image_buffer);
  if (input_a.empty() || input_b.empty())
    throw GraphError(GraphErrc::MissingDependency,
                     "add_weighted inputs must be non-empty.");

  const auto& P = node.runtime_parameters;
  double alpha = as_double_flexible(P, "alpha", 0.5);
  double beta = as_double_flexible(P, "beta", 0.5);
  double gamma = as_double_flexible(P, "gamma", 0.0);
  std::string strategy = as_str(P, "merge_strategy", "resize");

  normalize_to_base(input_b, input_a, strategy);

  // Optional per-channel mapping
  YAML::Node chmap = P["channel_mapping"];
  bool has_mapping = chmap && chmap.IsMap();

  cv::Mat output(input_a.rows, input_a.cols,
                 CV_MAKETYPE(CV_32F, input_a.channels()));
  if (!has_mapping) {
    cv::addWeighted(input_a, alpha, input_b, beta, gamma, output);
  } else {
    int in_ch = input_a.channels();
    int out_ch = in_ch;
    auto infer_max_dest = [&](const YAML::Node& n) -> int {
      int m = -1;
      if (!n || !n.IsMap())
        return -1;
      for (auto it = n.begin(); it != n.end(); ++it) {
        YAML::Node dsts = it->second;
        if (!dsts || !dsts.IsSequence())
          continue;
        for (size_t i = 0; i < dsts.size(); ++i) {
          try {
            m = std::max(m, dsts[i].as<int>());
          } catch (...) {
          }
        }
      }
      return m;
    };
    YAML::Node ch_input0 = chmap["input0"], ch_input1 = chmap["input1"];
    int max0 = infer_max_dest(ch_input0);
    int max1 = infer_max_dest(ch_input1);
    int maxd = std::max({max0, max1, in_ch - 1});
    out_ch = std::max(out_ch, maxd + 1);

    std::vector<cv::Mat> A, B;
    cv::split(input_a, A);
    cv::split(input_b, B);
    if ((int)A.size() < out_ch)
      A.resize(out_ch, cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1));
    if ((int)B.size() < out_ch)
      B.resize(out_ch, cv::Mat::zeros(input_b.rows, input_b.cols, CV_32FC1));
    std::vector<cv::Mat> O(out_ch);
    for (int c = 0; c < out_ch; ++c) {
      O[c] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
      if (c < in_ch)
        cv::addWeighted(A[c], alpha, B[c], beta, gamma, O[c]);
      else if (gamma != 0.0)
        O[c] = cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(gamma));
    }
    std::vector<char> covered(out_ch, 0);
    auto mark_covered = [&](const YAML::Node& n) {
      if (!n || !n.IsMap())
        return;
      for (auto it = n.begin(); it != n.end(); ++it) {
        YAML::Node dsts = it->second;
        if (!dsts || !dsts.IsSequence())
          continue;
        for (size_t i = 0; i < dsts.size(); ++i) {
          int d = -1;
          try {
            d = dsts[i].as<int>();
          } catch (...) {
            continue;
          }
          if (d >= 0 && d < out_ch)
            covered[d] = 1;
        }
      }
    };
    mark_covered(ch_input0);
    mark_covered(ch_input1);
    for (int d = 0; d < out_ch; ++d)
      if (covered[d])
        O[d] = (gamma != 0.0)
                   ? cv::Mat(input_a.rows, input_a.cols, CV_32FC1,
                             cv::Scalar(gamma))
                   : cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    auto apply_map = [&](const YAML::Node& n,
                         const std::vector<cv::Mat>& src_planes, double w) {
      if (!n || !n.IsMap())
        return;
      for (auto it = n.begin(); it != n.end(); ++it) {
        int src = -1;
        try {
          src = it->first.as<int>();
        } catch (...) {
          continue;
        }
        YAML::Node dsts = it->second;
        if (!dsts || !dsts.IsSequence())
          continue;
        for (size_t i = 0; i < dsts.size(); ++i) {
          int d = -1;
          try {
            d = dsts[i].as<int>();
          } catch (...) {
            continue;
          }
          if (src < 0 || src >= (int)src_planes.size())
            continue;
          if (d < 0 || d >= (int)O.size())
            continue;
          cv::add(O[d], src_planes[src] * w, O[d]);
        }
      }
    };
    apply_map(ch_input0, A, alpha);
    apply_map(ch_input1, B, beta);
    std::string alpha_strategy = as_str(P, "alpha_strategy", "weighted");
    if ((alpha_strategy != "weighted") && out_ch >= 4) {
      int aidx = 3;
      if (alpha_strategy == "max")
        O[aidx] = cv::max(A.size() > 3 ? A[3] : O[aidx],
                          B.size() > 3 ? B[3] : O[aidx]);
      else if (alpha_strategy == "copy0") {
        if ((int)A.size() > 3)
          O[aidx] = A[3].clone();
      } else if (alpha_strategy == "copy1") {
        if ((int)B.size() > 3)
          O[aidx] = B[3].clone();
      } else if (alpha_strategy == "set1")
        O[aidx] =
            cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(1.0));
      else if (alpha_strategy == "set0")
        O[aidx] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    }
    cv::merge(O, output);
  }

  NodeOutput out;
  out.image_buffer = fromCvMat(output);
  return out;
}

static NodeOutput op_abs_diff_monolithic(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
  if (inputs.size() < 2)
    throw GraphError(GraphErrc::MissingDependency,
                     "diff requires two input images.");
  cv::Mat input_a = toCvMat(inputs[0]->image_buffer);
  cv::Mat input_b = toCvMat(inputs[1]->image_buffer);
  if (input_a.empty() || input_b.empty())
    throw GraphError(GraphErrc::MissingDependency,
                     "diff inputs must be non-empty.");
  std::string strategy =
      as_str(node.runtime_parameters, "merge_strategy", "crop");
  normalize_to_base(input_b, input_a, strategy);

  std::string alpha_strategy =
      as_str(node.runtime_parameters, "alpha_strategy", "copy0");
  int ch = input_a.channels();
  cv::Mat output(input_a.rows, input_a.cols, CV_MAKETYPE(CV_32F, ch));
  if (ch < 2) {
    cv::absdiff(input_a, input_b, output);
  } else {
    std::vector<cv::Mat> Aa, Bb, Oo;
    cv::split(input_a, Aa);
    cv::split(input_b, Bb);
    Oo.resize(ch);
    int color_channels = (ch == 4) ? 3 : ch;
    for (int c = 0; c < color_channels; ++c)
      cv::absdiff(Aa[c], Bb[c], Oo[c]);
    if (ch == 4) {
      if (alpha_strategy == "copy0")
        Oo[3] = Aa[3].clone();
      else if (alpha_strategy == "copy1")
        Oo[3] = Bb[3].clone();
      else if (alpha_strategy == "max")
        Oo[3] = cv::max(Aa[3], Bb[3]);
      else if (alpha_strategy == "min")
        Oo[3] = cv::min(Aa[3], Bb[3]);
      else if (alpha_strategy == "diff")
        cv::absdiff(Aa[3], Bb[3], Oo[3]);
      else if (alpha_strategy == "set1")
        Oo[3] = cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(1.0));
      else if (alpha_strategy == "set0")
        Oo[3] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
      else
        Oo[3] = Aa[3].clone();
    }
    cv::merge(Oo, output);
  }
  NodeOutput out;
  out.image_buffer = fromCvMat(output);
  return out;
}

static NodeOutput op_multiply_monolithic(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  std::lock_guard<std::mutex> lock(g_opencv_op_mutex);
  if (inputs.size() < 2)
    throw GraphError(GraphErrc::MissingDependency,
                     "multiply requires two input images.");
  cv::Mat input_a = toCvMat(inputs[0]->image_buffer);
  cv::Mat input_b = toCvMat(inputs[1]->image_buffer);
  if (input_a.empty() || input_b.empty())
    throw GraphError(GraphErrc::MissingDependency,
                     "multiply inputs must be non-empty.");
  std::string strategy =
      as_str(node.runtime_parameters, "merge_strategy", "resize");
  normalize_to_base(input_b, input_a, strategy);
  const auto& P = node.runtime_parameters;
  double scale = as_double_flexible(P, "scale", 1.0);
  cv::Mat output;
  cv::multiply(input_a, input_b, output, scale);
  NodeOutput out;
  out.image_buffer = fromCvMat(output);
  return out;
}

// --- 注册所有操作 ---
void register_builtin() {
  static std::once_flag init_once;
  std::call_once(init_once, [] {
    cv::ocl::setUseOpenCL(false);
    // Avoid OpenCV spinning its own threads conflicting with our scheduler on
    // first use
    cv::setNumThreads(1);
  });
  auto& R = OpRegistry::instance();

  // Register with new OpImplementations API (Phase 1)
  // Monolithic HP implementations
  R.register_op_hp_monolithic("image_source", "path",
                              MonolithicOpFunc(op_image_source_path));
  R.register_op_hp_monolithic("image_generator", "constant",
                              MonolithicOpFunc(op_constant_image));
  R.register_op_hp_monolithic("image_generator", "perlin_noise",
                              MonolithicOpFunc(op_perlin_noise));
  R.register_op_hp_monolithic("analyzer", "get_dimensions",
                              MonolithicOpFunc(op_get_dimensions));
  R.register_op_hp_monolithic("math", "divide", MonolithicOpFunc(op_divide));
  R.register_op_hp_monolithic("image_process", "convolve",
                              MonolithicOpFunc(op_convolve));
  R.register_op_hp_monolithic("image_process", "resize",
                              MonolithicOpFunc(op_resize));
  R.register_op_hp_monolithic("image_process", "crop",
                              MonolithicOpFunc(op_crop));
  R.register_op_hp_monolithic("image_process", "extract_channel",
                              MonolithicOpFunc(op_extract_channel));
  R.register_op_hp_monolithic("image_mixing", "add_weighted",
                              MonolithicOpFunc(op_add_weighted_monolithic));
  R.register_op_hp_monolithic("image_mixing", "diff",
                              MonolithicOpFunc(op_abs_diff_monolithic));
  R.register_op_hp_monolithic("image_mixing", "multiply",
                              MonolithicOpFunc(op_multiply_monolithic));

  // Tiled HP implementations currently prefer HP-domain Macro granularity.
  OpMetadata tiled_meta;
  tiled_meta.tile_preference = TileSizePreference::MACRO;
  // RT implementations currently prefer RT-domain Micro granularity to match
  // the proxy pipeline; RT-domain Macro remains a valid scheduling category.
  OpMetadata rt_meta;
  rt_meta.tile_preference = TileSizePreference::MICRO;

  DirtyRoiPropFunc identity_roi = [](const Node&, const cv::Rect& roi,
                                     const GraphModel&) { return roi; };
  ForwardRoiPropFunc identity_forward =
      ForwardRoiPropFunc(identity_forward_roi);

  R.register_dirty_propagator("image_source", "path", identity_roi);
  R.register_dirty_propagator("image_generator", "constant", identity_roi);
  R.register_dirty_propagator("image_generator", "perlin_noise", identity_roi);
  R.register_dirty_propagator("analyzer", "get_dimensions", identity_roi);
  R.register_dirty_propagator("math", "divide", identity_roi);
  R.register_forward_propagator("image_source", "path", identity_forward);
  R.register_forward_propagator("image_generator", "constant",
                                identity_forward);
  R.register_forward_propagator("image_generator", "perlin_noise",
                                identity_forward);
  R.register_forward_propagator("analyzer", "get_dimensions", identity_forward);
  R.register_forward_propagator("math", "divide", identity_forward);
  R.register_dirty_propagator("image_process", "resize",
                              DirtyRoiPropFunc(resize_dirty_roi));
  R.register_dirty_propagator("image_process", "crop",
                              DirtyRoiPropFunc(crop_dirty_roi));
  R.register_dirty_propagator("image_process", "extract_channel", identity_roi);
  R.register_dirty_propagator("image_process", "curve_transform", identity_roi);
  R.register_dirty_propagator("image_mixing", "add_weighted", identity_roi);
  R.register_dirty_propagator("image_mixing", "diff", identity_roi);
  R.register_dirty_propagator("image_mixing", "multiply", identity_roi);
  R.register_forward_propagator("image_process", "resize",
                                ForwardRoiPropFunc(resize_forward_roi));
  R.register_forward_propagator("image_process", "crop",
                                ForwardRoiPropFunc(crop_forward_roi));
  R.register_forward_propagator("image_process", "extract_channel",
                                identity_forward);
  R.register_forward_propagator("image_process", "curve_transform",
                                identity_forward);
  R.register_forward_propagator("image_mixing", "add_weighted",
                                identity_forward);
  R.register_forward_propagator("image_mixing", "diff", identity_forward);
  R.register_forward_propagator("image_mixing", "multiply", identity_forward);

  R.register_dirty_propagator("image_process", "convolve",
                              DirtyRoiPropFunc(convolve_dirty_roi));
  R.register_forward_propagator("image_process", "convolve",
                                ForwardRoiPropFunc(convolve_forward_roi));

  // Gaussian blur: register both monolithic HP and tiled HP under the same key
  R.register_op_hp_monolithic("image_process", "gaussian_blur",
                              MonolithicOpFunc(op_gaussian_blur_monolithic));
  R.register_op_hp_tiled("image_process", "gaussian_blur",
                         TileOpFunc(op_gaussian_blur_tiled), tiled_meta);
  // Optional alias for backward compatibility if any graph uses
  // "gaussian_blur_tiled"
  R.register_op_hp_tiled("image_process", "gaussian_blur_tiled",
                         TileOpFunc(op_gaussian_blur_tiled), tiled_meta);
  // RT path currently reuses HP kernels until lighter variants land
  R.register_op_rt_tiled("image_process", "gaussian_blur",
                         TileOpFunc(op_gaussian_blur_tiled), rt_meta);
  R.register_op_rt_tiled("image_process", "gaussian_blur_tiled",
                         TileOpFunc(op_gaussian_blur_tiled), rt_meta);
  R.register_dirty_propagator("image_process", "gaussian_blur",
                              DirtyRoiPropFunc(gaussian_blur_dirty_roi));
  R.register_dirty_propagator("image_process", "gaussian_blur_tiled",
                              DirtyRoiPropFunc(gaussian_blur_dirty_roi));
  R.register_forward_propagator("image_process", "gaussian_blur",
                                ForwardRoiPropFunc(gaussian_blur_forward_roi));
  R.register_forward_propagator("image_process", "gaussian_blur_tiled",
                                ForwardRoiPropFunc(gaussian_blur_forward_roi));

  R.register_op_hp_tiled("image_process", "curve_transform",
                         TileOpFunc(op_curve_transform_tiled), tiled_meta);
  // Image mixing: provide both monolithic and tiled implementations; global HP
  // prefers monolithic
  R.register_op_hp_monolithic("image_mixing", "add_weighted",
                              MonolithicOpFunc(op_add_weighted_monolithic));
  R.register_op_hp_tiled("image_mixing", "add_weighted",
                         TileOpFunc(op_add_weighted_tiled), tiled_meta);
  R.register_op_rt_tiled("image_mixing", "add_weighted",
                         TileOpFunc(op_add_weighted_tiled), rt_meta);
  R.register_op_hp_monolithic("image_mixing", "diff",
                              MonolithicOpFunc(op_abs_diff_monolithic));
  R.register_op_hp_tiled("image_mixing", "diff", TileOpFunc(op_abs_diff_tiled),
                         tiled_meta);
  R.register_op_hp_monolithic("image_mixing", "multiply",
                              MonolithicOpFunc(op_multiply_monolithic));
  R.register_op_hp_tiled("image_mixing", "multiply",
                         TileOpFunc(op_multiply_tiled), tiled_meta);
}

}  // namespace ops
}  // namespace ps
