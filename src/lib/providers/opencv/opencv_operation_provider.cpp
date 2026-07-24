// FILE: src/lib/providers/opencv/opencv_operation_provider.cpp
#include "providers/opencv/opencv_operation_provider.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#if defined(PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
#include <atomic>
#endif
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#if defined(PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
#include "providers/opencv/opencv_operation_provider_test_access.hpp"
#endif
#include "compute/compute_geometry.hpp"  // NOLINT(build/include_subdir)
#include "core/ops.hpp"
#include "core/param_utils.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)

namespace ps::providers::opencv {

// =============================================================================
// ==                         全局资源与辅助函数                           ==
// =============================================================================

#if defined(PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
namespace {

/** @brief Borrowed observer published only by serialized integration tests. */
std::atomic<OpenCvOperationObserver*> g_opencv_operation_observer{nullptr};

/**
 * @brief One-shot OpenCV status injected into process initialization.
 *
 * @note Zero means disabled. Test publication and provider initialization are
 *       serialized by the isolated exception-contract test process.
 */
std::atomic<int> g_opencv_process_initialization_failure{cv::Error::StsOk};

/**
 * @brief Brackets one built-in OpenCV callback body for deterministic tests.
 *
 * @throws Nothing.
 * @note The constructor snapshots the borrowed observer once so exit is sent
 *       to the same object even if publication is later cleared. Test lifetime
 *       rules forbid clearing while a callback remains active.
 */
class OpenCvOperationObservationScope final {
 public:
  /**
   * @brief Records observed callback entry.
   * @param operation_key Stable built-in `type:subtype` key.
   * @throws Nothing.
   */
  explicit OpenCvOperationObservationScope(const char* operation_key) noexcept
      : operation_key_(operation_key),
        observer_(g_opencv_operation_observer.load(std::memory_order_acquire)) {
    if (observer_ != nullptr) {
      observer_->on_enter(operation_key_);
    }
  }

  /**
   * @brief Records observed callback exit during return or unwinding.
   * @throws Nothing.
   */
  ~OpenCvOperationObservationScope() noexcept {
    if (observer_ != nullptr) {
      observer_->on_exit(operation_key_);
    }
  }

  /**
   * @brief Prevents duplicate callback-exit ownership.
   * @param other Scope retaining its exit notification.
   * @throws Nothing because copying is unavailable.
   */
  OpenCvOperationObservationScope(
      const OpenCvOperationObservationScope& other) = delete;

  /**
   * @brief Prevents replacing callback-exit ownership.
   * @param other Scope that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  OpenCvOperationObservationScope& operator=(
      const OpenCvOperationObservationScope& other) = delete;

 private:
  /** @brief Static-lifetime operation key sent on entry and exit. */
  const char* operation_key_;

  /** @brief Borrowed observer snapshot retained through callback exit. */
  OpenCvOperationObserver* observer_;
};

}  // namespace

/** @copydoc set_opencv_operation_observer_for_testing */
void set_opencv_operation_observer_for_testing(
    OpenCvOperationObserver* observer) noexcept {
  g_opencv_operation_observer.store(observer, std::memory_order_release);
}

/** @copydoc set_opencv_process_initialization_failure_for_testing */
void set_opencv_process_initialization_failure_for_testing(
    int error_code) noexcept {
  g_opencv_process_initialization_failure.store(error_code,
                                                std::memory_order_release);
}
#define PHOTOSPIDER_OBSERVE_OPENCV_OPERATION(operation_key) \
  OpenCvOperationObservationScope opencv_observation(operation_key)
#else
#define PHOTOSPIDER_OBSERVE_OPENCV_OPERATION(operation_key) ((void)0)
#endif

/**
 * @brief Converts one OpenCV exception before it can escape the provider.
 *
 * @param operation_key Stable operation key or initialization phase label.
 * @param error OpenCV exception caught while the provider frame is active.
 * @return Never returns.
 * @throws std::bad_alloc as a fresh host-owned exception when OpenCV reports
 *         `cv::Error::StsNoMem`.
 * @throws GraphError with `GraphErrc::ComputeError` for every other OpenCV
 *         failure.
 * @note The translated diagnostic owns its text and exposes no OpenCV dynamic
 *       exception type beyond this private provider boundary.
 */
[[noreturn]] static void throw_translated_opencv_exception(
    const char* operation_key, const cv::Exception& error) {
  if (error.code == cv::Error::StsNoMem) {
    throw std::bad_alloc();
  }
  throw GraphError(GraphErrc::ComputeError, std::string("OpenCV provider '") +
                                                operation_key +
                                                "' failed: " + error.what());
}

/**
 * @brief Wraps one full-output OpenCV callback with the provider error fence.
 *
 * @param operation_key Process-lifetime operation key used for diagnostics.
 * @param callback Provider callback to retain.
 * @return Registry callback that preserves non-OpenCV host exceptions and
 *         translates every `cv::Exception`.
 * @throws std::bad_alloc if callback storage allocation fails.
 * @note The wrapper owns no global execution mutex. Its callback target must
 *       remain reentrant for concurrent registry snapshots.
 */
static MonolithicOpFunc fence_monolithic_operation(const char* operation_key,
                                                   MonolithicOpFunc callback) {
  return [operation_key, callback = std::move(callback)](
             const Node& node,
             const std::vector<const NodeOutput*>& inputs) mutable {
    try {
      return callback(node, inputs);
    } catch (const cv::Exception& error) {
      throw_translated_opencv_exception(operation_key, error);
    }
  };
}

/**
 * @brief Wraps one tiled OpenCV callback with the provider error fence.
 *
 * @param operation_key Process-lifetime operation key used for diagnostics.
 * @param callback Provider callback to retain.
 * @return Registry callback that preserves non-OpenCV host exceptions and
 *         translates every `cv::Exception`.
 * @throws std::bad_alloc if callback storage allocation fails.
 * @note Tile invocations remain independently callable and are never
 *       serialized by this wrapper.
 */
static TileOpFunc fence_tiled_operation(const char* operation_key,
                                        TileOpFunc callback) {
  return [operation_key, callback = std::move(callback)](
             const Node& node, const OutputTile& output,
             const std::vector<InputTile>& inputs) mutable {
    try {
      callback(node, output, inputs);
    } catch (const cv::Exception& error) {
      throw_translated_opencv_exception(operation_key, error);
    }
  };
}

#if defined(PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
/** @copydoc invoke_monolithic_opencv_exception_fence_for_testing */
void invoke_monolithic_opencv_exception_fence_for_testing(int error_code) {
  MonolithicOpFunc callback = fence_monolithic_operation(
      "testing:monolithic_exception_fence",
      [error_code](const Node&,
                   const std::vector<const NodeOutput*>&) -> NodeOutput {
        throw cv::Exception(
            error_code, "deterministic injected provider exception",
            "invoke_monolithic_opencv_exception_fence_for_testing", __FILE__,
            __LINE__);
      });
  const Node node;
  (void)callback(node, {});
}

/** @copydoc invoke_tiled_opencv_exception_fence_for_testing */
void invoke_tiled_opencv_exception_fence_for_testing(int error_code) {
  TileOpFunc callback = fence_tiled_operation(
      "testing:tiled_exception_fence",
      [error_code](const Node&, const OutputTile&,
                   const std::vector<InputTile>&) {
        throw cv::Exception(error_code,
                            "deterministic injected provider exception",
                            "invoke_tiled_opencv_exception_fence_for_testing",
                            __FILE__, __LINE__);
      });
  const Node node;
  const OutputTile output;
  callback(node, output, {});
}
#endif

/**
 * @brief Applies the provider-owned OpenCV process policy exactly once.
 *
 * @return Nothing.
 * @throws std::bad_alloc when OpenCV reports initialization exhaustion.
 * @throws GraphError with `GraphErrc::ComputeError` for another OpenCV
 *         initialization failure.
 * @throws std::system_error when `std::call_once` synchronization fails.
 * @note OpenCV internal CPU threading is fixed at one before any provider
 *       callback is published. A throwing initialization remains retryable
 *       under ordinary `std::call_once` semantics. Test builds may consume one
 *       private injected OpenCV status inside the same once body before the
 *       real process call; production builds contain no injection state.
 */
static void initialize_opencv_process_policy() {
  static std::once_flag init_once;
  try {
    std::call_once(init_once, [] {
#if defined(PHOTOSPIDER_INTERNAL_OPENCV_PROVIDER_TESTING)
      const int injected_error =
          g_opencv_process_initialization_failure.exchange(
              cv::Error::StsOk, std::memory_order_acq_rel);
      if (injected_error != cv::Error::StsOk) {
        throw cv::Exception(
            injected_error, "deterministic injected initialization failure",
            "initialize_opencv_process_policy", __FILE__, __LINE__);
      }
#endif
      cv::setNumThreads(1);
    });
  } catch (const cv::Exception& error) {
    throw_translated_opencv_exception("initialization", error);
  }
}

/**
 * @brief Expands one kernel-native propagation ROI with checked arithmetic.
 *
 * @param roi Source ROI.
 * @param padding Non-negative pixels added on every side.
 * @return Expanded ROI, unchanged for non-positive padding, or empty when the
 *         expanded half-open range is not int-representable.
 * @throws Nothing.
 * @note Clipping remains the caller's responsibility because each operation
 *       has different input-extent knowledge.
 */
static PixelRect expand_roi(const PixelRect& roi, int padding) noexcept {
  return compute::expand_rect(roi, padding);
}

/**
 * @brief Builds a PixelRect from floor/ceil transformed floating-point edges.
 *
 * @param left Floating-point inclusive left edge.
 * @param top Floating-point inclusive top edge.
 * @param right Floating-point exclusive right edge.
 * @param bottom Floating-point exclusive bottom edge.
 * @return Checked integer rectangle or empty for non-finite,
 *         non-representable, or non-positive geometry.
 * @throws Nothing.
 * @note This helper centralizes the resize callbacks' outward rounding before
 *       delegating final narrowing to compute::rect_from_edges.
 */
static PixelRect rounded_pixel_rect(double left, double top, double right,
                                    double bottom) noexcept {
  const double x0 = std::floor(left);
  const double y0 = std::floor(top);
  const double x1 = std::ceil(right);
  const double y1 = std::ceil(bottom);
  constexpr double kIntMin =
      static_cast<double>(std::numeric_limits<int>::min());
  constexpr double kIntMax =
      static_cast<double>(std::numeric_limits<int>::max());
  if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
      !std::isfinite(y1) || x0 < kIntMin || y0 < kIntMin || x1 > kIntMax ||
      y1 > kIntMax) {
    return PixelRect{};
  }
  return compute::rect_from_edges(
      static_cast<std::int64_t>(x0), static_cast<std::int64_t>(y0),
      static_cast<std::int64_t>(x1), static_cast<std::int64_t>(y1));
}

