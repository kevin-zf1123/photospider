#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "photospider/data/color_array.hpp"
#include "photospider/numeric/curves.hpp"

namespace ps::numeric {
/** @brief Common color-ramp authoring choices; no dynamic sample data. */
struct ColorRampOptions {
  /** @brief Defaults to the explicitly supplied colors dtype hint. */
  std::optional<ElementType> dtype;
  /** @brief Clamp or Reject; color ramps do not extrapolate. */
  CurveDomain out_of_domain = CurveDomain::Clamp;
  CpuNumericProfile profile = CpuNumericProfile::Strict;
};
/** @brief RGB association is independent of the input description. */
struct RgbRampOptions : ColorRampOptions {
  /** @brief Default None for RGB, Premultiplied for explicitly described RGBA.
   */
  std::optional<ColorAssociation> output_association;
};
/** @brief Original unwrapped hue; no shortest-path or normalization mode. */
struct HueRampOptions : ColorRampOptions {
  /** @brief Radian/PiMultiple; defaults to input unit, RationalPi to
   * PiMultiple. */
  std::optional<ColorHueUnit> output_hue_unit;
};
/** @brief sRGB-primary/D65/sRGB-transfer description for RGB ramp inputs.
 * association defaults to None (three channels). Four-channel inputs must
 * explicitly select Straight or Premultiplied. No payload conversion occurs.
 * Pure/thread-safe; allocation may throw bad_alloc.
 */
inline ColorArrayDescriptor color_ramp_rgb_description(
    ColorAssociation association = ColorAssociation::None) {
  ColorArrayDescriptor result;
  result.model = ColorModel::Rgb;
  result.association = association;
  auto coordinates =
      color_primary_coordinates(ColorPrimaryPreset::Srgb).take_value();
  result.white = coordinates.white;
  result.primaries = coordinates.primaries;
  result.transfer = ColorTransfer{};
  return result;
}
/** @brief Explicit CMYK printing profile identity; import/bind its bytes
 * separately. Pure, allocation-free and thread-safe. Compilation resolves the
 * owning resource.
 */
inline ColorArrayDescriptor color_ramp_cmyk_description(
    ColorProfileIdentity identity) {
  ColorArrayDescriptor result;
  result.model = ColorModel::Cmyk;
  result.reference = ColorReference::ProfileRelative;
  result.white.reset();
  result.profile = identity;
  return result;
}
namespace color_ramp_detail {
inline Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument,
          message,
          FailureReason::InvalidDomain,
          {FailureOrigin::Schema, FailureScope::Unspecified}};
}
inline ColorArrayDescriptor description(
    ColorModel model, ColorHueUnit unit = ColorHueUnit::Radian) {
  ColorArrayDescriptor result;
  result.model = model;
  if (model == ColorModel::Cielab || model == ColorModel::Cielch)
    result.white = color_white_d50();
  if (model == ColorModel::Hsl) {
    result = color_ramp_rgb_description();
    result.model = model;
  }
  if (model == ColorModel::Cielch || model == ColorModel::Oklch ||
      model == ColorModel::Hsl) {
    result.hue = unit;
    if (unit == ColorHueUnit::RationalPi)
      result.source_layout = ColorSourceLayout::RationalHueSplit;
  }
  return result;
}
inline Result<WorkflowNode> node(
    std::uint64_t id, const char* name, ColorModel model,
    std::vector<WorkflowInput> inputs, ElementType colors_type,
    const ColorArrayDescriptor& description, const ColorRampOptions& options,
    std::optional<ColorAssociation> association = {},
    std::optional<ColorHueUnit> source_hue = {},
    std::optional<ColorHueUnit> output_hue = {}) {
  using Answer = Result<WorkflowNode>;
  const auto dtype = options.dtype.value_or(colors_type);
  const auto* suffix = options.profile == CpuNumericProfile::Strict ? "_strict"
                       : options.profile == CpuNumericProfile::AppleSiliconNeon
                           ? "_accelerated_apple_silicon"
                       : options.profile == CpuNumericProfile::X86Avx2
                           ? "_accelerated_x86_64"
                           : nullptr;
  if (!id || !suffix || description.model != model ||
      (colors_type != ElementType::Float32 &&
       colors_type != ElementType::Float64) ||
      (dtype != ElementType::Float32 && dtype != ElementType::Float64) ||
      (options.out_of_domain != CurveDomain::Clamp &&
       options.out_of_domain != CurveDomain::Reject))
    return Answer(invalid("invalid color-ramp id/model/dtype/domain/profile"));
  if (source_hue &&
      (description.hue != source_hue ||
       description.source_layout != (*source_hue == ColorHueUnit::RationalPi
                                         ? ColorSourceLayout::RationalHueSplit
                                         : ColorSourceLayout::Interleaved)))
    return Answer(invalid("color-ramp source hue description mismatch"));
  if (output_hue && *output_hue != ColorHueUnit::Radian &&
      *output_hue != ColorHueUnit::PiMultiple)
    return Answer(invalid("color-ramp output hue requires a floating unit"));
  auto encoded = color_array_parameter(description);
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
  if (association) {
    if ((*association == ColorAssociation::None) !=
            (description.association == ColorAssociation::None) ||
        (*association != ColorAssociation::None &&
         *association != ColorAssociation::Straight &&
         *association != ColorAssociation::Premultiplied))
      return Answer(
          invalid("RGB output association disagrees with channel arity"));
    result.parameters["output_association"] = std::string(
        *association == ColorAssociation::None       ? "none"
        : *association == ColorAssociation::Straight ? "straight"
                                                     : "premultiplied");
  }
  if (output_hue)
    result.parameters["output_hue_unit"] = std::string(
        *output_hue == ColorHueUnit::Radian ? "radian" : "pi_multiple");
  return Answer(std::move(result));
}
}  // namespace color_ramp_detail
/** @brief Shared constructor contract for the independent color-ramp
 * primitives. Ordered inputs: input[S], stops[K], colors[K,C]; floating ports
 * independently use Float32/64. S has rank 1..7, K=1..65536 and all logical
 * products <=2^40. Rational-pi constructors split colors into Float32/64 [K,2]
 * and two Int64[K] ports. colors_type is only an authoring hint for default
 * dtype; Compiler checks actual edges. Helpers own statics, perform no I/O and
 * may be used concurrently; invalid options fail
 * InvalidArgument/InvalidDomain/Schema. Allocation may throw bad_alloc.
 * Descriptor/edge conflicts fail TypeMismatch during compilation. Nonempty
 * runtime requests collect all inputs and validate all finite strictly
 * increasing stops, then every finite position before selected one/two
 * color-row mathematics. Output values is a complete dense S+[C] Value,
 * retaining ColorArray metadata and resources. Component requests close to
 * complete colors; fragments retain the full output owner. Empty reads no
 * sample data. Typed/upstream validation covers whole inputs; any input edit
 * invalidates the complete recorded output demand. Numeric failures have Run
 * scope. Unused generic color rows remain mathematically unused, but
 * upstream/typed failures anywhere are observable. Full-input collect,
 * full-output payload, fixed arithmetic workspace and O(K) stop storage must
 * fit host budgets, including for small requested regions. Exact interpolation
 * rounds only at destination; direct/complete-identical rows keep converted
 * zero signs, mixed exact zeros are +0. RGB alone decodes and encodes transfer
 * with exact alpha; other models interpolate their own coordinates. Polar hue
 * is unwrapped, including zero chroma/saturation, with certified pi unit
 * conversion. No gamut clipping, model conversion, adaptation or ICC CMM. Host
 * work/capacity/stage/cancellation bounds apply to every refinement; an
 * unfinished proof fails ResourceExhausted. Immutable outputs outlive contexts.
 */
