#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/ops/numeric/color_description.hpp"
#include "photospider/ops/numeric/curves.hpp"

namespace ps::numeric {
/** @brief Common color-ramp authoring choices; no dynamic sample data. */
struct ColorRampOptions {
  /** @brief Defaults to the explicitly supplied colors dtype hint. */
  std::optional<ElementType> dtype;
  /** @brief Clamp or Reject; color ramps do not extrapolate. */
  CurveDomain out_of_domain = CurveDomain::Clamp;
  CpuNumericProfile profile = CpuNumericProfile::Strict;
};
/** @brief Original unwrapped hue; no shortest-path or normalization mode. */
struct HueRampOptions : ColorRampOptions {
  /** @brief Radian/PiMultiple; defaults to input unit, RationalPi to
   * PiMultiple. */
  std::optional<HueUnit> output_hue_unit;
};
/** @brief Straight sRGB/D65 tuple, optionally with an independent alpha.
 * Pure and thread-safe; no payload conversion. Allocation may throw bad_alloc.
 */
inline TensorDescription color_ramp_rgb_description(bool with_alpha = false) {
  TensorInterpretation interpretation;
  interpretation.model = "rgb";
  interpretation.reference = "display";
  interpretation.association = "straight";
  interpretation.primaries = "srgb_rec709";
  interpretation.transfer = "srgb";
  interpretation.coordinates = TensorModelCoordinates{};
  interpretation.coordinates->scale = "relative";
  return color_tuple_description(std::move(interpretation), {"1", "1", "1"},
                                 with_alpha)
      .take_value();
}
/** @brief CMYK tuple bound to a printing-profile identity.
 * Import and bind its frozen bytes separately. Compilation checks the profile.
 * Returns a checked description; pure and thread-safe. Allocation may throw.
 */
inline Result<TensorDescription> color_ramp_cmyk_description(
    ResourceIdentity identity) {
  TensorInterpretation interpretation;
  interpretation.model = "cmyk";
  interpretation.reference = "profile";
  interpretation.convention = "icc-native";
  interpretation.association = "straight";
  interpretation.profile = identity;
  return color_tuple_description(std::move(interpretation),
                                 {"1", "1", "1", "1"});
}
namespace color_ramp_detail {
inline Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline TensorDescription description(const std::string& model,
                                     HueUnit unit = HueUnit::Radian) {
  TensorInterpretation interpretation;
  interpretation.model = model;
  interpretation.reference = "display";
  interpretation.association = "straight";
  interpretation.white = model == "cielab" || model == "cielch"
                             ? std::array<double, 2>{.3457, .3585}
                             : std::array<double, 2>{.3127, .3290};
  interpretation.coordinates = TensorModelCoordinates{};
  interpretation.coordinates->scale = "relative";
  std::vector<std::string> units{"1", "1", "1"};
  if (model == "cielab")
    units = {"1", "a*", "b*"};
  if (model == "cielch" || model == "oklch")
    units = {"1", model == "cielch" ? "C*" : "1", hue_unit_name(unit)};
  if (model == "hsl") {
    interpretation.primaries = "srgb_rec709";
    interpretation.transfer = "srgb";
    units[0] = hue_unit_name(unit);
  }
  return color_tuple_description(std::move(interpretation), units).take_value();
}
inline Result<WorkflowNode> node(std::uint64_t id, const char* name,
                                 const std::string& model,
                                 std::vector<WorkflowInput> inputs,
                                 ElementType colors_type,
                                 const TensorDescription& description,
                                 const ColorRampOptions& options,
                                 std::optional<HueUnit> source_hue = {},
                                 std::optional<HueUnit> output_hue = {}) {
  using Answer = Result<WorkflowNode>;
  const auto dtype = options.dtype.value_or(colors_type);
  const auto* suffix = options.profile == CpuNumericProfile::Strict ? "_strict"
                       : options.profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : options.profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !suffix ||
      (colors_type != ElementType::Float32 &&
       colors_type != ElementType::Float64) ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64) ||
      (options.out_of_domain != CurveDomain::Clamp &&
       options.out_of_domain != CurveDomain::Reject))
    return Answer(invalid("invalid color-ramp id/dtype/domain/profile"));
  auto group = resolve_color_tuple(description);
  if (!group.ok())
    return Answer(group.status());
  if (group.value().interpretation.model != model ||
      (group.value().alpha && model != "rgb"))
    return Answer(invalid("color-ramp model/alpha mismatch"));
  if (source_hue && (!hue_unit_name(*source_hue) ||
                     group.value().components[model == "hsl" ? 0 : 2].unit !=
                         hue_unit_name(*source_hue)))
    return Answer(invalid("color-ramp source hue description mismatch"));
  if (output_hue && *output_hue != HueUnit::Radian &&
      *output_hue != HueUnit::PiMultiple)
    return Answer(invalid("color-ramp output hue requires a floating unit"));
  auto encoded = tensor_description_parameter(description);
  if (!encoded.ok())
    return Answer(encoded.status());
  WorkflowNode result{
      id,
      std::string("curve.color_ramp_") + name + suffix,
      std::move(inputs),
      {{"color_description", encoded.take_value()},
       {"dtype",
        std::string(dtype == ElementType::Float32 ? "float32" : "float64")},
       {"out_of_domain",
        std::string(options.out_of_domain == CurveDomain::Clamp ? "clamp"
                                                                : "reject")}}};
  if (output_hue)
    result.parameters["output_hue_unit"] =
        std::string(hue_unit_name(*output_hue));
  return Answer(std::move(result));
}
}  // namespace color_ramp_detail
/** @brief Shared constructor contract for the independent color-ramp
 * primitives. Ordered inputs are Result tensors `input`, `stops`, `colors`;
 * each has one tensor member under any schema id/version/key. Their complete
 * `sample_shape()` values are S, [K] and [K,C]. Floating ports independently
 * use Float32/64. S has rank 1..7, K=1..65536 and all logical products <=2^40.
 * Rational-pi constructors use split Float32/64 colors [K,2] and Int64[K]
 * numerator and denominator inputs; selected rows require positive
 * denominators. `colors_type` is only an authoring hint for default output
 * dtype; Compiler checks actual Result edges. Helpers create only graph nodes
 * and parameters and may be used concurrently; invalid options fail
 * InvalidArgument/InvalidDomain/Schema. Allocation may throw bad_alloc.
 * Descriptor/edge conflicts fail TypeMismatch at compilation. Each selected
 * output tile has one fixed CPU invocation, reading the matching query region
 * and all stops/colors/p/q tiles on each input's own grid. K=1 still reads
 * query. Output `values` uses photospider.tensor v1/member samples, descriptor
 * shape S+[C], atomic_trailing_axes=1, without color metadata or input batch
 * grouping. Empty emits no task. Only selected dense output tiles are
 * allocated.
 *
 * Finite strictly increasing stops, finite queries, selected complete-color
 * model/alpha validity, selected q>0, reject-domain membership and finite
 * representable output are unchecked caller preconditions. RGBA with rounded
 * zero alpha requires rounded zero RGB; valid transparent output is all +0.
 * Exact interpolation rounds once at destination. Direct/complete-identical
 * rows retain converted zero signs; genuine mixed exact zero is +0. RGB alone
 * decodes/encodes transfer with exact alpha. Polar hue remains unwrapped with
 * certified pi unit conversion, including at zero chroma or saturation.
 *
 * Each invocation borrows affine inputs and owns fixed arithmetic scratch.
 * Preparation is immutable and shareable across Runs. Cancellation/capacity
 * failures stop the invocation; incomplete certification is ResourceExhausted.
 * CMYK selects and validates frozen ICC resources once per preparation, also
 * for Empty and artifact import. Plans and kernels retain those resources;
 * output Results retain their own data and survive context teardown.
 */