/**
 * @brief Builds a PixelRect from truncation-based origin and extent values.
 *
 * @param x Floating-point origin x.
 * @param y Floating-point origin y.
 * @param width Floating-point width.
 * @param height Floating-point height.
 * @return Checked integer rectangle or empty for invalid/unrepresentable
 *         values.
 * @throws Nothing.
 * @note Ratio-mode crop execution truncates each scaled component toward zero;
 *       propagation mirrors that policy before checked endpoint construction.
 */
static PixelRect truncated_pixel_rect(double x, double y, double width,
                                      double height) noexcept {
  const double tx = std::trunc(x);
  const double ty = std::trunc(y);
  const double tw = std::trunc(width);
  const double th = std::trunc(height);
  constexpr double kIntMin =
      static_cast<double>(std::numeric_limits<int>::min());
  constexpr double kIntMax =
      static_cast<double>(std::numeric_limits<int>::max());
  if (!std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tw) ||
      !std::isfinite(th) || tx < kIntMin || ty < kIntMin || tx > kIntMax ||
      ty > kIntMax || tw <= 0.0 || th <= 0.0 || tw > kIntMax || th > kIntMax) {
    return PixelRect{};
  }
  const auto x0 = static_cast<std::int64_t>(tx);
  const auto y0 = static_cast<std::int64_t>(ty);
  return compute::rect_from_edges(x0, y0, x0 + static_cast<std::int64_t>(tw),
                                  y0 + static_cast<std::int64_t>(th));
}

/**
 * @brief Multiplies two row-major homogeneous spatial matrices.
 *
 * @param lhs Left 3-by-3 matrix.
 * @param rhs Right 3-by-3 matrix.
 * @return Row-major matrix product.
 * @throws Nothing.
 * @note The helper operates on kernel-owned scalar arrays; OpenCV matrix types
 *       are never stored in spatial metadata.
 */
static std::array<double, 9> multiply_matrix(const std::array<double, 9>& lhs,
                                             const std::array<double, 9>& rhs) {
  std::array<double, 9> out{};
  const auto index = [](int row, int column) { return row * 3 + column; };
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 3; ++k) {
        sum += lhs[index(r, k)] * rhs[index(k, c)];
      }
      out[index(r, c)] = sum;
    }
  }
  return out;
}

/**
 * @brief Builds one row-major homogeneous scale transform.
 *
 * @param scale_x Horizontal scale.
 * @param scale_y Vertical scale.
 * @return 3-by-3 scale matrix.
 * @throws Nothing.
 * @note The result remains a dependency-neutral spatial value.
 */
static std::array<double, 9> make_scale_matrix(double scale_x, double scale_y) {
  return {scale_x, 0.0, 0.0, 0.0, scale_y, 0.0, 0.0, 0.0, 1.0};
}

/**
 * @brief Builds one row-major homogeneous translation transform.
 *
 * @param translation_x Horizontal translation.
 * @param translation_y Vertical translation.
 * @return 3-by-3 translation matrix.
 * @throws Nothing.
 * @note The result crosses no OpenCV boundary.
 */
static std::array<double, 9> make_translation_matrix(double translation_x,
                                                     double translation_y) {
  return {1.0, 0.0, translation_x, 0.0, 1.0, translation_y, 0.0, 0.0, 1.0};
}

/**
 * @brief Selects the request-effective parameter map for one built-in op.
 *
 * @param node Node whose execution-local values take precedence.
 * @return Borrowed runtime map when populated, otherwise the static map.
 * @throws Nothing.
 * @note The returned reference must not outlive node.
 */
static const plugin::ParameterMap& effective_parameters(
    const Node& node) noexcept {
  return node.runtime_parameters.empty() ? node.parameters
                                         : node.runtime_parameters;
}

/**
 * @brief Reads an optional integer parameter without hiding memory exhaustion.
 *
 * @param node Node whose runtime parameters take precedence over static values.
 * @param key Parameter key to resolve.
 * @return Representable integer, or nullopt for a missing/invalid parameter.
 * @throws Nothing.
 * @note Double is accepted only when finite, integral, and int-representable.
 */
static std::optional<int> node_param_int(const Node& node,
                                         const std::string& key) {
  const plugin::ParameterValue* value =
      find_parameter(effective_parameters(node), key);
  return value == nullptr ? std::nullopt : parameter_value_as_int(*value);
}

/**
 * @brief Reads an optional floating-point parameter without hiding exhaustion.
 *
 * @param node Node whose runtime parameters take precedence over static values.
 * @param key Parameter key to resolve.
 * @return Numeric value, or nullopt for a missing/non-numeric parameter.
 * @throws Nothing.
 * @note Int64 values are converted using ordinary double precision.
 */
static std::optional<double> node_param_double(const Node& node,
                                               const std::string& key) {
  const plugin::ParameterValue* value =
      find_parameter(effective_parameters(node), key);
  return value == nullptr ? std::nullopt : parameter_value_as_double(*value);
}

/**
 * @brief Reads an optional string parameter without hiding memory exhaustion.
 *
 * @param node Node whose runtime parameters take precedence over static values.
 * @param key Parameter key to resolve.
 * @return Scalar text, or nullopt for a missing/null/container parameter.
 * @throws std::bad_alloc if string copying or numeric formatting allocates.
 * @note Bool, Int64, and Double preserve the legacy built-in scalar-to-text
 * behavior without weakening the strict public SDK accessors.
 */
static std::optional<std::string> node_param_string(const Node& node,
                                                    const std::string& key) {
  const plugin::ParameterMap& parameters = effective_parameters(node);
  const plugin::ParameterValue* value = find_parameter(parameters, key);
  if (value == nullptr || value->is_null() || value->is_array() ||
      value->is_object()) {
    return std::nullopt;
  }
  return as_str(parameters, key);
}

/**
 * @brief Borrows an object-valued entry from a ParameterMap.
 *
 * @param parameters Parameter map to inspect.
 * @param key Object entry name.
 * @return Borrowed object pointer, or nullptr when absent/wrongly typed.
 * @throws Nothing.
 * @note The pointer remains valid only while parameters is alive and unchanged.
 */
static const plugin::ParameterValue::Object* parameter_object(
    const plugin::ParameterMap& parameters, std::string_view key) noexcept {
  const plugin::ParameterValue* value = find_parameter(parameters, key);
  return value != nullptr && value->is_object() ? &value->as_object() : nullptr;
}

/**
 * @brief Borrows one nested object from an optional parent object.
 *
 * @param parent Parent object, or nullptr.
 * @param key Nested entry name.
 * @return Borrowed nested object pointer, or nullptr when unavailable.
 * @throws Nothing.
 * @note The pointer shares the parent ParameterValue lifetime.
 */
