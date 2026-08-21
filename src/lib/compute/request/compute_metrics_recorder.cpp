#include "compute/request/compute_metrics_recorder.hpp"

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

#include "photospider/data/image_view.hpp"
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
 * @brief Completes an empty absolute ROI from canonical Value image bounds.
 * @param ctx Spatial metadata to update.
 * @param output Output whose named image Value supplies the fallback extent.
 * @return Nothing.
 * @throws std::invalid_argument when a signed image endpoint or span cannot
 * enter the current PixelRect representation.
 * @throws std::overflow_error when immutable data-window arithmetic overflows.
 * @note Existing positive ROI dimensions are preserved unchanged. Fallback
 *       validation completes before `ctx` receives the synthesized ROI.
 */
void ensure_absolute_roi(SpatialContext& ctx, const NodeOutput& output) {
  if (ctx.absolute_roi.width > 0 && ctx.absolute_roi.height > 0) {
    return;
  }
  if (!output.has_image_value() ||
      !output.image_value().image_facet().has_value()) {
    return;
  }
  const ImageBounds& bounds = output.image_value().image_bounds();
  const std::size_t width = image_bounds_width(bounds);
  const std::size_t height = image_bounds_height(bounds);
  const auto int_min =
      static_cast<std::int64_t>(std::numeric_limits<int>::min());
  const auto int_max =
      static_cast<std::int64_t>(std::numeric_limits<int>::max());
  if (bounds.x_begin < int_min || bounds.x_begin > int_max ||
      bounds.y_begin < int_min || bounds.y_begin > int_max ||
      bounds.x_end < int_min || bounds.x_end > int_max ||
      bounds.y_end < int_min || bounds.y_end > int_max ||
      width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument(
        "Canonical image bounds exceed SpatialContext PixelRect geometry.");
  }
  ctx.absolute_roi = PixelRect{
      static_cast<int>(bounds.x_begin), static_cast<int>(bounds.y_begin),
      static_cast<int>(width), static_cast<int>(height)};
}

/**
 * @brief Inherits spatial metadata from the first connected image slot.
 * @param output Computed output whose default spatial state may be replaced.
 * @param inputs Destination-indexed inputs, including disconnected null slots.
 * @return Nothing.
 * @throws std::invalid_argument or std::overflow_error when canonical image
 * bounds cannot enter SpatialContext.
 * @note Null placeholders are skipped without compressing callback-visible
 *       input ordering. The output absolute ROI is always completed
 *       afterwards. Inheritance and fallback validation finish in a local
 *       value, so either exception leaves the original output space unchanged.
 */
void inherit_spatial_context(NodeOutput& output,
                             const std::vector<const NodeOutput*>& inputs) {
  SpatialContext resolved_space = output.space;
  if (is_default_space(resolved_space)) {
    const auto first_connected =
        std::find_if(inputs.begin(), inputs.end(),
                     [](const NodeOutput* input) { return input != nullptr; });
    if (first_connected != inputs.end()) {
      resolved_space = (*first_connected)->space;
      resolved_space.local_inverse_matrix = {1.0, 0.0, 0.0, 0.0, 1.0,
                                             0.0, 0.0, 0.0, 1.0};
    }
  }
  ensure_absolute_roi(resolved_space, output);
  output.space = resolved_space;
}

/**
 * @brief Converts every public device label to stable diagnostic text.
 * @param backend Backend family retained by a canonical Value binding.
 * @return Owned uppercase label, or `UNKNOWN` for an invalid value.
 * @throws std::bad_alloc if result string storage cannot allocate.
 * @note This function does not imply that pixel storage is host-addressable.
 */
