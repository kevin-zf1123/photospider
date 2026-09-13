#pragma once

/**
 * @file local_inpaint_navier_stokes_common.hpp
 * @brief Shared PNT-05A profile helpers for both required variants.
 *
 * The frozen numeric profile is `opencv_4_12_ns_f32_planar_v1`: OpenCV 4.12.0
 * `INPAINT_NS` applied to three single-channel Float32 planes in R, G, B order.
 * This file owns everything both variants must share: the closed radius
 * schema, the canonical scene-or-display image profile, complete
 * logical-domain validation of the image and hole mask, hole counting, 0/255
 * mask materialization, per-channel plane packing, bitwise output copy-back,
 * cancellation cadence in logical samples and publication. The discrete solver
 * is deliberately not shared: the OpenCV adapter calls the pinned library and
 * the native Apple Silicon variant runs the licensed standalone port.
 *
 * Pinned source: OpenCV tag 4.12.0, `modules/photo/src/inpaint.cpp`, blob
 * 2f2f368fa13da0bc1426b71862205048c6ea0f94, under the Intel License Agreement
 * reproduced in `local_inpaint_navier_stokes_native.cpp`.
 */

#include <cfenv>  // NOLINT(build/c++11)
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/basic_execution.hpp"
#include "photospider/photospider.hpp"

namespace ps::plugin_internal::local_inpaint_ns {

using basic_internal::Failure;
using basic_internal::poll;
using basic_internal::require;
using basic_internal::take;

/** @brief Inclusive radius range of the frozen profile. */
constexpr std::int64_t radius_minimum = 1;
constexpr std::int64_t radius_maximum = 32;
/** @brief Inclusive logical extent range of the frozen profile. */
constexpr std::uint64_t extent_minimum = 3;
constexpr std::uint64_t extent_maximum = 32768;
/** @brief Largest admitted logical pixel count. */
constexpr std::uint64_t pixel_limit = (UINT64_C(1) << 31) - 1;
/**
 * @brief Logical samples between two cancellation observations.
 * @note Validation, copy, packing and initialization count every RGBA and mask
 * read, so the observation gap is bounded in samples rather than rows.
 */
constexpr std::uint64_t sample_poll_samples = 4096;
/** @brief Pinned arrival-time initialization of the guard grid. */
constexpr float initial_arrival_time = 1.0e6F;
/**
 * @brief Arithmetic exception flags rejected as nonfinite intermediates.
 * @note A finite published hole sample can hide an overflowing intermediate
 * such as `VectorLength(gradI)`. Raising any of these flags fails the channel.
 */
constexpr int rejected_exceptions = FE_INVALID | FE_OVERFLOW | FE_DIVBYZERO;

/** @brief Required bounded Int64 radius shared by both variants. */
inline OperationParameterSpec radius_parameter() {
  return {"radius",
          OperationParameterType::Int64,
          true,
          true,
          static_cast<double>(radius_minimum),
          static_cast<double>(radius_maximum)};
}

/** @brief Reads `radius` after host schema validation. */
inline std::int64_t radius(const OperationInvocation& call) {
  return std::get<std::int64_t>(call.parameters.at("radius"));
}

/**
 * @brief Canonical RGBA input/output port of the profile.
 * @note Typed Image semantics accept the canonical profile with either the
 * scene or the display reference; the callback validates the exact remaining
 * fields. A fixed RgbaFloat32 port cannot express that choice.
 */
inline OperationPortConstraint image_port() {
  OperationPortConstraint port;
  port.kind = OperationPortKind::Typed;
  port.semantic_kind = static_cast<std::uint32_t>(SemanticKind::Image);
  port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
  port.rank = 3;
  return port;
}

/** @brief Canonical typed HW coverage port of the profile. */
inline OperationPortConstraint mask_port() {
  return OperationPortConstraint{OperationPortKind::Float32Mask, 0, 0};
}

/** @brief Reads one Float32 sample through checked logical addressing. */
template <class Number>
inline Number read_sample(const Value& value,
                          const std::vector<std::uint64_t>& coordinate) {
  const auto address = take(value.byte_address(coordinate));
  Number number;
  std::memcpy(&number, value.bytes().data() + address, sizeof(number));
  return number;
}

/**
 * @brief Counts visited logical samples and polls within a fixed sample gap.
 * @note Construction observes cancellation once, at the helper's entry. It is
 * not an observation at the helper's exit, so a caller that resumes its own
 * counter across a nested helper must observe cancellation at that stage
 * boundary itself; otherwise the nested helper's last `sample_poll_samples`
 * samples can bridge into the caller's next interval. Every validation, copy,
 * packing and initialization loop uses this counter.
 * @note Construction is not `noexcept`: an already cancelled invocation must
 * propagate its typed failure instead of terminating.
 */
class SampleWatch final {
 public:
  /** @brief Observes cancellation at the helper boundary. */
  explicit SampleWatch(const OperationInvocation& call) : call_(&call) {
    poll(*call_);
  }

