#include "compute/compute_metrics_recorder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "photospider/core/image_buffer.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Compares two spatial metadata scalars within a fixed tolerance.
 * @param a First value.
 * @param b Second value.
 * @param eps Maximum accepted absolute difference.
 * @return True when the values differ by no more than eps.
 * @throws Nothing.
 * @note The helper is used only for identity/default metadata recognition; it
 * does not define a public numeric-equality contract.
 */
bool approx_equal(double a, double b, double eps = 1e-9) {
  return std::abs(a - b) <= eps;
}

/**
 * @brief Reports whether a 3x3 spatial transform is approximately identity.
 * @param mat Matrix values in row-major order.
 * @return True when every element matches identity within approx_equal.
 * @throws Nothing.
 * @note Non-finite values compare unequal and therefore do not look default.
 */
bool is_identity_matrix(const std::array<double, 9>& mat) {
  static constexpr std::array<double, 9> kIdentity{1.0, 0.0, 0.0, 0.0, 1.0,
                                                   0.0, 0.0, 0.0, 1.0};
  for (size_t i = 0; i < mat.size(); ++i) {
    if (!approx_equal(mat[i], kIdentity[i]))
      return false;
  }
  return true;
}

/**
 * @brief Detects the unset SpatialContext state used for input inheritance.
 * @param ctx Spatial metadata to inspect.
 * @return True when ROI is empty, scales are one, and transforms are identity.
 * @throws Nothing.
 * @note A default context may be replaced by the first connected input.
 */
bool is_default_space(const SpatialContext& ctx) {
  return (ctx.absolute_roi.width <= 0 || ctx.absolute_roi.height <= 0) &&
         approx_equal(ctx.global_scale_x, 1.0) &&
         approx_equal(ctx.global_scale_y, 1.0) &&
         is_identity_matrix(ctx.transform_matrix) &&
         is_identity_matrix(ctx.inverse_matrix);
}

/**
 * @brief Completes an empty absolute ROI from output image dimensions.
 * @param ctx Spatial metadata to update.
 * @param buffer Output image descriptor supplying the fallback extent.
 * @return Nothing.
 * @throws Nothing.
 * @note Existing positive ROI dimensions are preserved unchanged.
 */
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
 * @brief Mutable accumulator for one CPU image's active scalar samples.
 * @throws Nothing for value operations.
 * @note `has_non_finite` preserves the legacy `has_nan` diagnostic behavior
 * that also flagged infinities through OpenCV checkRange. NaN samples do not
 * participate in min/max comparisons.
 */
struct PixelStatistics {
  /** @brief Whether at least one non-NaN value initialized the range. */
  bool has_comparable_value = false;

  /** @brief Whether a floating sample was NaN or infinite. */
  bool has_non_finite = false;

  /**
   * @brief Smallest comparable sample, or positive infinity when all active
   * samples are NaN.
   */
  double min_value = std::numeric_limits<double>::infinity();

  /**
   * @brief Largest comparable sample, or negative infinity when all active
   * samples are NaN.
   */
  double max_value = -std::numeric_limits<double>::infinity();
};

/**
 * @brief Adds one scalar to the range/non-finite accumulator.
 * @param value Scalar converted to double.
 * @param floating_point Whether the source scalar type can encode NaN/Inf.
 * @param statistics Mutable accumulator.
 * @return Nothing.
 * @throws Nothing.
 * @note NaN sets the diagnostic flag and is excluded from range comparisons;
 * infinities set the flag but retain their ordered min/max behavior.
 */
void observe_pixel_value(double value, bool floating_point,
                         PixelStatistics* statistics) noexcept {
  if (floating_point && !std::isfinite(value)) {
    statistics->has_non_finite = true;
    if (std::isnan(value)) {
      return;
    }
  }
  if (!statistics->has_comparable_value) {
    statistics->has_comparable_value = true;
    statistics->min_value = value;
    statistics->max_value = value;
    return;
  }
  statistics->min_value = std::min(statistics->min_value, value);
  statistics->max_value = std::max(statistics->max_value, value);
}

/**
 * @brief Reads one potentially unaligned scalar without aliasing assumptions.
 * @tparam Scalar Declared ImageBuffer scalar storage type.
 * @param bytes Address of sizeof(Scalar) readable bytes.
 * @return Scalar copied from the row.
 * @throws Nothing.
 * @note memcpy keeps row inspection valid for externally owned CPU snapshots
 * whose base alignment is weaker than kernel allocation alignment.
 */