std::string device_to_string(DeviceBackend backend) {
  switch (backend) {
    case DeviceBackend::CPU:
      return "CPU";
    case DeviceBackend::Metal:
      return "GPU_METAL";
    case DeviceBackend::CUDA:
      return "GPU_CUDA";
    case DeviceBackend::Vulkan:
      return "GPU_VULKAN";
    case DeviceBackend::NPU:
      return "ASIC_NPU";
  }
  return "UNKNOWN";
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
 * @tparam Scalar Declared whole-byte Value scalar storage type.
 * @param bytes Address of sizeof(Scalar) readable bytes.
 * @return Scalar copied from the row.
 * @throws Nothing.
 * @note memcpy keeps inspection valid for arbitrary validated Value strides
 * and base alignment.
 */
template <typename Scalar>
Scalar read_scalar(const std::byte* bytes) noexcept {
  Scalar value{};
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

/**
 * @brief Inspects all logical image samples of one declared scalar type.
 * @tparam Scalar C++ type corresponding to Value element facts.
 * @param view Valid retaining host-readable ImageView.
 * @param statistics Mutable range/non-finite accumulator.
 * @return Nothing.
 * @throws std::out_of_range only if internal validated coordinate iteration
 * diverges from the immutable descriptor.
 * @throws std::bad_alloc when ImageView::channel_data cannot allocate its
 * per-sample full-rank logical-coordinate vector.
 * @note ImageView resolves arbitrary signed strides and skips every padding or
 * non-logical byte. No compatibility projection is allocated.
 */
template <typename Scalar>
void inspect_typed_pixels(const ImageView& view, PixelStatistics* statistics) {
  for (std::size_t y = 0U; y < view.height(); ++y) {
    for (std::size_t x = 0U; x < view.width(); ++x) {
      for (std::size_t channel = 0U; channel < view.channels(); ++channel) {
        const Scalar value =
            read_scalar<Scalar>(view.channel_data(x, y, channel));
        observe_pixel_value(static_cast<double>(value),
                            std::is_floating_point_v<Scalar>, statistics);
      }
    }
  }
}

/**
 * @brief Dispatches CPU pixel inspection by immutable Value element facts.
 * @param value Valid Ready host-readable image Value.
 * @return Accumulated active-pixel statistics.
 * @throws std::invalid_argument for an unsupported semantics/width pair.
 * @throws ReadyFenceAccessError, BufferAccessError, or std::out_of_range from
 * checked view access.
 * @throws std::bad_alloc when ImageView cannot copy complete ImageFacet
 * metadata or ImageView::channel_data cannot allocate a per-sample full-rank
 * logical-coordinate vector.
 * @note No provider conversion, sample-domain normalization, or compatibility
 * snapshot is performed. Native UINT32 samples promote exactly to the binary64
 * diagnostic range.
 */
PixelStatistics inspect_cpu_pixels(const Value& value) {
  ImageView view(value);
  const DenseTensorDescriptor& descriptor = view.descriptor();
  const std::uint32_t bits = descriptor.storage_encoding.bit_width;
  PixelStatistics statistics;
  if (descriptor.element_semantics == ElementSemantics::UnsignedInteger &&
      bits == 8U) {
    inspect_typed_pixels<std::uint8_t>(view, &statistics);
  } else if (descriptor.element_semantics == ElementSemantics::SignedInteger &&
             bits == 8U) {
    inspect_typed_pixels<std::int8_t>(view, &statistics);
  } else if (descriptor.element_semantics ==
                 ElementSemantics::UnsignedInteger &&
             bits == 16U) {
    inspect_typed_pixels<std::uint16_t>(view, &statistics);
  } else if (descriptor.element_semantics == ElementSemantics::SignedInteger &&
             bits == 16U) {
    inspect_typed_pixels<std::int16_t>(view, &statistics);
  } else if (descriptor.element_semantics ==
                 ElementSemantics::UnsignedInteger &&
             bits == 32U) {
    inspect_typed_pixels<std::uint32_t>(view, &statistics);
  } else if (descriptor.element_semantics == ElementSemantics::FloatingPoint &&
             bits == 32U) {
    inspect_typed_pixels<float>(view, &statistics);
  } else if (descriptor.element_semantics == ElementSemantics::FloatingPoint &&
             bits == 64U) {
    inspect_typed_pixels<double>(view, &statistics);
  } else {
    throw std::invalid_argument(
        "Debug statistics do not support this Value element type.");
  }
  return statistics;
}

/**
 * @brief Populates pixel statistics when generic CPU access is valid.
 * @param output Output whose debug fields may receive min/max/non-finite data.
 * @return Nothing.
 * @throws std::invalid_argument for unsupported image element facts.
 * @throws ReadyFenceAccessError, BufferAccessError, or std::out_of_range from
 * checked Value access.
 * @throws std::bad_alloc when ImageView cannot copy complete ImageFacet
 * metadata or ImageView::channel_data cannot allocate a per-sample full-rank
 * logical-coordinate vector.
 * @note Absent, pending, failed, cancelled, or non-host-visible Values retain
 * operation-provided statistics. Ready host-visible samples are scanned
 * through ImageView and padding is never interpreted. An all-NaN active
 * payload retains the legacy positive/negative infinity empty-range sentinels.
 */
void populate_debug_statistics(NodeOutput& output) {
  if (!output.has_image_value()) {
    return;
  }
  const Value& value = output.image_value();
  if (!value.ready_fence().poll().ready() ||
      !value.buffer_handle().host_visible()) {
    return;
  }
  const PixelStatistics statistics = inspect_cpu_pixels(value);
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
 * @throws std::invalid_argument if enabled statistics receive unsupported
 * immutable image element facts or signed image endpoints/spans cannot enter
 * the SpatialContext representation.
 * @throws std::overflow_error when immutable image-window arithmetic is
 * unrepresentable.
 * @throws ReadyFenceAccessError, BufferAccessError, or std::out_of_range when
 * a Ready host-visible Value cannot provide a checked logical sample.
 * @throws std::bad_alloc if ImageView cannot copy complete ImageFacet metadata
 * or ImageView::channel_data cannot allocate a per-sample full-rank
 * logical-coordinate vector, or if diagnostic device-label storage cannot
 * allocate.
 * @note Spatial inheritance occurs before timestamp, worker, duration, and
 *       device publication. A spatial fallback conversion failure leaves the
 *       original space, debug metadata, and named Values unchanged. Debug
 *       identity fields are always updated after successful spatial
 *       completion; only Ready host-visible pixel statistics depend on
 *       `enable_timing`. Pending Values expose device metadata through
 *       representation-neutral indexed StorageBinding inspection without
 *       payload access and retain callback-provided statistics until a later
 *       Ready observation. Native UINT32 samples are promoted exactly to
 *       binary64 min/max diagnostics without sample-domain normalization.
 *       ImageView walks logical samples and ignores padding; opaque backend
 *       statistics remain untouched without a device adapter.
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
  DeviceBackend backend = DeviceBackend::CPU;
  if (output.has_image_value()) {
    backend = output.image_value().storage_binding().device.backend();
  } else if (!output.named_values.empty()) {
    backend = output.named_values.begin()
                  ->second.storage_binding(0U)
                  .device.backend();
  }
  output.debug.compute_device = device_to_string(backend);
  if (enable_timing)
    populate_debug_statistics(output);
}

}  // namespace ps::compute