static const plugin::ParameterValue::Object* nested_parameter_object(
    const plugin::ParameterValue::Object* parent,
    std::string_view key) noexcept {
  if (parent == nullptr) {
    return nullptr;
  }
  const auto value = parent->find(key);
  return value != parent->end() && value->second.is_object()
             ? &value->second.as_object()
             : nullptr;
}

/**
 * @brief Parses one string-keyed source-channel index.
 *
 * @param text Complete decimal key text.
 * @return Exact int on success, otherwise std::nullopt.
 * @throws Nothing.
 * @note Leading/trailing characters and overflow are rejected.
 */
static std::optional<int> parse_channel_index(std::string_view text) noexcept {
  int index = -1;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, index);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return index;
}

/**
 * @brief Finds the largest valid destination channel in one mapping object.
 *
 * @param mapping Source-channel keys mapped to destination-index arrays.
 * @return Largest int-representable destination, or -1 when none exists.
 * @throws Nothing.
 * @note Invalid entries are skipped to preserve the existing optional mapping
 * semantics.
 */
static int max_destination_channel(
    const plugin::ParameterValue::Object* mapping) noexcept {
  int maximum = -1;
  if (mapping == nullptr) {
    return maximum;
  }
  for (const auto& [source, destinations] : *mapping) {
    (void)source;
    if (!destinations.is_array()) {
      continue;
    }
    for (const plugin::ParameterValue& destination : destinations.as_array()) {
      const std::optional<int> index = parameter_value_as_int(destination);
      if (index) {
        maximum = std::max(maximum, *index);
      }
    }
  }
  return maximum;
}

/**
 * @brief Marks valid mapped destinations in an existing coverage vector.
 *
 * @param mapping Optional source-to-destination mapping.
 * @param covered Destination flags to update in place.
 * @return Nothing.
 * @throws Nothing.
 * @note Out-of-range and incorrectly typed entries are ignored.
 */
static void mark_mapped_channels(const plugin::ParameterValue::Object* mapping,
                                 std::vector<char>& covered) noexcept {
  if (mapping == nullptr) {
    return;
  }
  for (const auto& [source, destinations] : *mapping) {
    (void)source;
    if (!destinations.is_array()) {
      continue;
    }
    for (const plugin::ParameterValue& destination : destinations.as_array()) {
      const std::optional<int> index = parameter_value_as_int(destination);
      if (index && *index >= 0 && *index < static_cast<int>(covered.size())) {
        covered[*index] = 1;
      }
    }
  }
}

/**
 * @brief Accumulates mapped source planes into destination planes.
 *
 * @param mapping Optional source-to-destination mapping.
 * @param source_planes Immutable source channel matrices.
 * @param weight Scalar applied to each mapped source contribution.
 * @param destination_planes Destination matrices updated in place.
 * @return Nothing.
 * @throws cv::Exception if OpenCV multiplication or addition fails.
 * @throws std::bad_alloc if OpenCV temporary storage cannot allocate.
 * @note Invalid source keys, destination values, and indexes are skipped.
 */
static void apply_channel_mapping(const plugin::ParameterValue::Object* mapping,
                                  const std::vector<cv::Mat>& source_planes,
                                  double weight,
                                  std::vector<cv::Mat>& destination_planes) {
  if (mapping == nullptr) {
    return;
  }
  for (const auto& [source_text, destinations] : *mapping) {
    const std::optional<int> source = parse_channel_index(source_text);
    if (!source || *source < 0 ||
        *source >= static_cast<int>(source_planes.size()) ||
        !destinations.is_array()) {
      continue;
    }
    for (const plugin::ParameterValue& destination : destinations.as_array()) {
      const std::optional<int> index = parameter_value_as_int(destination);
      if (!index || *index < 0 ||
          *index >= static_cast<int>(destination_planes.size())) {
        continue;
      }
      cv::add(destination_planes[*index], source_planes[*source] * weight,
              destination_planes[*index]);
    }
  }
}

/**
 * @brief Reads a valid cached image extent without exposing provider geometry.
 *
 * @param output_opt Optional cached node output.
 * @return Positive kernel-native extent, otherwise an empty PixelSize.
 * @throws Nothing.
 * @note Pixel storage ownership stays in NodeOutput; only scalar dimensions
 *       cross into ROI propagation.
 */
static PixelSize cached_image_size(
    const std::optional<NodeOutput>& output_opt) noexcept {
  if (!output_opt)
    return PixelSize{};
  const auto& buf = output_opt->image_buffer;
  if (buf.width <= 0 || buf.height <= 0)
    return PixelSize{};
  return PixelSize{buf.width, buf.height};
}

/**
 * @brief Infers an input extent without forcing graph execution.
 *
 * @param node Node whose runtime hint and parent caches are inspected.
 * @param graph Graph used to resolve image-input parents.
 * @return First positive kernel-native extent, otherwise empty.
 * @throws Nothing.
 * @note Runtime HP history takes precedence over parent cached outputs.
 */
static PixelSize infer_input_size_hint(const Node& node,
                                       const GraphModel& graph) noexcept {
  if (node.last_input_size_hp && node.last_input_size_hp->width > 0 &&
      node.last_input_size_hp->height > 0) {
    return *node.last_input_size_hp;
  }
  for (const auto& input : node.image_inputs) {
    if (input.from_node_id < 0)
      continue;
    const Node* parent = graph.find_node(input.from_node_id);
    if (!parent)
      continue;
    PixelSize size = cached_image_size(parent->cached_output_high_precision);
    if (size.width > 0 && size.height > 0)
      return size;
  }
  return PixelSize{};
}

/**
 * @brief Maps Gaussian-blur output dirtiness to its halo-expanded input ROI.
 *
 * @param node Blur node supplying operation identity.
 * @param downstream_roi Dirty output ROI.
 * @param graph Unused graph context required by the callback contract.
 * @param output_extent Unused output extent.
 * @param input_extents Unused input extents.
 * @param parameters Effective request-local parameters.
 * @param inputs Unused resolved input snapshots.
 * @return Kernel-native input demand expanded by the effective blur radius.
 * @throws Nothing.
 * @note The planner clips the returned demand to the resolved input extent.
 */
static PixelRect gaussian_blur_dirty_roi(
    const Node& node, const PixelRect& downstream_roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* inputs) noexcept {
  (void)graph;
  (void)output_extent;
  (void)input_extents;
  (void)inputs;
  const int radius =
      ops::builtin_input_halo_radius(node.type, node.subtype, parameters);
  return expand_roi(downstream_roi, radius);
}

/**
 * @brief Maps convolution output dirtiness to its halo-expanded input ROI.
 *
 * @param node Convolution node supplying operation identity.
 * @param downstream_roi Dirty output ROI.
 * @param graph Unused graph context required by the callback contract.
 * @param output_extent Unused output extent.
 * @param input_extents Unused input extents.
 * @param parameters Effective request-local parameters.
 * @param inputs Unused resolved input snapshots.
 * @return Kernel-native input demand expanded by the effective kernel radius.
 * @throws Nothing.
 * @note The planner performs extent clipping after this operation-specific
 *       expansion.
 */
static PixelRect convolve_dirty_roi(
    const Node& node, const PixelRect& downstream_roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* inputs) noexcept {
  (void)graph;
  (void)output_extent;
  (void)input_extents;
  (void)inputs;
  const int radius =
      ops::builtin_input_halo_radius(node.type, node.subtype, parameters);
  return expand_roi(downstream_roi, radius);
}

/**
 * @brief Returns conservative source padding for resize interpolation.
 *
 * @param node Resize node whose effective interpolation parameter is read.
 * @return Zero for nearest, two for cubic, and one for area/linear/default.
 * @throws std::bad_alloc if scalar-to-string parameter formatting allocates.
 * @note The value is used only by provider ROI callbacks and mirrors the
 *       OpenCV algorithm selection in `op_resize`.
 */
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

/**
 * @brief Projects a resized output ROI backward into its source extent.
 *
 * @param node Resize node with target dimensions and interpolation policy.
 * @param downstream_roi Dirty ROI in resized output coordinates.
 * @param graph Graph used for best-effort source extent resolution.
 * @param output_extent Unused resolved output extent.
 * @param input_extents Unused callback input extents.
 * @param parameters Unused effective parameter snapshot.
 * @param inputs Unused resolved input snapshots.
 * @return Clipped kernel-native source ROI, or empty when required dimensions
 *         are unavailable or invalid.
 * @throws std::bad_alloc if parameter string formatting exhausts memory.
 * @note Floating-point scale edges round outward and checked geometry rejects
 *       non-representable results.
 */
