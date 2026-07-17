#include "compute/compute_metrics_recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "runtime/graph_runtime.hpp"

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
    ctx.absolute_roi = PixelRect{0, 0, buffer.width, buffer.height};
  }
}

/**
 * @brief Inherits spatial metadata from the first connected image slot.
 * @param output Computed output whose default spatial state may be replaced.
 * @param inputs Destination-indexed inputs, including disconnected null slots.
 * @return Nothing.
 * @throws Nothing under current fixed-size spatial value assignment.
 * @note Null placeholders are skipped without compressing callback-visible
 * input ordering. The output absolute ROI is always completed afterwards.
 */
void inherit_spatial_context(NodeOutput& output,
                             const std::vector<const NodeOutput*>& inputs) {
  if (is_default_space(output.space)) {
    const auto first_connected =
        std::find_if(inputs.begin(), inputs.end(),
                     [](const NodeOutput* input) { return input != nullptr; });
    if (first_connected != inputs.end()) {
      output.space = (*first_connected)->space;
      output.space.local_inverse_matrix = {1.0, 0.0, 0.0, 0.0, 1.0,
                                           0.0, 0.0, 0.0, 1.0};
    }
  }
  ensure_absolute_roi(output.space, output.image_buffer);
}

/**
 * @brief Converts every public device label to stable diagnostic text.
 * @param device Device enumerator returned by an operation.
 * @return Owned uppercase label, or `UNKNOWN` for an invalid value.
 * @throws std::bad_alloc if result string storage cannot allocate.
 * @note This function does not imply that pixel storage is host-addressable.
 */
std::string device_to_string(Device device) {
  switch (device) {
    case Device::CPU:
      return "CPU";
    case Device::GPU_METAL:
      return "GPU_METAL";
    case Device::GPU_CUDA:
      return "GPU_CUDA";
    case Device::ASIC_NPU:
      return "ASIC_NPU";
    default:
      return "UNKNOWN";
  }
}

/**
 * @brief Populates pixel statistics when generic CPU access is valid.
 * @param output Output whose debug fields may receive min/max/NaN values.
 * @return Nothing.
 * @throws cv::Exception from CPU matrix inspection.
 * @note Non-CPU and context-only images retain their operation-provided
 * statistics because only a device adapter may map those resources safely.
 */
void populate_debug_statistics(NodeOutput& output) {
  if (output.image_buffer.device != Device::CPU || !output.image_buffer.data)
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

/**
 * @brief Completes one output's spatial inheritance and execution diagnostics.
 *
 * @param output Mutable completed operation output.
 * @param inputs Destination-indexed upstream outputs; null disconnected slots
 *        are skipped while choosing the first live spatial source.
 * @param enable_timing Whether pixel min/max/NaN inspection is enabled.
 * @param execution_ms Measured duration, clamped to zero and rounded to whole
 *        milliseconds.
 * @return Nothing.
 * @throws cv::Exception if enabled image conversion or statistics fail.
 * @throws std::bad_alloc if the device label cannot be stored.
 * @note Spatial inheritance occurs before timestamp, worker, duration, and
 *       device publication. Debug identity fields are always updated; only
 *       CPU-addressable pixel statistics depend on `enable_timing`. Opaque
 *       backend statistics remain untouched without a device adapter.
 */
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