/** @brief Linear-light RGB/RGBA; default description is three-channel sRGB. */
inline Result<WorkflowNode> color_ramp_rgb_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description = color_ramp_rgb_description(),
    RgbRampOptions options = {}) {
  const auto association = options.output_association.value_or(
      description.association == ColorAssociation::None
          ? ColorAssociation::None
          : ColorAssociation::Premultiplied);
  return color_ramp_detail::node(
      id, "rgb", ColorModel::Rgb,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, association);
}
/** @brief Exact component interpolation in the declared Cmyk coordinates. */
inline Result<WorkflowNode> color_ramp_cmyk_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description, ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cmyk", ColorModel::Cmyk,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Exact component interpolation in the declared Xyz coordinates. */
inline Result<WorkflowNode> color_ramp_xyz_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description =
        color_ramp_detail::description(ColorModel::Xyz),
    ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "xyz", ColorModel::Xyz,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Exact component interpolation in the declared Cielab coordinates. */
inline Result<WorkflowNode> color_ramp_cielab_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description =
        color_ramp_detail::description(ColorModel::Cielab),
    ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cielab", ColorModel::Cielab,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Exact component interpolation in the declared Oklab coordinates. */
inline Result<WorkflowNode> color_ramp_oklab_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description =
        color_ramp_detail::description(ColorModel::Oklab),
    ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "oklab", ColorModel::Oklab,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Exact component interpolation in the declared Ycbcr coordinates. */