static PixelRect resize_dirty_roi(
    const Node& node, const PixelRect& downstream_roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* inputs) {
  (void)output_extent;
  (void)input_extents;
  (void)parameters;
  (void)inputs;
  if (downstream_roi.width <= 0 || downstream_roi.height <= 0) {
    return PixelRect{};
  }

  auto maybe_out_w = node_param_int(node, "width");
  auto maybe_out_h = node_param_int(node, "height");
  if (!maybe_out_w || !maybe_out_h || *maybe_out_w <= 0 || *maybe_out_h <= 0) {
    return PixelRect{};
  }

  const PixelSize input_size = infer_input_size_hint(node, graph);
  if (input_size.width <= 0 || input_size.height <= 0) {
    return PixelRect{};
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
      (static_cast<double>(downstream_roi.x) + downstream_roi.width) * scale_x +
      static_cast<double>(pad);
  const double bottom =
      (static_cast<double>(downstream_roi.y) + downstream_roi.height) *
          scale_y +
      static_cast<double>(pad);

  return compute::clip_rect(rounded_pixel_rect(left, top, right, bottom),
                            input_size);
}

/**
 * @brief Projects a crop output ROI backward into source coordinates.
 *
 * @param node Crop node with ratio- or value-mode geometry.
 * @param downstream_roi Dirty ROI in cropped output coordinates.
 * @param graph Graph used for best-effort source extent resolution.
 * @param output_extent Unused resolved output extent.
 * @param input_extents Unused callback input extents.
 * @param parameters Unused effective parameter snapshot.
 * @param inputs Unused resolved input snapshots.
 * @return Kernel-native source ROI clipped to known bounds, or empty for
 *         invalid crop geometry.
 * @throws std::bad_alloc if parameter string formatting exhausts memory.
 * @note Ratio-mode truncation matches execution; intersections and translation
 *       use checked half-open geometry.
 */
static PixelRect crop_dirty_roi(const Node& node,
                                const PixelRect& downstream_roi,
                                const GraphModel& graph,
                                const PixelSize& output_extent,
                                const std::vector<PixelSize>& input_extents,
                                const plugin::ParameterMap& parameters,
                                const std::vector<const NodeOutput*>* inputs) {
  (void)output_extent;
  (void)input_extents;
  (void)parameters;
  (void)inputs;
  if (downstream_roi.width <= 0 || downstream_roi.height <= 0) {
    return PixelRect{};
  }

  const PixelSize input_size = infer_input_size_hint(node, graph);
  const bool have_input_size = input_size.width > 0 && input_size.height > 0;
  const PixelRect input_bounds =
      have_input_size ? PixelRect{0, 0, input_size.width, input_size.height}
                      : PixelRect{};

  std::string mode = as_str(node.runtime_parameters, "mode", "");
  if (mode.empty())
    mode = as_str(node.parameters, "mode", "value");
  if (mode.empty())
    mode = "value";

  PixelRect crop_rect;
  if (mode == "ratio") {
    if (!have_input_size) {
      return PixelRect{};
    }
    auto rx_opt = node_param_double(node, "x");
    auto ry_opt = node_param_double(node, "y");
    auto rw_opt = node_param_double(node, "width");
    auto rh_opt = node_param_double(node, "height");
    if (!rx_opt || !ry_opt || !rw_opt || !rh_opt) {
      return PixelRect{};
    }
    double rx = *rx_opt;
    double ry = *ry_opt;
    double rw = *rw_opt;
    double rh = *rh_opt;
    if (rx < 0.0 || ry < 0.0 || rw <= 0.0 || rh <= 0.0) {
      return PixelRect{};
    }
    crop_rect =
        truncated_pixel_rect(rx * input_size.width, ry * input_size.height,
                             rw * input_size.width, rh * input_size.height);
    crop_rect = compute::intersect_rect(crop_rect, input_bounds);
    if (crop_rect.width <= 0 || crop_rect.height <= 0) {
      return PixelRect{};
    }
  } else {
    auto w_opt = node_param_int(node, "width");
    auto h_opt = node_param_int(node, "height");
    if (!w_opt || !h_opt || *w_opt <= 0 || *h_opt <= 0) {
      return PixelRect{};
    }
    int x = node_param_int(node, "x").value_or(0);
    int y = node_param_int(node, "y").value_or(0);
    crop_rect =
        compute::rect_from_edges(x, y, static_cast<std::int64_t>(x) + *w_opt,
                                 static_cast<std::int64_t>(y) + *h_opt);
    if (have_input_size) {
      crop_rect = compute::intersect_rect(crop_rect, input_bounds);
      if (crop_rect.width <= 0 || crop_rect.height <= 0) {
        return PixelRect{};
      }
    }
  }

  const PixelRect valid_downstream_roi = compute::clip_rect(
      downstream_roi, PixelSize{crop_rect.width, crop_rect.height});
  if (valid_downstream_roi.width <= 0 || valid_downstream_roi.height <= 0) {
    return PixelRect{};
  }

  return compute::translate_rect(valid_downstream_roi, crop_rect.x,
                                 crop_rect.y);
}

/**
 * @brief Projects changed Gaussian-blur input pixels into output coordinates.
 *
 * @param node Blur node supplying operation identity.
 * @param upstream_roi Changed source ROI.
 * @param graph Unused graph context.
 * @param parent_size Source extent.
 * @param child_size Unused destination extent.
 * @param input_index Unused input slot.
 * @param input_extents Unused input extent list.
 * @param parameters Effective request-local parameters.
 * @return Source-clipped ROI expanded by the effective blur radius.
 * @throws Nothing.
 * @note Expansion intentionally follows clipping to preserve prior propagation
 *       semantics; downstream planning owns destination clipping.
 */
static PixelRect gaussian_blur_forward_roi(
    const Node& node, const PixelRect& upstream_roi, const GraphModel& graph,
    const PixelSize& parent_size, const PixelSize& child_size,
    size_t input_index, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters) noexcept {
  (void)graph;
  (void)child_size;
  (void)input_index;
  (void)input_extents;
  if (upstream_roi.width <= 0 || upstream_roi.height <= 0) {
    return PixelRect{};
  }
  const int radius =
      ops::builtin_input_halo_radius(node.type, node.subtype, parameters);
  return expand_roi(compute::clip_rect(upstream_roi, parent_size), radius);
}

/**
 * @brief Projects changed convolution input pixels into output coordinates.
 *
 * @param node Convolution node supplying operation identity.
 * @param upstream_roi Changed source ROI.
 * @param graph Unused graph context.
 * @param parent_size Source extent.
 * @param child_size Unused destination extent.
 * @param input_index Unused input slot.
 * @param input_extents Unused input extent list.
 * @param parameters Effective request-local parameters.
 * @return Source-clipped ROI expanded by the effective kernel radius.
 * @throws Nothing.
 * @note Downstream planning clips the expanded result to its destination
 *       extent.
 */
static PixelRect convolve_forward_roi(
    const Node& node, const PixelRect& upstream_roi, const GraphModel& graph,
    const PixelSize& parent_size, const PixelSize& child_size,
    size_t input_index, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters) noexcept {
  (void)graph;
  (void)child_size;
  (void)input_index;
  (void)input_extents;
  if (upstream_roi.width <= 0 || upstream_roi.height <= 0) {
    return PixelRect{};
  }
  const int radius =
      ops::builtin_input_halo_radius(node.type, node.subtype, parameters);
  return expand_roi(compute::clip_rect(upstream_roi, parent_size), radius);
}

/**
 * @brief Projects a source ROI forward through resize geometry.
 *
 * @param node Resize node with interpolation policy.
 * @param upstream_roi Changed source ROI.
 * @param graph Unused graph context.
 * @param parent_size Source extent.
 * @param child_size Destination extent.
 * @param input_index Unused input slot.
 * @param input_extents Unused input extent list.
 * @param parameters Unused effective parameter snapshot.
 * @return Outward-rounded destination ROI or empty for invalid extents.
 * @throws std::bad_alloc if interpolation parameter lookup exhausts memory.
 * @note Destination clipping remains the propagation service's responsibility.
 */
static PixelRect resize_forward_roi(
    const Node& node, const PixelRect& upstream_roi, const GraphModel& graph,
    const PixelSize& parent_size, const PixelSize& child_size,
    size_t input_index, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters) {
  (void)graph;
  (void)input_index;
  (void)input_extents;
  (void)parameters;
  if (upstream_roi.width <= 0 || upstream_roi.height <= 0) {
    return PixelRect{};
  }
  if (parent_size.width <= 0 || parent_size.height <= 0 ||
      child_size.width <= 0 || child_size.height <= 0) {
    return PixelRect{};
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
      (static_cast<double>(upstream_roi.x) + upstream_roi.width) * scale_x +
      static_cast<double>(pad);
  const double bottom =
      (static_cast<double>(upstream_roi.y) + upstream_roi.height) * scale_y +
      static_cast<double>(pad);

  return rounded_pixel_rect(left, top, right, bottom);
}

/**
 * @brief Projects a source ROI forward into crop-local coordinates.
 *
 * @param node Crop node with ratio- or value-mode geometry.
 * @param upstream_roi Changed source ROI.
 * @param graph Unused graph context.
 * @param parent_size Source extent.
 * @param child_size Unused destination extent.
 * @param input_index Unused input slot.
 * @param input_extents Unused input extent list.
 * @param parameters Unused effective parameter snapshot.
 * @return Kernel-native crop-local intersection or empty for invalid geometry.
 * @throws std::bad_alloc if parameter string formatting exhausts memory.
 * @note Ratio-mode truncation matches crop execution; checked translation
 *       prevents overflow at extreme origins.
 */
static PixelRect crop_forward_roi(
    const Node& node, const PixelRect& upstream_roi, const GraphModel& graph,
    const PixelSize& parent_size, const PixelSize& child_size,
    size_t input_index, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters) {
  (void)graph;
  (void)child_size;
  (void)input_index;
  (void)input_extents;
  (void)parameters;
  if (upstream_roi.width <= 0 || upstream_roi.height <= 0) {
    return PixelRect{};
  }
  std::string mode = node_param_string(node, "mode").value_or("value");
  PixelRect crop_rect;
  if (mode == "ratio") {
    if (parent_size.width <= 0 || parent_size.height <= 0)
      return PixelRect{};
    auto rx = node_param_double(node, "x");
    auto ry = node_param_double(node, "y");
    auto rw = node_param_double(node, "width");
    auto rh = node_param_double(node, "height");
    if (!rx || !ry || !rw || !rh)
      return PixelRect{};
    crop_rect =
        truncated_pixel_rect(*rx * parent_size.width, *ry * parent_size.height,
                             *rw * parent_size.width, *rh * parent_size.height);
  } else {
    int x = node_param_int(node, "x").value_or(0);
    int y = node_param_int(node, "y").value_or(0);
    auto w = node_param_int(node, "width");
    auto h = node_param_int(node, "height");
    if (!w || !h || *w <= 0 || *h <= 0)
      return PixelRect{};
    crop_rect =
        compute::rect_from_edges(x, y, static_cast<std::int64_t>(x) + *w,
                                 static_cast<std::int64_t>(y) + *h);
  }

  const PixelRect intersect = compute::intersect_rect(
      compute::clip_rect(upstream_roi, parent_size), crop_rect);
  if (intersect.width <= 0 || intersect.height <= 0) {
    return PixelRect{};
  }
  return compute::translate_rect(intersect,
                                 -static_cast<std::int64_t>(crop_rect.x),
                                 -static_cast<std::int64_t>(crop_rect.y));
}

/**
 * @brief Preserves ROI geometry for pointwise or data-only operations.
 *
 * @param node Unused operation node.
 * @param upstream_roi Changed source ROI.
 * @param graph Unused graph context.
 * @param parent_size Unused source extent.
 * @param child_size Unused destination extent.
 * @param input_index Unused input slot.
 * @param input_extents Unused input extent list.
 * @param parameters Unused effective parameter snapshot.
 * @return The unchanged kernel-native ROI.
 * @throws Nothing.
 * @note The propagation service performs graph-level clipping and merging.
 */
static PixelRect identity_forward_roi(
    const Node& node, const PixelRect& upstream_roi, const GraphModel& graph,
    const PixelSize& parent_size, const PixelSize& child_size,
    size_t input_index, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters) noexcept {
  (void)node;
  (void)graph;
  (void)parent_size;
  (void)child_size;
  (void)input_index;
  (void)input_extents;
  (void)parameters;
  return upstream_roi;
}

/**
 * @brief Preserves downstream dirty demand for pointwise or source operations.
 *
 * @param node Unused operation node.
 * @param downstream_roi Dirty output ROI.
 * @param graph Unused graph context.
 * @param output_extent Unused output extent.
 * @param input_extents Unused input extent list.
 * @param parameters Unused effective parameter snapshot.
 * @param inputs Unused resolved input snapshots.
 * @return The unchanged kernel-native dirty ROI.
 * @throws Nothing.
 * @note Graph-level planning performs extent clipping after this callback.
 */
static PixelRect identity_dirty_roi(
    const Node& node, const PixelRect& downstream_roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* inputs) noexcept {
  (void)node;
  (void)graph;
  (void)output_extent;
  (void)input_extents;
  (void)parameters;
  (void)inputs;
  return downstream_roi;
}

/**
 * @brief Conservatively expands dirty demand for displacement-style nodes.
 *
 * @param node Node with optional displacement or brush radius.
 * @param downstream_roi Dirty output ROI.
 * @param graph Unused graph context.
 * @param output_extent Unused output extent.
 * @param input_extents Unused input extent list.
 * @param parameters Unused effective parameter snapshot.
 * @param inputs Unused resolved input snapshots.
 * @return Expanded kernel-native demand or unchanged ROI when no displacement
 *         is declared.
 * @throws std::bad_alloc if parameter string formatting exhausts memory.
 * @note The callback is currently retained for future built-in registration.
 */
[[maybe_unused]] static PixelRect conservative_dirty_roi(
    const Node& node, const PixelRect& downstream_roi, const GraphModel& graph,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    const plugin::ParameterMap& parameters,
    const std::vector<const NodeOutput*>* inputs) {
  (void)graph;
  (void)output_extent;
  (void)input_extents;
  (void)parameters;
  (void)inputs;
  if (downstream_roi.width <= 0 || downstream_roi.height <= 0) {
    return PixelRect{};
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

/**
 * @brief Loads one image file and normalizes scalar storage to float32.
 *
 * @param node Source node carrying a required static `path` parameter.
 * @param inputs Unused source inputs.
 * @return Owned CPU image normalized to `[0,1]` for unsigned 8/16-bit files.
 * @throws GraphError with `GraphErrc::InvalidParameter` for an empty path or
 *         `GraphErrc::Io` when decoding produces no image.
 * @throws GraphError with `GraphErrc::Io` when the injected production codec
 * cannot decode or convert the image artifact.
 * @throws std::bad_alloc for codec or output ownership allocation failure.
 * @note The operation owns no filesystem codec implementation. It obtains the
 * configured artifact codec from the product composition boundary and returns
 * the codec-owned ImageBuffer unchanged.
 */
static NodeOutput op_image_source_path(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  (void)inputs;
  const auto& P = node.parameters;
  std::string path = as_str(P, "path");
  if (path.empty())
    throw GraphError(GraphErrc::InvalidParameter,
                     "image_source:path requires parameters.path");

  const auto codec = providers::make_configured_image_artifact_codec();
  NodeOutput result;
  result.image_buffer = codec->decode(path);
  return result;
}

/**
 * @brief Creates one uniform float32 image from request-effective parameters.
 *
 * @param node Generator node carrying width, height, channels, and byte-scale
 *        value parameters.
 * @param inputs Unused generator inputs.
 * @return Owned CPU image whose channel values equal `value / 255`.
 * @throws cv::Exception internally for invalid shape/channel or allocation
 *         failure; the provider fence translates the exception category.
 * @throws std::bad_alloc if host ownership storage allocation fails.
 * @note The callback owns only invocation-local OpenCV matrix state.
 */
static NodeOutput op_constant_image(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  (void)inputs;
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

/**
 * @brief Generates one float32 Perlin-noise image on the CPU.
 *
 * @param node Generator node carrying width, height, grid size, and seed.
 * @param inputs Unused generator inputs.
 * @return Owned single-channel noise image with values normalized to `[0,1]`.
 * @throws GraphError with `GraphErrc::InvalidParameter` for non-positive
 *         dimensions or grid size.
 * @throws cv::Exception internally for OpenCV allocation/access failure; the
 *         provider fence translates it before registry return.
 * @throws std::bad_alloc if permutation or image ownership allocation fails.
 * @note A seed of `-1` uses `std::random_device`; every other seed is
 *       deterministic. All permutation and matrix state is callback-local.
 */
static NodeOutput op_perlin_noise(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  (void)inputs;
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

/**
 * @brief Applies a full-image floating-point convolution on the CPU provider.
 * @param node Effective padding, absolute-value, and gradient parameters.
 * @param inputs Source image followed by a single-channel kernel image.
 * @return Owned filtered image output.
 * @throws std::bad_alloc if parameter or output storage allocation fails.
 * @throws GraphError for missing inputs or invalid kernels.
 * @throws cv::Exception when OpenCV rejects the convolution; the provider
 *         callback fence translates it to `GraphError`.
 * @note All OpenCV objects are callback-local `cv::Mat` values. Independent
 *       invocations are reentrant and receive no outer operation mutex.
 */
static NodeOutput op_convolve(const Node& node,
                              const std::vector<const NodeOutput*>& inputs) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_process:convolve");
  // Defensive checks against cold-start/invalid inputs
  if (inputs.size() < 2 || inputs[0]->image_buffer.width == 0 ||
      inputs[0]->image_buffer.height == 0 ||
      inputs[1]->image_buffer.width == 0 ||
      inputs[1]->image_buffer.height == 0) {
    throw GraphError(
        GraphErrc::MissingDependency,
        "Convolve requires two non-empty input images (src and kernel).");
  }

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
  if (h_and_v) {
    cv::Mat gx, gy, transposed_kernel;
    cv::filter2D(src, gx, CV_32F, kernel, cv::Point(-1, -1), 0, border_type);
    cv::transpose(kernel, transposed_kernel);
    cv::filter2D(src, gy, CV_32F, transposed_kernel, cv::Point(-1, -1), 0,
                 border_type);
    cv::magnitude(gx, gy, out_f32);
    if (take_absolute) {
      cv::absdiff(out_f32, cv::Scalar::all(0), out_f32);
    }
  } else {
    cv::filter2D(src, out_f32, CV_32F, kernel, cv::Point(-1, -1), 0,
                 border_type);
    if (take_absolute) {
      cv::absdiff(out_f32, cv::Scalar::all(0), out_f32);
    }
  }

  NodeOutput result;
  result.image_buffer = fromCvMat(out_f32);
  return result;
}

/**
 * @brief Resizes one full CPU image and propagates its spatial transform.
 * @param node Effective output dimensions and interpolation mode.
 * @param inputs One required source image with spatial metadata.
 * @return Owned resized image and updated spatial metadata.
 * @throws std::bad_alloc if parameter or output storage allocation fails.
 * @throws GraphError for missing input or non-positive output dimensions.
 * @throws cv::Exception if OpenCV resize or buffer conversion fails.
 * @note The implementation is CPU-only `cv::Mat` work and is reentrant across
 *       independent callback invocations.
 */
static NodeOutput op_resize(const Node& node,
                            const std::vector<const NodeOutput*>& inputs) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_process:resize");
  if (inputs.empty() || inputs[0]->image_buffer.width == 0)
    throw GraphError(GraphErrc::MissingDependency,
                     "Resize requires an input image.");

  cv::Mat input = toCvMat(inputs[0]->image_buffer);

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
  cv::Mat output;
  cv::resize(input, output, cv::Size(width, height), 0, 0, flag);
  NodeOutput result;
  result.image_buffer = fromCvMat(output);
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

/**
 * @brief Crops or pads one full CPU image and updates spatial coordinates.
 * @param node Effective value- or ratio-mode crop rectangle.
 * @param inputs One required source image with spatial metadata.
 * @return Owned crop canvas and translated spatial metadata.
 * @throws std::bad_alloc if parameter or output storage allocation fails.
 * @throws GraphError for missing input or invalid crop dimensions.
 * @throws cv::Exception if OpenCV view, copy, or buffer conversion fails.
 * @note The callback uses only local `cv::Mat` headers and independent output
 *       storage, so execution workers may invoke it concurrently. Spatial ROI
 *       endpoints are derived with checked kernel geometry before publication.
 */
static NodeOutput op_crop(const Node& node,
                          const std::vector<const NodeOutput*>& inputs) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_process:crop");
  if (inputs.empty() || inputs[0]->image_buffer.width == 0)
    throw GraphError(GraphErrc::MissingDependency,
                     "Crop requires an input image.");

  cv::Mat src = toCvMat(inputs[0]->image_buffer);

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
    x = rx * src.cols;
    y = ry * src.rows;
    w = rw * src.cols;
    h = rh * src.rows;
  } else {
    x = as_int_flexible(P, "x", -1);
    y = as_int_flexible(P, "y", -1);
    w = as_int_flexible(P, "width", -1);
    h = as_int_flexible(P, "height", -1);
    if (w <= 0 || h <= 0)
      throw GraphError(GraphErrc::InvalidParameter,
                       "Crop value mode requires positive width and height.");
  }
  cv::Mat canvas = cv::Mat::zeros(h, w, src.type());
  cv::Rect src_rect(0, 0, src.cols, src.rows), crop_rect(x, y, w, h);
  cv::Rect intersect = src_rect & crop_rect;
  cv::Rect dst_roi(intersect.x - x, intersect.y - y, intersect.width,
                   intersect.height);
  if (intersect.width > 0 && intersect.height > 0)
    src(intersect).copyTo(canvas(dst_roi));
  NodeOutput result;
  result.image_buffer = fromCvMat(canvas);
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
    const PixelRect parent_world = in_space.absolute_roi;
    const std::int64_t world_x = static_cast<std::int64_t>(parent_world.x) + x;
    const std::int64_t world_y = static_cast<std::int64_t>(parent_world.y) + y;
    const PixelRect world_request =
        compute::rect_from_edges(world_x, world_y, world_x + w, world_y + h);
    const PixelRect clipped =
        compute::intersect_rect(world_request, parent_world);
    if (clipped.width > 0 && clipped.height > 0) {
      result.space.absolute_roi = clipped;
    } else {
      result.space.absolute_roi = world_request;
    }
  } else {
    result.space.absolute_roi =
        compute::rect_from_edges(x, y, static_cast<std::int64_t>(x) + w,
                                 static_cast<std::int64_t>(y) + h);
  }
  return result;
}

/**
 * @brief Extracts one named channel from a full CPU image.
 * @param node Effective channel name or numeric alias.
 * @param inputs One required source image.
 * @return Owned single-channel output image.
 * @throws std::bad_alloc if parameter or channel storage allocation fails.
 * @throws GraphError for missing input or an unavailable channel.
 * @throws cv::Exception if OpenCV split or buffer conversion fails.
 * @note The callback is stateless `cv::Mat` work and is safe for concurrent
 *       invocation on independent inputs.
 */
static NodeOutput op_extract_channel(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_process:extract_channel");
  if (inputs.empty() || inputs[0]->image_buffer.width == 0)
    throw GraphError(GraphErrc::MissingDependency,
                     "Extract channel requires an input image.");

  cv::Mat input = toCvMat(inputs[0]->image_buffer);

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
  if (ch_idx < 0 || input.channels() <= ch_idx)
    throw GraphError(GraphErrc::InvalidParameter,
                     "Invalid or unavailable channel for extraction.");
  std::vector<cv::Mat> channels;
  cv::split(input, channels);
  NodeOutput result;
  result.image_buffer = fromCvMat(channels[ch_idx]);
  return result;
}

// =============================================================================
// ==                       类型二: TILED (分块计算) 操作 ==
// =============================================================================
/**
 * @brief Applies the pointwise curve transform to one independently owned tile.
 * @param node Effective curve coefficient.
 * @param output_tile Writable destination tile owned by the current task.
 * @param input_tiles One immutable normalized input tile.
 * @return Nothing.
 * @throws std::bad_alloc if parameter or temporary matrix allocation fails.
 * @throws GraphError if the required input tile is missing.
 * @throws cv::Exception if OpenCV arithmetic or adapter conversion fails.
 * @note Local `cv::Mat` headers share only task-owned payloads. Multiple
 *       execution workers may execute this callback concurrently.
 */
static void op_curve_transform_tiled(
    const Node& node, const OutputTile& output_tile,
    const std::vector<InputTile>& input_tiles) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_process:curve_transform");

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

/**
 * @brief Blurs one output tile from an immutable halo-expanded CPU input.
 * @param node Effective kernel size and sigma.
 * @param output_tile Writable destination tile owned by the current task.
 * @param input_tiles One normalized input tile including the required halo.
 * @return Nothing.
 * @throws std::bad_alloc if parameter or temporary matrix allocation fails.
 * @throws GraphError if the required input tile is missing.
 * @throws std::runtime_error if planned halo geometry cannot cover output.
 * @throws cv::Exception if OpenCV blur, copy, or adapter conversion fails.
 * @note Every invocation owns its temporary matrix and output region, so the
 *       callback is reentrant across execution workers.
 */
static void op_gaussian_blur_tiled(const Node& node,
                                   const OutputTile& output_tile,
                                   const std::vector<InputTile>& input_tiles) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_process:gaussian_blur");

  if (input_tiles.empty()) {
    throw GraphError(GraphErrc::MissingDependency,
                     "gaussian_blur requires one input tile with halo.");
  }

  const InputTile& input_tile_with_halo = input_tiles[0];
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

/**
 * @brief Blurs one complete CPU image for monolithic execution.
 * @param node Effective kernel size and sigma.
 * @param inputs One required full-image input.
 * @return Owned blurred image output.
 * @throws std::bad_alloc if parameter or output allocation fails.
 * @throws GraphError if the required input is missing.
 * @throws cv::Exception if OpenCV blur or adapter conversion fails.
 * @note Callback-local `cv::Mat` values make independent invocations reentrant.
 */
static NodeOutput op_gaussian_blur_monolithic(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_process:gaussian_blur");
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

/**
 * @brief Blends two input tiles with optional per-channel value mappings.
 *
 * @param node Operation node providing weights, mapping, and alpha strategy.
 * @param output_tile Writable output tile.
 * @param input_tiles Two normalized input tiles.
 * @return Nothing.
 * @throws std::bad_alloc if parameter copying or temporary channel storage
 * exhausts memory.
 * @throws GraphError for missing/mismatched inputs or invalid strategy data.
 * @throws cv::Exception for OpenCV blending/channel failures.
 * @note Invalid individual channel-map entries are skipped, while memory
 *       exhaustion remains exceptional for the public Host compute boundary.
 *       All mutable matrices are callback-local or task-owned, so independent
 *       execution invocations are reentrant.
 */
static void op_add_weighted_tiled(const Node& node,
                                  const OutputTile& output_tile,
                                  const std::vector<InputTile>& input_tiles) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_mixing:add_weighted");

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
  const plugin::ParameterValue::Object* channel_mapping =
      parameter_object(P, "channel_mapping");
  const bool has_mapping = channel_mapping != nullptr;

  if (!has_mapping) {
    // Default: weighted blend per channel
    cv::addWeighted(input_a, alpha, input_b, beta, gamma, output);
    return;
  }

  // Prepare output planes
  int in_ch = input_a.channels();
  int out_ch = in_ch;

  const plugin::ParameterValue::Object* ch_input0 =
      nested_parameter_object(channel_mapping, "input0");
  const plugin::ParameterValue::Object* ch_input1 =
      nested_parameter_object(channel_mapping, "input1");
  int max0 = max_destination_channel(ch_input0);
  int max1 = max_destination_channel(ch_input1);
  int maxd = std::max({max0, max1, in_ch - 1});
  out_ch = std::max(out_ch, maxd + 1);

  // Split inputs into planes
  std::vector<cv::Mat> A, B;
  cv::split(input_a, A);
  cv::split(input_b, B);
  // Ensure plane count
  if (static_cast<int>(A.size()) < out_ch)
    A.resize(out_ch, cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1));
  if (static_cast<int>(B.size()) < out_ch)
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
  mark_mapped_channels(ch_input0, covered);
  mark_mapped_channels(ch_input1, covered);
  for (int d = 0; d < out_ch; ++d) {
    if (covered[d]) {
      if (gamma != 0.0)
        O[d] = cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(gamma));
      else
        O[d] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    }
  }

  apply_channel_mapping(ch_input0, A, alpha, O);
  apply_channel_mapping(ch_input1, B, beta, O);

  // Optional alpha_strategy for dest alpha channel
  std::string alpha_strategy = as_str(P, "alpha_strategy", "weighted");
  if ((alpha_strategy != "weighted") && out_ch >= 4) {
    int aidx = 3;
    if (alpha_strategy == "max") {
      O[aidx] =
          cv::max(A.size() > 3 ? A[3] : O[aidx], B.size() > 3 ? B[3] : O[aidx]);
    } else if (alpha_strategy == "copy0") {
      if (static_cast<int>(A.size()) > 3)
        O[aidx] = A[3].clone();
    } else if (alpha_strategy == "copy1") {
      if (static_cast<int>(B.size()) > 3)
        O[aidx] = B[3].clone();
    } else if (alpha_strategy == "set1") {
      O[aidx] = cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(1.0));
    } else if (alpha_strategy == "set0") {
      O[aidx] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    }
  }

  cv::merge(O, output);
}

