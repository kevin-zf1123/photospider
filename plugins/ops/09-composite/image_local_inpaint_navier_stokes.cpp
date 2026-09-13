#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <utility>

#include "00-foundation/image_common.hpp"
#include "plugin/builtin_operations.hpp"
#ifdef PHOTOSPIDER_INPAINT_OPENCV
#include <opencv2/photo.hpp>
#endif

namespace ps::plugin_internal {
namespace {
using namespace image_ops;  // NOLINT(build/namespaces)

Result<Value> execute(const OperationInvocation& call) {
  const auto radius = std::get<std::int64_t>(call.parameters.at("radius"));
  const auto& image = call.inputs[0];
  const auto& mask = call.inputs[1];
  const auto h = image.descriptor().shape[0];
  const auto w = image.descriptor().shape[1];
  if (h < 3 || w < 3)
    return Result<Value>(Status::failure(ErrorCode::OperationFailed, "image must be at least 3x3"));
  std::vector<float> pixels(h * w * 4);
  std::vector<std::uint8_t> holes(h * w);
  std::size_t count = 0;
  for (std::uint64_t y = 0; y < h; ++y) {
    if (call.cancellation.cancelled())
      return Result<Value>(Status::failure(ErrorCode::Cancelled, "inpaint cancelled"));
    for (std::uint64_t x = 0; x < w; ++x) {
      const auto m = sample(mask, y, x);
      if (!std::isfinite(m) || (m != 0.0F && m != 1.0F))
        return Result<Value>(Status::failure(ErrorCode::OperationFailed, "hole_mask must be binary and finite"));
      holes[y * w + x] = m == 1.0F ? 1 : 0;
      count += holes[y * w + x];
      for (std::uint64_t c = 0; c < 4; ++c) {
        const float v = sample(image, y, x, c);
        if (!std::isfinite(v) || (c == 3 && v != 1.0F))
          return Result<Value>(Status::failure(
              ErrorCode::OperationFailed,
              "image samples must be finite with opaque alpha"));
        pixels[(y * w + x) * 4 + c] = v;
      }
    }
  }
  if (count == h * w)
    return Result<Value>(Status::failure(ErrorCode::OperationFailed, "full mask is not fillable"));
  if (count != 0) {
    for (std::uint64_t pass = 0; pass < h * w; ++pass) {
      bool changed = false;
      for (std::uint64_t y = 0; y < h; ++y) for (std::uint64_t x = 0; x < w; ++x) {
        const auto index = y * w + x;
        if (!holes[index]) continue;
        float sum[3] = {0, 0, 0}; int n = 0;
        for (int dy = -static_cast<int>(radius); dy <= static_cast<int>(radius); ++dy)
          for (int dx = -static_cast<int>(radius); dx <= static_cast<int>(radius); ++dx) {
            if (dx * dx + dy * dy > radius * radius) continue;
            const auto yy = static_cast<int64_t>(y) + dy, xx = static_cast<int64_t>(x) + dx;
            if (yy < 0 || xx < 0 || yy >= static_cast<int64_t>(h) || xx >= static_cast<int64_t>(w)) continue;
            const auto ni = static_cast<std::size_t>(yy * w + xx);
            if (!holes[ni]) {
              for (int c = 0; c < 3; ++c) sum[c] += pixels[ni * 4 + c];
              ++n;
            }
          }
        if (n) {
          for (int c = 0; c < 3; ++c) pixels[index * 4 + c] = sum[c] / n;
          holes[index] = 0;
          changed = true;
        }
      }
      if (!changed) break;
    }
  }
  auto made = MutableValue::allocate(image.descriptor(), call.output_region, call.allocator);
  if (!made.ok()) return Result<Value>(made.status());
  auto output = made.take_value();
  std::size_t at = 0;
  for (std::uint64_t y = 0; y < h; ++y) for (std::uint64_t x = 0; x < w; ++x) for (int c = 0; c < 4; ++c) {
    std::memcpy(output.data() + at, &pixels[(y * w + x) * 4 + c], sizeof(float)); at += sizeof(float);
  }
  return std::move(output).publish(image.facets());
}
#ifdef PHOTOSPIDER_INPAINT_OPENCV
Result<Value> execute_opencv(const OperationInvocation& call) {
  const int radius = static_cast<int>(std::get<std::int64_t>(call.parameters.at("radius")));
  const auto& image = call.inputs[0]; const auto& mask = call.inputs[1];
  const int h = static_cast<int>(image.descriptor().shape[0]);
  const int w = static_cast<int>(image.descriptor().shape[1]);
  cv::Mat hole(h, w, CV_8UC1); std::vector<float> packed(static_cast<size_t>(h) * w * 4);
  if (h < 3 || w < 3) return Result<Value>(Status::failure(ErrorCode::OperationFailed, "image must be at least 3x3"));
  size_t count = 0;
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    const float m = sample(mask, y, x); if (!std::isfinite(m) || (m != 0 && m != 1)) return Result<Value>(Status::failure(ErrorCode::OperationFailed, "invalid mask"));
    hole.at<unsigned char>(y, x) = m == 1 ? 255 : 0; count += m == 1;
    for (int c = 0; c < 4; ++c) { packed[(static_cast<size_t>(y) * w + x) * 4 + c] = sample(image, y, x, c); if (!std::isfinite(packed[(static_cast<size_t>(y) * w + x) * 4 + c]) || (c == 3 && packed[(static_cast<size_t>(y) * w + x) * 4 + c] != 1.0F)) return Result<Value>(Status::failure(ErrorCode::OperationFailed, "invalid image")); }
  }
  if (count == static_cast<size_t>(h) * w) return Result<Value>(Status::failure(ErrorCode::OperationFailed, "full mask"));
  for (int c = 0; c < 3; ++c) {
    std::vector<float> plane_data(static_cast<size_t>(h) * w);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) plane_data[static_cast<size_t>(y) * w + x] = packed[(static_cast<size_t>(y) * w + x) * 4 + c];
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (hole.at<unsigned char>(y, x)) plane_data[static_cast<size_t>(y) * w + x] = 0.0F;
    cv::Mat plane(h, w, CV_32FC1, plane_data.data());
    cv::Mat out; cv::inpaint(plane, hole, out, radius, cv::INPAINT_NS);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) if (hole.at<unsigned char>(y, x)) packed[(static_cast<size_t>(y) * w + x) * 4 + c] = out.at<float>(y, x);
    if (call.cancellation.cancelled()) return Result<Value>(Status::failure(ErrorCode::Cancelled, "inpaint cancelled"));
  }
  auto made = MutableValue::allocate(image.descriptor(), call.output_region, call.allocator); if (!made.ok()) return Result<Value>(made.status());
  auto output = made.take_value(); std::memcpy(output.data(), packed.data(), packed.size() * sizeof(float));
  return std::move(output).publish(image.facets());
}
#endif
}  // namespace

Status register_local_inpaint_navier_stokes(OperationRegistry* registry) {
  for (const char* key : {"image.local_inpaint_navier_stokes_openCV",
                          "image.local_inpaint_navier_stokes_native_apple_silicon"}) {
    OperationDefinition op;
    op.key = key;
    op.traits.input_count = 2;
    op.traits.input_schema = {{OperationPortKind::RgbaFloat32, 0, 0}, {OperationPortKind::Float32Mask, 0, 0}};
    op.traits.parameter_schema = {OperationParameterSpec{"radius", OperationParameterType::Int64, true, true, 1, 32}};
    op.traits.outputs[0].key = "image";
    op.traits.outputs[0].output_element_type = ElementType::Float32;
    op.traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
    op.traits.outputs[0].region_rule = OperationRegionRule::Whole;
    op.traits.outputs[0].output_semantic_rule = OperationSemanticRule::PreserveInput;
    op.traits.outputs[0].output_schema = op.traits.input_schema[0];
    op.callback = (std::string(key).find("openCV") != std::string::npos) ?
#ifdef PHOTOSPIDER_INPAINT_OPENCV
        execute_opencv : execute;
#else
        execute;
#endif
    auto status = registry->register_operation(std::move(op));
    if (!status.ok()) return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
