#include "kernel/services/compute-service/compute_metrics_recorder.hpp"

#include <chrono>
#include <cmath>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/graph_runtime.hpp"

namespace ps::compute {
namespace {

bool approx_equal(double a, double b, double eps = 1e-9) {
  return std::abs(a - b) <= eps;
}

bool is_identity_matrix(const std::array<double, 9>& mat) {
  static constexpr std::array<double, 9> kIdentity{1.0, 0.0, 0.0, 0.0, 1.0,
                                                   0.0, 0.0, 0.0, 1.0};
  for (size_t i = 0; i < mat.size(); ++i) {
    if (!approx_equal(mat[i], kIdentity[i]))
      return false;
  }
  return true;
}

bool is_default_space(const SpatialContext& ctx) {
  return (ctx.absolute_roi.width <= 0 || ctx.absolute_roi.height <= 0) &&
         approx_equal(ctx.global_scale_x, 1.0) &&
         approx_equal(ctx.global_scale_y, 1.0) &&
         is_identity_matrix(ctx.transform_matrix) &&
         is_identity_matrix(ctx.inverse_matrix);
}

void ensure_absolute_roi(SpatialContext& ctx, const ImageBuffer& buffer) {
  if ((ctx.absolute_roi.width <= 0 || ctx.absolute_roi.height <= 0) &&
      buffer.width > 0 && buffer.height > 0) {
    ctx.absolute_roi = cv::Rect(0, 0, buffer.width, buffer.height);
  }
}

void inherit_spatial_context(NodeOutput& output,
                             const std::vector<const NodeOutput*>& inputs) {
  if (!inputs.empty() && is_default_space(output.space)) {
    output.space = inputs.front()->space;
    output.space.local_inverse_matrix = {1.0, 0.0, 0.0, 0.0, 1.0,
                                         0.0, 0.0, 0.0, 1.0};
  }
  ensure_absolute_roi(output.space, output.image_buffer);
}

std::string device_to_string(Device device) {
  switch (device) {
    case Device::CPU:
      return "CPU";
    case Device::GPU_METAL:
      return "GPU_METAL";
    default:
      return "UNKNOWN";
  }
}

void populate_debug_statistics(NodeOutput& output) {
  if (!output.image_buffer.data && !output.image_buffer.context)
    return;
  if (output.image_buffer.width <= 0 || output.image_buffer.height <= 0)
    return;
  cv::Mat mat = toCvMat(output.image_buffer);
  if (mat.empty())
    return;
  if (!mat.isContinuous())
    mat = mat.clone();
  if (mat.channels() > 1)
    mat = mat.reshape(1);
  double min_val = 0.0;
  double max_val = 0.0;
  cv::minMaxIdx(mat, &min_val, &max_val);
  output.debug.min_val = min_val;
  output.debug.max_val = max_val;
  output.debug.has_nan = !cv::checkRange(mat, true, nullptr);
}

}  // namespace

void ComputeMetricsRecorder::finalize_output_metadata(
    NodeOutput& output, const std::vector<const NodeOutput*>& inputs,
    bool enable_timing, double execution_ms) {
  inherit_spatial_context(output, inputs);
  const auto now = std::chrono::high_resolution_clock::now();
  output.debug.timestamp_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          now.time_since_epoch())
          .count());
  output.debug.computed_by_worker_id = GraphRuntime::this_worker_id();
  if (execution_ms < 0.0)
    execution_ms = 0.0;
  output.debug.execution_time_ms =
      static_cast<uint64_t>(std::llround(execution_ms));
  output.debug.compute_device = device_to_string(output.image_buffer.device);
  if (enable_timing)
    populate_debug_statistics(output);
}

}  // namespace ps::compute