/**
 * @brief Computes absolute difference for two normalized CPU input tiles.
 * @param node Effective alpha-channel strategy.
 * @param output_tile Writable destination tile owned by the current task.
 * @param input_tiles Two immutable normalized input tiles.
 * @return Nothing.
 * @throws std::bad_alloc if parameter or channel storage allocation fails.
 * @throws GraphError for missing or mismatched inputs.
 * @throws cv::Exception if OpenCV channel arithmetic or merging fails.
 * @note Temporary channels and the output region are task-owned, permitting
 *       concurrent execution invocation without outer serialization.
 */
static void op_abs_diff_tiled(const Node& node, const OutputTile& output_tile,
                              const std::vector<InputTile>& input_tiles) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_mixing:diff");

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

/**
 * @brief Multiplies two normalized CPU input tiles pointwise.
 * @param node Effective scalar multiplier.
 * @param output_tile Writable destination tile owned by the current task.
 * @param input_tiles Two immutable normalized input tiles.
 * @return Nothing.
 * @throws std::bad_alloc if parameter conversion or OpenCV allocation fails.
 * @throws GraphError for missing or mismatched inputs.
 * @throws cv::Exception if OpenCV multiplication or adapter conversion fails.
 * @note The operation has no shared mutable state and is reentrant across
 *       independent output tiles.
 */