inline Result<WorkflowNode> color_ramp_ycbcr_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description, ColorRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "ycbcr", ColorModel::Ycbcr,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options);
}
/** @brief Original Radian Cielch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_cielch_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description = color_ramp_detail::description(
        ColorModel::Cielch, ColorHueUnit::Radian),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cielch", ColorModel::Cielch,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, {}, ColorHueUnit::Radian,
      options.output_hue_unit.value_or(ColorHueUnit::Radian));
}
/** @brief Original PiMultiple Cielch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_cielch_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description = color_ramp_detail::description(
        ColorModel::Cielch, ColorHueUnit::PiMultiple),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cielch_pi", ColorModel::Cielch,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, {}, ColorHueUnit::PiMultiple,
      options.output_hue_unit.value_or(ColorHueUnit::PiMultiple));
}
/** @brief Original RationalPi Cielch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_cielch_rational_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, WorkflowInput numerator, WorkflowInput denominator,
    ElementType colors_type,
    ColorArrayDescriptor description = color_ramp_detail::description(
        ColorModel::Cielch, ColorHueUnit::RationalPi),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "cielch_rational_pi", ColorModel::Cielch,
      {std::move(input), std::move(stops), std::move(colors),
       std::move(numerator), std::move(denominator)},
      colors_type, description, options, {}, ColorHueUnit::RationalPi,
      options.output_hue_unit.value_or(ColorHueUnit::PiMultiple));
}
/** @brief Original Radian Oklch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_oklch_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description =
        color_ramp_detail::description(ColorModel::Oklch, ColorHueUnit::Radian),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "oklch", ColorModel::Oklch,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, {}, ColorHueUnit::Radian,
      options.output_hue_unit.value_or(ColorHueUnit::Radian));
}
/** @brief Original PiMultiple Oklch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_oklch_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description = color_ramp_detail::description(
        ColorModel::Oklch, ColorHueUnit::PiMultiple),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "oklch_pi", ColorModel::Oklch,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, {}, ColorHueUnit::PiMultiple,
      options.output_hue_unit.value_or(ColorHueUnit::PiMultiple));
}
/** @brief Original RationalPi Oklch hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_oklch_rational_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, WorkflowInput numerator, WorkflowInput denominator,
    ElementType colors_type,
    ColorArrayDescriptor description = color_ramp_detail::description(
        ColorModel::Oklch, ColorHueUnit::RationalPi),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "oklch_rational_pi", ColorModel::Oklch,
      {std::move(input), std::move(stops), std::move(colors),
       std::move(numerator), std::move(denominator)},
      colors_type, description, options, {}, ColorHueUnit::RationalPi,
      options.output_hue_unit.value_or(ColorHueUnit::PiMultiple));
}
/** @brief Original Radian Hsl hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_hsl_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description =
        color_ramp_detail::description(ColorModel::Hsl, ColorHueUnit::Radian),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "hsl", ColorModel::Hsl,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, {}, ColorHueUnit::Radian,
      options.output_hue_unit.value_or(ColorHueUnit::Radian));
}
/** @brief Original PiMultiple Hsl hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_hsl_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, ElementType colors_type,
    ColorArrayDescriptor description = color_ramp_detail::description(
        ColorModel::Hsl, ColorHueUnit::PiMultiple),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "hsl_pi", ColorModel::Hsl,
      {std::move(input), std::move(stops), std::move(colors)}, colors_type,
      description, options, {}, ColorHueUnit::PiMultiple,
      options.output_hue_unit.value_or(ColorHueUnit::PiMultiple));
}
/** @brief Original RationalPi Hsl hue; full-color exact interpolation. */
inline Result<WorkflowNode> color_ramp_hsl_rational_pi_node(
    std::uint64_t id, WorkflowInput input, WorkflowInput stops,
    WorkflowInput colors, WorkflowInput numerator, WorkflowInput denominator,
    ElementType colors_type,
    ColorArrayDescriptor description = color_ramp_detail::description(
        ColorModel::Hsl, ColorHueUnit::RationalPi),
    HueRampOptions options = {}) {
  return color_ramp_detail::node(
      id, "hsl_rational_pi", ColorModel::Hsl,
      {std::move(input), std::move(stops), std::move(colors),
       std::move(numerator), std::move(denominator)},
      colors_type, description, options, {}, ColorHueUnit::RationalPi,
      options.output_hue_unit.value_or(ColorHueUnit::PiMultiple));
}
}  // namespace ps::numeric