  /** @brief Notes one visited sample and observes cancellation when due. */
  void note() {
    ++seen_;
    if (seen_ >= sample_poll_samples) {
      seen_ = 0;
      poll(*call_);
    }
  }

 private:
  const OperationInvocation* call_;
  std::uint64_t seen_ = 0;
};

/**
 * @brief Checked planar geometry of one profile image and its hole mask.
 * @note Padded addressing matches the pinned (H+2)x(W+2) guard grid, and work
 * addressing matches the packed HxW single-channel plane.
 */
struct Planar final {
  std::uint64_t height = 0;
  std::uint64_t width = 0;
  std::uint64_t pixels = 0;
  std::uint64_t padded = 0;
  /** @brief Padded row length W+2. */
  std::uint64_t padded_row() const noexcept { return width + 2; }
  /** @brief Guard-grid index of padded coordinate (row, column). */
  std::uint64_t padded_index(std::uint64_t row,
                             std::uint64_t column) const noexcept {
    return row * (width + 2) + column;
  }
  /** @brief Packed index of logical coordinate (y, x). */
  std::uint64_t work_index(std::uint64_t y, std::uint64_t x) const noexcept {
    return y * width + x;
  }
};

/**
 * @brief Validates descriptors, the shared grid and checked byte products.
 * @param call Invocation whose port 0 is the image and port 1 the hole mask.
 * @return Checked planar geometry.
 * @note Rejects unsupported dtype/rank/channel profile, extents outside
 * three..32768 on either axis, a hole mask on a different logical grid, and
 * unrepresentable or over-limit extents before any allocation.
 */
inline Planar planar_geometry(const OperationInvocation& call) {
  const auto& image = call.inputs[0];
  const auto& mask = call.inputs[1];
  require(image.descriptor().element_type == ElementType::Float32 &&
              mask.descriptor().element_type == ElementType::Float32,
          ErrorCode::TypeMismatch, "inpaint profile requires Float32 inputs");
  const auto& shape = image.descriptor().shape;
  require(shape.size() == 3 && shape[2] == 4, ErrorCode::TypeMismatch,
          "inpaint image requires ordered HWC RGBA");
  require(mask.descriptor().shape.size() == 2 &&
              mask.descriptor().shape[0] == shape[0] &&
              mask.descriptor().shape[1] == shape[1],
          ErrorCode::TypeMismatch,
          "hole mask must share the image spatial grid");
  require(shape[0] >= extent_minimum && shape[1] >= extent_minimum,
          ErrorCode::TypeMismatch,
          "inpaint requires spatial extents of at least three");
  require(shape[0] <= extent_maximum && shape[1] <= extent_maximum,
          ErrorCode::ResourceExhausted,
          "inpaint spatial extent exceeds the profile bound");
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  require(shape[0] <= pixel_limit / shape[1], ErrorCode::ResourceExhausted,
          "inpaint pixel count is not representable");
  Planar planar;
  planar.height = shape[0];
  planar.width = shape[1];
  planar.pixels = planar.height * planar.width;
  require(planar.pixels <= pixel_limit, ErrorCode::ResourceExhausted,
          "inpaint pixel count exceeds the profile bound");
  require(planar.height + 2 <= maximum / (planar.width + 2),
          ErrorCode::ResourceExhausted,
          "inpaint padded extent is not representable");
  planar.padded = (planar.height + 2) * (planar.width + 2);
  require(planar.padded <= maximum / 16, ErrorCode::ResourceExhausted,
          "inpaint padded byte products are not representable");
  return planar;
}

/**
 * @brief Validates the exact PNT-05A image interpretation.
 * @param image Port-0 image Value.
 * @note Accepts the canonical linear sRGB D65 coverage-premultiplied RGBA
 * profile with a scene or display reference. Every other semantic field, an
 * additional facet, or a changed channel set fails with TypeMismatch.
 */
inline void require_image_profile(const Value& image) {
  require(image.facets().size() == 1, ErrorCode::TypeMismatch,
          "inpaint image requires exactly one semantic facet");
  auto semantic = take(decode_semantic(image.facets().front()));
  const auto canonical = rgba_semantics();
  require(semantic.kind == SemanticKind::Image &&
              semantic.model == canonical.model &&
              semantic.primaries == canonical.primaries &&
              semantic.white == canonical.white &&
              semantic.transfer == canonical.transfer &&
              semantic.unit == canonical.unit &&
              semantic.association == canonical.association &&
              semantic.coordinate_space == canonical.coordinate_space &&
              semantic.direction == canonical.direction &&
              semantic.sample_origin == canonical.sample_origin &&
              semantic.sample_step == canonical.sample_step &&
              semantic.sample_axis_unit == canonical.sample_axis_unit &&
              semantic.media_type == canonical.media_type &&
              semantic.plane_origin == canonical.plane_origin &&
              semantic.plane_step == canonical.plane_step,
          ErrorCode::TypeMismatch,
          "inpaint image requires the canonical linear RGBA profile");
  require(semantic.reference == "scene" || semantic.reference == "display",
          ErrorCode::TypeMismatch,
          "inpaint image requires a scene or display reference");
  require(semantic.channels.size() == canonical.channels.size(),
          ErrorCode::TypeMismatch, "inpaint image requires four channels");
  for (std::size_t index = 0; index < canonical.channels.size(); ++index)
    require(semantic.channels[index].role == canonical.channels[index].role &&
                semantic.channels[index].unit == canonical.channels[index].unit,
            ErrorCode::TypeMismatch, "inpaint image channel roles changed");
}

/**
 * @brief Validates the complete logical domain and counts hole pixels.
 * @param call Invocation with a validated image and matching hole mask.
 * @param planar Checked planar geometry.
 * @return K, the number of hole samples.
 * @note Requires every RGBA sample finite, alpha exactly one and every mask
 * sample exactly zero or one; both signed zeros are known samples. RGB
 * placeholders inside holes are validated and never used as color. Exactly five
 * logical samples are observed per pixel.
 */
inline std::uint64_t validate_domain(const OperationInvocation& call,
                                     const Planar& planar) {
  const auto& image = call.inputs[0];
  const auto& mask = call.inputs[1];
  SampleWatch watch(call);
  std::vector<std::uint64_t> sample{0, 0, 0};
  std::vector<std::uint64_t> pixel{0, 0};
  std::uint64_t holes = 0;
  for (std::uint64_t y = 0; y < planar.height; ++y) {
    sample[0] = y;
    pixel[0] = y;
    for (std::uint64_t x = 0; x < planar.width; ++x) {
      sample[1] = x;
      pixel[1] = x;
      float alpha = 0;
      for (std::uint64_t channel = 0; channel < 4; ++channel) {
        sample[2] = channel;
        const float value = read_sample<float>(image, sample);
        watch.note();
        require(std::isfinite(value), ErrorCode::OperationFailed,
                "inpaint requires finite RGBA samples");
        if (channel == 3)
          alpha = value;
      }
      require(alpha == 1.0F, ErrorCode::OperationFailed,
              "inpaint requires alpha exactly one");
      const float hole = read_sample<float>(mask, pixel);
      watch.note();
      require(hole == 0.0F || hole == 1.0F, ErrorCode::OperationFailed,
              "hole mask sample must be exactly zero or one");
      holes += hole == 1.0F ? 1 : 0;
    }
  }
  return holes;
}

/**
 * @brief Materializes the internal 0/255 hole mask.
 * @note One logical sample is observed per pixel.
 */
inline void build_binary_mask(const OperationInvocation& call,
                              const Planar& planar, std::uint8_t* out) {
  const auto& mask = call.inputs[1];
  SampleWatch watch(call);
  std::vector<std::uint64_t> pixel{0, 0};
  for (std::uint64_t y = 0; y < planar.height; ++y) {
    pixel[0] = y;
    for (std::uint64_t x = 0; x < planar.width; ++x) {
      pixel[1] = x;
      const float sample = read_sample<float>(mask, pixel);
      watch.note();
      out[planar.work_index(y, x)] = sample == 0.0F ? 0 : 255;
    }
  }
}

/**
 * @brief Packs one packed Float32 work plane from the image channel.
 * @param call Invocation with a validated image.
 * @param planar Checked planar geometry.
 * @param channel Selected R, G or B channel index.
 * @param binary Internal 0/255 hole mask of the same grid.
 * @param plane Packed Float32 destination of `planar.pixels` samples.
 * @note Every hole sample becomes positive zero, so no hole placeholder can
 * reach the solver; known samples keep their exact bits. One logical sample is
 * observed per pixel.
 */
inline void pack_plane(const OperationInvocation& call, const Planar& planar,
                       std::uint64_t channel, const std::uint8_t* binary,
                       float* plane) {
  const auto& image = call.inputs[0];
  SampleWatch watch(call);
  std::vector<std::uint64_t> sample{0, 0, channel};
  std::uint64_t index = 0;
  for (std::uint64_t y = 0; y < planar.height; ++y) {
    sample[0] = y;
    for (std::uint64_t x = 0; x < planar.width; ++x) {
      sample[1] = x;
      if (binary[planar.work_index(y, x)] == 0) {
        plane[index] = read_sample<float>(image, sample);
      } else {
        plane[index] = 0.0F;
      }
      watch.note();
      ++index;
    }
  }
}

/**
 * @brief Copies the complete output Region from input 0 without conversion.
 * @note Byte preservation keeps unmasked samples, negative zeros and alpha
 * exactly as published by the caller. Four logical samples are observed per
 * pixel.
 */
inline void copy_region(const OperationInvocation& call, const Region& region,
                        MutableValue* output) {
  const auto& image = call.inputs[0];
  const auto& dimensions = region.dimensions();
  SampleWatch watch(call);
  std::vector<std::uint64_t> sample{0, 0, 0};
  std::size_t target = 0;
  for (std::uint64_t y = dimensions[0].offset;
       y < dimensions[0].offset + dimensions[0].extent; ++y) {
    sample[0] = y;
    for (std::uint64_t x = dimensions[1].offset;
         x < dimensions[1].offset + dimensions[1].extent; ++x) {
      sample[1] = x;
      for (std::uint64_t channel = 0; channel < 4; ++channel) {
        sample[2] = channel;
        const float number = read_sample<float>(image, sample);
        watch.note();
        std::memcpy(output->data() + target, &number, sizeof(number));
        target += sizeof(number);
      }
    }
  }
  require(target == output->size(), ErrorCode::OperationFailed,
          "inpaint output Region is not a packed plane");
}

/**
 * @brief Rejects a nonfinite solved hole sample anywhere on the logical grid.
 * @param plane Solved packed work plane of one channel.
 * @param binary Internal 0/255 hole mask of the same grid.
 * @note The complete domain is checked, so a nonfinite result fails even when
 * the requested output Region would not publish that sample. One logical sample
 * is observed per visited pixel.
 */
inline void require_finite_holes(const OperationInvocation& call,
                                 const Planar& planar,
                                 const std::uint8_t* binary,
                                 const float* plane) {
  SampleWatch watch(call);
  for (std::uint64_t y = 0; y < planar.height; ++y)
    for (std::uint64_t x = 0; x < planar.width; ++x) {
      const std::uint64_t index = planar.work_index(y, x);
      watch.note();
      if (binary[index] == 0)
        continue;
      require(std::isfinite(plane[index]), ErrorCode::OperationFailed,
              "inpaint produced a nonfinite hole sample");
    }
}

/**
 * @brief Replaces every hole sample of one channel inside the output Region.
 * @param plane Solved packed work plane, reused between channels.
 * @param binary Internal 0/255 hole mask of the same grid.
 * @param channel Selected R, G or B channel index.
 * @note Unreached hole samples are written from the work plane as well, so an
 * exhausted frontier publishes the pinned zero work value instead of a
 * placeholder. Four logical samples are observed per pixel.
 */
inline void write_holes(const OperationInvocation& call, const Planar& planar,
                        const Region& region, const float* plane,
                        const std::uint8_t* binary, std::uint64_t channel,
                        MutableValue* output) {
  const auto& dimensions = region.dimensions();
  SampleWatch watch(call);
  std::size_t target = 0;
  for (std::uint64_t y = dimensions[0].offset;
       y < dimensions[0].offset + dimensions[0].extent; ++y)
    for (std::uint64_t x = dimensions[1].offset;
         x < dimensions[1].offset + dimensions[1].extent; ++x) {
      const std::uint64_t index = planar.work_index(y, x);
      const bool hole = binary[index] != 0;
      for (std::uint64_t c = 0; c < 4; ++c) {
        watch.note();
        if (hole && c == channel)
          std::memcpy(output->data() + target, plane + index, sizeof(float));
        target += sizeof(float);
      }
    }
}

/**
 * @brief Clears the rejected binary32 exception flags before a solve.
 * @note The caller environment is restored by the image-scope or callback
 * `Float32Environment`, so clearing here cannot leak to the caller.
 */
inline void clear_arithmetic_exceptions() noexcept {
  std::feclearexcept(rejected_exceptions);
}

/**
 * @brief Rejects a channel whose arithmetic raised a nonfinite exception.
 * @note A finite published sample can still hide an overflowing intermediate;
 * this check fails the channel instead of publishing it.
 */
inline void require_clean_arithmetic() {
  require(std::fetestexcept(rejected_exceptions) == 0,
          ErrorCode::OperationFailed,
          "inpaint produced nonfinite intermediate arithmetic");
}

/**
 * @brief Runs one shared-profile callback with typed failure fencing.
 * @param call Synchronous invocation.
 * @param solve Variant solver invoked only for a nonempty, non-full mask.
 * @return Published named `image` result or the first typed failure.
 * @note The complete domain is validated before the all-zero identity shortcut,
 * a full mask never produces a result, and every failure releases unpublished
 * output and scratch.
 */
template <class Solver>
inline Result<Value> run_profile(const OperationInvocation& call,
                                 Solver solve) {
  try {
    input_internal::Float32Environment environment;
    require(environment.active(), ErrorCode::OperationFailed,
            "inpaint requires the binary32 floating environment");
    poll(call);
    const Planar layout = planar_geometry(call);
    require_image_profile(call.inputs[0]);
    const std::int64_t range = radius(call);
    const std::uint64_t holes = validate_domain(call, layout);
    require(holes < layout.pixels, ErrorCode::OperationFailed,
            "inpaint requires at least one known sample");
    poll(call);
    auto made = take(MutableValue::allocate(
        call.inputs[0].descriptor(), call.output_region, call.allocator));
    MutableValue output = std::move(made);
    // Observe cancellation on both sides of the output allocation and at the
    // start of the copying stage.
    poll(call);
    copy_region(call, call.output_region, &output);
    if (holes != 0) {
      poll(call);
      solve(call, layout, range, &output);
    }
    poll(call);
    return std::move(output).publish(call.inputs[0].facets());
  } catch (const Failure& failure) {
    return Result<Value>(failure.status);
  }
}

}  // namespace ps::plugin_internal::local_inpaint_ns
