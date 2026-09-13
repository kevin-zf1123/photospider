/**
 * @file local_inpaint_navier_stokes_opencv.cpp
 * @brief PNT-05A OpenCV adapter over the locally provisioned 4.12.0 build.
 *
 * This adapter calls the pinned library directly: it packs one Float32 plane,
 * zeroes the hole samples, builds the internal 0/255 mask and calls
 * `cv::inpaint(..., INPAINT_NS)` once per channel in R, G, B order. The
 * library's own allocations are neither host-reserved nor interruptible, so the
 * variant declares that limitation instead of claiming the native frontier
 * cancellation bound or a hard host budget: cancellation is observed before and
 * after every channel call only. `estimated_external_bytes` documents the
 * untracked footprint of one pinned call; host-owned packing stays accounted
 * through the invocation allocator.
 */

#ifndef PHOTOSPIDER_HAS_OPENCV_INPAINT
#error \
    "local_inpaint_navier_stokes_opencv.cpp requires the pinned OpenCV adapter build"
#endif

#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <opencv2/core.hpp>
#include <opencv2/photo.hpp>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/basic_execution.hpp"
#include "09-composite/local_inpaint_navier_stokes_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace local_inpaint_ns;  // NOLINT(build/namespaces)

/** @brief Padded guard-grid state of one pinned channel call: f, band, mask
 * (one byte each), and the Float32 arrival grid (four bytes). */
constexpr std::uint64_t padded_state_bytes = 7;
/** @brief Pinned heap vector bound: at most one 16-byte entry per padded
 * sample at two-times geometric growth capacity. */
constexpr std::uint64_t padded_heap_bytes = 32;

/**
 * @brief Conservative untracked footprint of one pinned library call, bytes.
 * @note Estimates the guard-grid state and the inserting-order heap vector the
 * pinned implementation allocates internally. These bytes are outside the
 * invocation allocator and therefore outside the host execution budget; the
 * value is reported, never reserved.
 */
inline std::uint64_t estimated_external_bytes(const Planar& planar) {
  return planar.padded * (padded_state_bytes + padded_heap_bytes);
}

/** @brief Reports one typed profile failure with a dynamic diagnostic. */
[[noreturn]] void fail(ErrorCode code, const std::string& message) {
  throw Failure{Status::failure(code, message)};
}

/** @brief Adapter solver: one pinned channel call per R, G and B plane. */
void solve(const OperationInvocation& call, const Planar& planar,
           std::int64_t range, MutableValue* output) {
  // Representability guard only; the estimate is never charged to the host.
  require(estimated_external_bytes(planar) <=
              std::numeric_limits<std::uint64_t>::max() / 2,
          ErrorCode::ResourceExhausted,
          "pinned inpaint footprint is not representable");
  poll(call);
  auto mask_buffer = take(call.allocator.allocate(planar.pixels));
  poll(call);
  auto source_buffer =
      take(call.allocator.allocate(planar.pixels * sizeof(float)));
  poll(call);
  auto solved_buffer =
      take(call.allocator.allocate(planar.pixels * sizeof(float)));
  poll(call);
  auto* binary = mask_buffer.data();
  auto* source = reinterpret_cast<float*>(source_buffer.data());
  auto* solved = reinterpret_cast<float*>(solved_buffer.data());
  build_binary_mask(call, planar, binary);
  const std::size_t step =
      static_cast<std::size_t>(planar.width) * sizeof(float);
  const cv::Mat mask(static_cast<int>(planar.height),
                     static_cast<int>(planar.width), CV_8UC1, binary,
                     static_cast<std::size_t>(planar.width));
  for (std::uint64_t channel = 0; channel < 3; ++channel) {
    poll(call);
    pack_plane(call, planar, channel, binary, source);
    // Observe cancellation after the last packing chunk as well, so a stop
    // requested during packing never enters the non-interruptible channel.
    poll(call);
    const cv::Mat input(static_cast<int>(planar.height),
                        static_cast<int>(planar.width), CV_32FC1, source, step);
    cv::Mat destination(static_cast<int>(planar.height),
                        static_cast<int>(planar.width), CV_32FC1, solved, step);
    clear_arithmetic_exceptions();
    try {
      cv::inpaint(input, mask, destination, static_cast<double>(range),
                  cv::INPAINT_NS);
    } catch (const cv::Exception& error) {
      // A refused allocation is a resource limit, not a computation failure.
      fail(error.code == cv::Error::StsNoMem ? ErrorCode::ResourceExhausted
                                             : ErrorCode::OperationFailed,
           std::string("pinned OpenCV inpaint failed: ") + error.what());
    } catch (const std::bad_alloc&) {
      fail(ErrorCode::ResourceExhausted,
           "pinned OpenCV inpaint exhausted untracked host memory");
    }
    // Observe cancellation before reporting a dirty arithmetic channel: a
    // direct registry invocation returns this callback status unchanged, so a
    // cancelled channel must not surface as OperationFailed.
    poll(call);
    require_clean_arithmetic();
    require_finite_holes(call, planar, binary, solved);
    write_holes(call, planar, call.output_region, solved, binary, channel,
                output);
  }
}
}  // namespace
Status register_image_local_inpaint_navier_stokes_opencv(
    OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "image.local_inpaint_navier_stokes_openCV";
  auto& traits = operation.traits;
  traits.input_count = 2;
  traits.input_schema = {image_port(), mask_port()};
  traits.parameter_schema = {radius_parameter()};
  // Host-owned packing only: one binary mask (N), one packed source plane
  // (4N) and one solved plane (4N). The pinned library's internal allocations
  // are reported by `estimated_external_bytes` and are not host-accounted.
  traits.workspace_bytes = 0;
  traits.workspace_input_multiplier = 1;
  traits.outputs[0].key = "image";
  traits.outputs[0].output_element_type = ElementType::Float32;
  traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.outputs[0].region_rule = OperationRegionRule::Whole;
  traits.outputs[0].output_semantic_rule = OperationSemanticRule::PreserveInput;
  traits.outputs[0].output_schema = traits.input_schema.front();
  operation.callback = [](const OperationInvocation& call) {
    if (call.backend == Backend::Gpu)
      return Result<Value>(Status::failure(
          ErrorCode::BackendUnavailable,
          "image.local_inpaint_navier_stokes_openCV is CPU only"));
    return run_profile(call, solve);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