static void op_multiply_tiled(const Node& node, const OutputTile& output_tile,
                              const std::vector<InputTile>& input_tiles) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_mixing:multiply");

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

/**
 * @brief Normalizes one image to a base image's extent and channel count.
 *
 * @param current_mat Mutable image replaced by normalized provider storage.
 * @param base_mat Immutable target extent, depth, and channel reference.
 * @param strategy `resize` or crop/zero-pad `crop` extent policy.
 * @return Nothing.
 * @throws GraphError with `GraphErrc::InvalidParameter` for an unsupported
 *         strategy or channel conversion.
 * @throws cv::Exception internally for resize, copy, merge, or color
 *         conversion failure; the provider fence translates it.
 * @throws std::bad_alloc for temporary channel or matrix ownership allocation.
 * @note Both matrices are callback-local headers; the helper publishes no
 *       shared provider state.
 */
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

/**
 * @brief Blends two full images with optional per-channel value mappings.
 *
 * @param node Operation node providing weights, mapping, and alpha strategy.
 * @param inputs Two resolved full-image inputs.
 * @return Blended NodeOutput with an owned image buffer.
 * @throws std::bad_alloc if parameter copying, channel storage, or result
 * allocation exhausts memory.
 * @throws GraphError for missing/empty/mismatched inputs or invalid strategy
 * data.
 * @throws cv::Exception for OpenCV blending/channel failures.
 * @note Invalid individual channel-map entries are skipped, while memory
 *       exhaustion remains exceptional for the public Host compute boundary.
 *       All mutable matrices and result storage are callback-local, so
 *       independent invocations are reentrant.
 */
