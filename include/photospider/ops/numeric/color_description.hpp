#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "photospider/ops/tensor_description.hpp"

namespace ps::numeric {
/** @brief Original unwrapped hue coordinates used by numeric color primitives.
 */
enum class HueUnit { Radian, PiMultiple, RationalPi };

/** @brief Canonical unit spelling; nullptr for an invalid enum value. */
inline const char* hue_unit_name(HueUnit unit) {
  switch (unit) {
    case HueUnit::Radian:
      return "radian";
    case HueUnit::PiMultiple:
      return "pi_multiple";
    case HueUnit::RationalPi:
      return "rational_pi";
  }
  return nullptr;
}

/** @brief Construct a TensorDescription for one logical color tuple.
 *
 * channel_axis=0; the unique "color" group has model-order indices starting at
 * zero. An optional independent straight alpha follows the color coordinates
 * in the same tuple. Units must have one entry per color coordinate. This
 * describes the tuple, not the rank or leading sample axes of a runtime port.
 * Rational-pi hue is a logical coordinate supplied by separate Int64 p/q ports
 * only in the corresponding ramp members.
 *
 * Native coordinates use relative-v1, scene/display reference and native
 * relative units: CIELAB/CIELCh l = L-star / 100, with unchanged a*, b* and C*.
 * CMYK uses icc-native with an explicit printing-profile identity and "profile"
 * reference. No payload conversion, resource lookup or sample validation
 * occurs. Invalid metadata returns InvalidArgument/InvalidDomain/Schema. Pure
 * and thread-safe; allocation may throw bad_alloc.
 */
PHOTOSPIDER_OPS_SDK_API Result<TensorDescription> color_tuple_description(
    TensorInterpretation interpretation, const std::vector<std::string>& units,
    bool with_alpha = false);

/** @brief Resolve and validate the explicit numeric color-tuple contract.
 *
 * Requires exactly one complete canonical-order group, channel_axis=0, no
 * component/axis/encoding/sampling metadata and no unrelated channels. Global
 * and channel interpretation assertions must agree with the group. Native RGB
 * basis/transfer field structure, reference-white presence, role units and
 * coefficient-field structure are checked; numeric/geometric domains are
 * unchecked caller premises. Preset basis fields are resolved in the group.
 * Only linear, sRGB and positive power-gamma transfer are admitted by these
 * numeric primitives. Absolute, configured, analytic-binding and premultiplied
 * descriptions are rejected. Alpha is independent and straight; individual
 * operation families impose their own arity restrictions.
 *
 * Current TensorDescription codecs are used directly. There is no legacy
 * color-array decoder, unit inference or compatibility path.
 */
PHOTOSPIDER_OPS_SDK_API Result<TensorColorGroup> resolve_color_tuple(
    const TensorDescription& description);

/** @brief Check a floating sampled port against a complete tuple description.
 *
 * The port has full sample_shape rank 2..8, positive extents, product <=2^40
 * and a final coordinate dimension. With rational_hue=true, it contains only
 * the two non-hue floating coordinates; p/q are separate operation ports.
 * Structural mismatch returns TypeMismatch/Schema. This never scans samples.
 */
PHOTOSPIDER_OPS_SDK_API Status validate_color_tuple_shape(
    const TensorDescription& description, const TensorDescriptor& descriptor,
    bool rational_hue = false);
}  // namespace ps::numeric