template <typename Scalar>
Scalar read_scalar(const std::byte* bytes) noexcept {
  Scalar value{};
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

/**
 * @brief Inspects all active samples of one declared scalar type.
 * @tparam Scalar C++ type corresponding to ImageBuffer::type.
 * @param buffer Valid nonempty CPU image.
 * @param statistics Mutable range/non-finite accumulator.
 * @return Nothing.
 * @throws std::invalid_argument for malformed/non-CPU row access.
 * @throws std::out_of_range only if internal validated row iteration diverges
 * from the descriptor.
 * @note Every row begins at ImageBuffer::step; padding bytes are skipped.
 * Channel values are flattened only logically, without allocating or
 * reshaping an adapter matrix.
 */
template <typename Scalar>
void inspect_typed_pixels(const ImageBuffer& buffer,
                          PixelStatistics* statistics) {
  const std::size_t row_bytes = image_buffer_row_bytes(buffer);
  const std::size_t sample_count = row_bytes / sizeof(Scalar);
  for (int row_index = 0; row_index < buffer.height; ++row_index) {
    const std::byte* row = image_buffer_row_data(buffer, row_index);
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
      const Scalar value = read_scalar<Scalar>(row + sample * sizeof(Scalar));
      observe_pixel_value(static_cast<double>(value),
                          std::is_floating_point_v<Scalar>, statistics);
    }
  }
}

/**
 * @brief Dispatches CPU pixel inspection by the public DataType enum.
 * @param buffer Valid nonempty CPU descriptor.
 * @return Accumulated active-pixel statistics.
 * @throws std::invalid_argument for an invalid descriptor or DataType.
 * @throws std::out_of_range if validated row iteration cannot be represented.
 * @note The switch is exhaustive for the public scalar contract and performs
 * no provider conversion.
 */
PixelStatistics inspect_cpu_pixels(const ImageBuffer& buffer) {
  validate_image_buffer(buffer);
  PixelStatistics statistics;
  switch (buffer.type) {
    case DataType::UINT8:
      inspect_typed_pixels<std::uint8_t>(buffer, &statistics);
      break;
    case DataType::INT8:
      inspect_typed_pixels<std::int8_t>(buffer, &statistics);
      break;
    case DataType::UINT16:
      inspect_typed_pixels<std::uint16_t>(buffer, &statistics);
      break;
    case DataType::INT16:
      inspect_typed_pixels<std::int16_t>(buffer, &statistics);
      break;
    case DataType::FLOAT32:
      inspect_typed_pixels<float>(buffer, &statistics);
      break;
    case DataType::FLOAT64:
      inspect_typed_pixels<double>(buffer, &statistics);
      break;
  }
  return statistics;
}

/**
 * @brief Populates pixel statistics when generic CPU access is valid.
 * @param output Output whose debug fields may receive min/max/non-finite data.
 * @return Nothing.
 * @throws std::invalid_argument for malformed image descriptors.
 * @throws std::out_of_range if validated row iteration is inconsistent.
 * @note Valid non-CPU/context-only and canonical-empty images retain their
 * operation-provided statistics because only a device adapter may map opaque
 * resources safely. Active rows are scanned through kernel primitives; padding
 * is never interpreted as pixels. An all-NaN active payload retains the legacy
 * positive/negative infinity empty-range sentinels.
 */
void populate_debug_statistics(NodeOutput& output) {
  const ImageBuffer& buffer = output.image_buffer;
  validate_image_buffer(buffer);
  if (buffer.device != Device::CPU || !buffer.data || buffer.width <= 0 ||
      buffer.height <= 0) {
    return;
  }
  const PixelStatistics statistics = inspect_cpu_pixels(buffer);
  output.debug.min_val = statistics.min_value;
  output.debug.max_val = statistics.max_value;
  output.debug.has_nan = statistics.has_non_finite;
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
 * @throws std::invalid_argument if enabled statistics receive a malformed
 * image descriptor.
 * @throws std::out_of_range if validated row iteration is inconsistent.
 * @throws std::bad_alloc if the device label cannot be stored.
 * @note Spatial inheritance occurs before timestamp, worker, duration, and
 *       device publication. Debug identity fields are always updated; only
 *       CPU-addressable pixel statistics depend on `enable_timing`. Those
 *       statistics walk active bytes by ImageBuffer::step and ignore padding;
 *       opaque backend statistics remain untouched without a device adapter.
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