/** @brief Linear-light RGB/RGBA; default description is three-channel sRGB. */
inline Result<WorkflowNode> color_ramp_rgb_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description = color_ramp_rgb_description(),
    ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "rgb", "rgb", {std::move(input), std::move(stops), std::move(colors)},
      colors_type, description, options);
}
/** @brief Exact component interpolation in the declared Cmyk coordinates. */
inline Result<WorkflowNode> color_ramp_cmyk_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description, ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cmyk", "cmyk",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Exact component interpolation in the declared Xyz coordinates. */
inline Result<WorkflowNode> color_ramp_xyz_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description = color_ramp_detail::description("xyz"),
    ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "xyz", "xyz", {std::move(input), std::move(stops), std::move(colors)},
      colors_type, description, options);
}
/** @brief Exact component interpolation in the declared Cielab coordinates. */
inline Result<WorkflowNode> color_ramp_cielab_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description = color_ramp_detail::description("cielab"),
    ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cielab", "cielab",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Exact component interpolation in the declared Oklab coordinates. */
inline Result<WorkflowNode> color_ramp_oklab_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description = color_ramp_detail::description("oklab"),
    ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "oklab", "oklab",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Exact component interpolation in the declared Ycbcr coordinates. */
inline Result<WorkflowNode> color_ramp_ycbcr_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description, ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "ycbcr", "ycbcr",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Original Radian Cielch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_cielch_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("cielch", HueUnit::Radian),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cielch", "cielch",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, HueUnit::Radian,
      options.output_hue_unit.value_or(HueUnit::Radian));
}
/** @brief Original PiMultiple Cielch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_cielch_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("cielch", HueUnit::PiMultiple),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cielch_pi", "cielch",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, HueUnit::PiMultiple,
      options.output_hue_unit.value_or(HueUnit::PiMultiple));
}
/** @brief Original RationalPi Cielch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_cielch_rational_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, WorkflowInput numerator, WorkflowInput denominator,
    ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("cielch", HueUnit::RationalPi),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cielch_rational_pi", "cielch",
      {std::move(input), std::move(stops), std::move(colors),
       std::move(numerator), std::move(denominator)},
      colors_type, description, options, HueUnit::RationalPi,
      options.output_hue_unit.value_or(HueUnit::PiMultiple));
}
/** @brief Original Radian Oklch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_oklch_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("oklch", HueUnit::Radian),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "oklch", "oklch",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, HueUnit::Radian,
      options.output_hue_unit.value_or(HueUnit::Radian));
}
/** @brief Original PiMultiple Oklch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_oklch_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("oklch", HueUnit::PiMultiple),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "oklch_pi", "oklch",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, HueUnit::PiMultiple,
      options.output_hue_unit.value_or(HueUnit::PiMultiple));
}
/** @brief Original RationalPi Oklch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_oklch_rational_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, WorkflowInput numerator, WorkflowInput denominator,
    ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("oklch", HueUnit::RationalPi),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "oklch_rational_pi", "oklch",
      {std::move(input), std::move(stops), std::move(colors),
       std::move(numerator), std::move(denominator)},
      colors_type, description, options, HueUnit::RationalPi,
      options.output_hue_unit.value_or(HueUnit::PiMultiple));
}
/** @brief Original Radian Hsl hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_hsl_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("hsl", HueUnit::Radian),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "hsl", "hsl", {std::move(input), std::move(stops), std::move(colors)},
      colors_type, description, options, HueUnit::Radian,
      options.output_hue_unit.value_or(HueUnit::Radian));
}
/** @brief Original PiMultiple Hsl hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_hsl_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("hsl", HueUnit::PiMultiple),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "hsl_pi", "hsl",
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, HueUnit::PiMultiple,
      options.output_hue_unit.value_or(HueUnit::PiMultiple));
}
/** @brief Original RationalPi Hsl hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_hsl_rational_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, WorkflowInput numerator, WorkflowInput denominator,
    ElementType colors_type,
    TensorDescription description =
        color_ramp_detail::description("hsl", HueUnit::RationalPi),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "hsl_rational_pi", "hsl",
      {std::move(input), std::move(stops), std::move(colors),
       std::move(numerator), std::move(denominator)},
      colors_type, description, options, HueUnit::RationalPi,
      options.output_hue_unit.value_or(HueUnit::PiMultiple));
}
}  // namespace ps::numeric