static NodeOutput op_add_weighted_monolithic(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_mixing:add_weighted");
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
  const plugin::ParameterValue::Object* channel_mapping =
      parameter_object(P, "channel_mapping");
  const bool has_mapping = channel_mapping != nullptr;

  cv::Mat output(input_a.rows, input_a.cols,
                 CV_MAKETYPE(CV_32F, input_a.channels()));
  if (!has_mapping) {
    cv::addWeighted(input_a, alpha, input_b, beta, gamma, output);
  } else {
    int in_ch = input_a.channels();
    int out_ch = in_ch;
    const plugin::ParameterValue::Object* ch_input0 =
        nested_parameter_object(channel_mapping, "input0");
    const plugin::ParameterValue::Object* ch_input1 =
        nested_parameter_object(channel_mapping, "input1");
    int max0 = max_destination_channel(ch_input0);
    int max1 = max_destination_channel(ch_input1);
    int maxd = std::max({max0, max1, in_ch - 1});
    out_ch = std::max(out_ch, maxd + 1);

    std::vector<cv::Mat> A, B;
    cv::split(input_a, A);
    cv::split(input_b, B);
    if (static_cast<int>(A.size()) < out_ch)
      A.resize(out_ch, cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1));
    if (static_cast<int>(B.size()) < out_ch)
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
    mark_mapped_channels(ch_input0, covered);
    mark_mapped_channels(ch_input1, covered);
    for (int d = 0; d < out_ch; ++d)
      if (covered[d])
        O[d] = (gamma != 0.0)
                   ? cv::Mat(input_a.rows, input_a.cols, CV_32FC1,
                             cv::Scalar(gamma))
                   : cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
    apply_channel_mapping(ch_input0, A, alpha, O);
    apply_channel_mapping(ch_input1, B, beta, O);
    std::string alpha_strategy = as_str(P, "alpha_strategy", "weighted");
    if ((alpha_strategy != "weighted") && out_ch >= 4) {
      int aidx = 3;
      if (alpha_strategy == "max") {
        O[aidx] = cv::max(A.size() > 3 ? A[3] : O[aidx],
                          B.size() > 3 ? B[3] : O[aidx]);
      } else if (alpha_strategy == "copy0") {
        if (static_cast<int>(A.size()) > 3)
          O[aidx] = A[3].clone();
      } else if (alpha_strategy == "copy1") {
        if (static_cast<int>(B.size()) > 3)
          O[aidx] = B[3].clone();
      } else if (alpha_strategy == "set1") {
        O[aidx] =
            cv::Mat(input_a.rows, input_a.cols, CV_32FC1, cv::Scalar(1.0));
      } else if (alpha_strategy == "set0") {
        O[aidx] = cv::Mat::zeros(input_a.rows, input_a.cols, CV_32FC1);
      }
    }
    cv::merge(O, output);
  }

  NodeOutput out;
  out.image_buffer = fromCvMat(output);
  return out;
}

/**
 * @brief Computes full-image absolute difference with alpha policy.
 * @param node Effective merge and alpha-channel strategies.
 * @param inputs Two required full-image inputs.
 * @return Owned absolute-difference image.
 * @throws std::bad_alloc if parameter, channel, or output allocation fails.
 * @throws GraphError for missing inputs or unsupported normalization.
 * @throws cv::Exception if OpenCV normalization or arithmetic fails.
 * @note All mutable matrices are callback-local; independent invocations are
 *       safe to run concurrently.
 */
static NodeOutput op_abs_diff_monolithic(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_mixing:diff");
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

/**
 * @brief Multiplies two full CPU images after configured normalization.
 * @param node Effective merge strategy and scale.
 * @param inputs Two required full-image inputs.
 * @return Owned multiplied image.
 * @throws std::bad_alloc if parameter or output allocation fails.
 * @throws GraphError for missing inputs or unsupported normalization.
 * @throws cv::Exception if OpenCV normalization or multiplication fails.
 * @note Callback-local matrices provide reentrant execution across independent
 *       execution work.
 */
static NodeOutput op_multiply_monolithic(
    const Node& node, const std::vector<const NodeOutput*>& inputs) {
  PHOTOSPIDER_OBSERVE_OPENCV_OPERATION("image_mixing:multiply");
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

/** @copydoc ps::providers::opencv::register_provider */
void register_provider() {
  initialize_opencv_process_policy();
  OpRegistry& registry = OpRegistry::instance();

  registry.register_op_hp_monolithic(
      "image_source", "path",
      fence_monolithic_operation("image_source:path",
                                 MonolithicOpFunc(op_image_source_path)));
  registry.register_op_hp_monolithic(
      "image_generator", "constant",
      fence_monolithic_operation("image_generator:constant",
                                 MonolithicOpFunc(op_constant_image)));
  registry.register_op_hp_monolithic(
      "image_generator", "perlin_noise",
      fence_monolithic_operation("image_generator:perlin_noise",
                                 MonolithicOpFunc(op_perlin_noise)));
  registry.register_op_hp_monolithic(
      "image_process", "convolve",
      fence_monolithic_operation("image_process:convolve",
                                 MonolithicOpFunc(op_convolve)));
  registry.register_op_hp_monolithic(
      "image_process", "resize",
      fence_monolithic_operation("image_process:resize",
                                 MonolithicOpFunc(op_resize)));
  registry.register_op_hp_monolithic(
      "image_process", "crop",
      fence_monolithic_operation("image_process:crop",
                                 MonolithicOpFunc(op_crop)));
  registry.register_op_hp_monolithic(
      "image_process", "extract_channel",
      fence_monolithic_operation("image_process:extract_channel",
                                 MonolithicOpFunc(op_extract_channel)));

  OpMetadata tiled_meta;
  tiled_meta.tile_preference = TileSizePreference::MACRO;
  OpMetadata rt_meta;
  rt_meta.tile_preference = TileSizePreference::MICRO;

  const DirtyRoiPropFunc identity_roi(identity_dirty_roi);
  const ForwardRoiPropFunc identity_forward(identity_forward_roi);

  registry.register_dirty_propagator("image_source", "path", identity_roi);
  registry.register_dirty_propagator("image_generator", "constant",
                                     identity_roi);
  registry.register_dirty_propagator("image_generator", "perlin_noise",
                                     identity_roi);
  registry.register_forward_propagator("image_source", "path",
                                       identity_forward);
  registry.register_forward_propagator("image_generator", "constant",
                                       identity_forward);
  registry.register_forward_propagator("image_generator", "perlin_noise",
                                       identity_forward);
  registry.register_dirty_propagator("image_process", "resize",
                                     DirtyRoiPropFunc(resize_dirty_roi));
  registry.register_dirty_propagator("image_process", "crop",
                                     DirtyRoiPropFunc(crop_dirty_roi));
  registry.register_dirty_propagator("image_process", "extract_channel",
                                     identity_roi);
  registry.register_dirty_propagator("image_process", "curve_transform",
                                     identity_roi);
  registry.register_dirty_propagator("image_mixing", "add_weighted",
                                     identity_roi);
  registry.register_dirty_propagator("image_mixing", "diff", identity_roi);
  registry.register_dirty_propagator("image_mixing", "multiply", identity_roi);
  registry.register_forward_propagator("image_process", "resize",
                                       ForwardRoiPropFunc(resize_forward_roi));
  registry.register_forward_propagator("image_process", "crop",
                                       ForwardRoiPropFunc(crop_forward_roi));
  registry.register_forward_propagator("image_process", "extract_channel",
                                       identity_forward);
  registry.register_forward_propagator("image_process", "curve_transform",
                                       identity_forward);
  registry.register_forward_propagator("image_mixing", "add_weighted",
                                       identity_forward);
  registry.register_forward_propagator("image_mixing", "diff",
                                       identity_forward);
  registry.register_forward_propagator("image_mixing", "multiply",
                                       identity_forward);

  registry.register_dirty_propagator("image_process", "convolve",
                                     DirtyRoiPropFunc(convolve_dirty_roi));
  registry.register_forward_propagator(
      "image_process", "convolve", ForwardRoiPropFunc(convolve_forward_roi));

  registry.register_op_hp_monolithic(
      "image_process", "gaussian_blur",
      fence_monolithic_operation(
          "image_process:gaussian_blur",
          MonolithicOpFunc(op_gaussian_blur_monolithic)));
  const TileOpFunc gaussian_blur = fence_tiled_operation(
      "image_process:gaussian_blur", TileOpFunc(op_gaussian_blur_tiled));
  registry.register_op_hp_tiled("image_process", "gaussian_blur", gaussian_blur,
                                tiled_meta);
  registry.register_op_rt_tiled("image_process", "gaussian_blur", gaussian_blur,
                                rt_meta);
  registry.register_dirty_propagator("image_process", "gaussian_blur",
                                     DirtyRoiPropFunc(gaussian_blur_dirty_roi));
  registry.register_forward_propagator(
      "image_process", "gaussian_blur",
      ForwardRoiPropFunc(gaussian_blur_forward_roi));

  registry.register_op_hp_tiled(
      "image_process", "curve_transform",
      fence_tiled_operation("image_process:curve_transform",
                            TileOpFunc(op_curve_transform_tiled)),
      tiled_meta);

  registry.register_op_hp_monolithic(
      "image_mixing", "add_weighted",
      fence_monolithic_operation("image_mixing:add_weighted",
                                 MonolithicOpFunc(op_add_weighted_monolithic)));
  const TileOpFunc add_weighted = fence_tiled_operation(
      "image_mixing:add_weighted", TileOpFunc(op_add_weighted_tiled));
  registry.register_op_hp_tiled("image_mixing", "add_weighted", add_weighted,
                                tiled_meta);
  registry.register_op_rt_tiled("image_mixing", "add_weighted", add_weighted,
                                rt_meta);

  registry.register_op_hp_monolithic(
      "image_mixing", "diff",
      fence_monolithic_operation("image_mixing:diff",
                                 MonolithicOpFunc(op_abs_diff_monolithic)));
  registry.register_op_hp_tiled(
      "image_mixing", "diff",
      fence_tiled_operation("image_mixing:diff", TileOpFunc(op_abs_diff_tiled)),
      tiled_meta);

  registry.register_op_hp_monolithic(
      "image_mixing", "multiply",
      fence_monolithic_operation("image_mixing:multiply",
                                 MonolithicOpFunc(op_multiply_monolithic)));
  registry.register_op_hp_tiled(
      "image_mixing", "multiply",
      fence_tiled_operation("image_mixing:multiply",
                            TileOpFunc(op_multiply_tiled)),
      tiled_meta);
}

#undef PHOTOSPIDER_OBSERVE_OPENCV_OPERATION

}  // namespace ps::providers::opencv
