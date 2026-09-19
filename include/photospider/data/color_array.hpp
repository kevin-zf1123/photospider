#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>

#include "photospider/data/color_profile_identity.hpp"
#include "photospider/data/value.hpp"

namespace ps {

/** @brief Independent color-array model tags; not legacy SemanticKind values.
 */
enum class ColorModel : std::uint8_t {
  Rgb = 1,
  Xyz = 2,
  Cielab = 3,
  Cielch = 4,
  Oklab = 5,
  Oklch = 6,
  Hsl = 7,
  Ycbcr = 8,
  Cmyk = 9,
};
/** @brief Explicit coordinate reference; only CMYK is profile-relative. */
enum class ColorReference : std::uint8_t {
  SceneRelative = 1,
  DisplayRelative = 2,
  ProfileRelative = 3,
};
/** @brief Premultiplied RGB stores alpha times encoded RGB. */
enum class ColorAssociation : std::uint8_t {
  None = 0,
  Straight = 1,
  Premultiplied = 2,
};
/** @brief Split rational hue describes static ramp ports, never a Value. */
enum class ColorSourceLayout : std::uint8_t {
  Interleaved = 0,
  RationalHueSplit = 1,
};
/** @brief Original unnormalized hue; no winding-count reduction. */
enum class ColorHueUnit : std::uint8_t {
  Radian = 0,
  PiMultiple = 1,
  RationalPi = 2,
};
/** @brief Supported odd signed transfer functions; alpha is excluded. */
enum class ColorTransferKind : std::uint8_t { Linear = 0, Srgb = 1, Gamma = 2 };
/** @brief Gamma exponent is present exactly when kind is Gamma. */
struct PHOTOSPIDER_API ColorTransfer final {
  ColorTransferKind kind = ColorTransferKind::Srgb;
  std::optional<double> gamma;
};
/**
 * @brief Owned model-specific interpretation of Float32/64 colors.
 * @note Irrelevant optional fields must be absent. Runtime shape is rank 2..8
 * with channels last; one color is [1,C]. Channel roles/units are determined
 * by model/association/hue, without free-form aliases. The default is XYZ D65
 * display-relative. ICC identity alone does not establish resource ownership.
 * Immutable descriptors may be shared across threads; synchronize mutation.
 */
struct PHOTOSPIDER_API ColorArrayDescriptor final {
  ColorModel model = ColorModel::Xyz;
  ColorReference reference = ColorReference::DisplayRelative;
  ColorAssociation association = ColorAssociation::None;
  ColorSourceLayout source_layout = ColorSourceLayout::Interleaved;
  std::optional<std::array<double, 2>> white =
      std::array<double, 2>{.3127, .3290};
  std::optional<std::array<double, 6>> primaries;
  std::optional<ColorTransfer> transfer;
  std::optional<ColorHueUnit> hue;
  std::optional<std::array<double, 2>> ncl_coefficients;
  std::optional<ColorProfileIdentity> profile;
};
/** @brief Numeric primary/white presets; transfer is chosen separately. */
enum class ColorPrimaryPreset {
  Srgb,
  DisplayP3,
  Rec2020,
  AdobeRgb1998,
  ProphotoRgb,
  AcesAp0,
  AcesAp1,
};
/** @brief Numeric NCL matrix presets; these perform no color conversion. */
enum class ColorNclPreset { Bt601, Bt709, Bt2020 };
/** @brief Preset coordinates, including virtual primaries and defined white. */
struct PHOTOSPIDER_API ColorPrimaryCoordinates final {
  std::array<double, 6> primaries;
  std::array<double, 2> white;
};
/** @brief Returns literal RN64 D65 xy; thread-safe, allocation-free. */
PHOTOSPIDER_API std::array<double, 2> color_white_d65() noexcept;
/** @brief Returns literal RN64 D50 xy; thread-safe, allocation-free. */
PHOTOSPIDER_API std::array<double, 2> color_white_d50() noexcept;
/** @brief Returns numeric primaries/white, or InvalidArgument for unknown enum.
 * @throws std::bad_alloc On error diagnostic allocation. Pure/thread-safe.
 */
PHOTOSPIDER_API Result<ColorPrimaryCoordinates> color_primary_coordinates(
    ColorPrimaryPreset preset);
/** @brief Returns RN64 Kr,Kb, or InvalidArgument for unknown enum.
 * @throws std::bad_alloc On error diagnostic allocation. Pure/thread-safe.
 */
PHOTOSPIDER_API Result<std::array<double, 2>> color_ncl_coefficients(
    ColorNclPreset preset);
/** @brief Validates and encodes interleaved runtime metadata as color-array-v1.
 * @param description Borrowed descriptor; no samples or ICC bytes are read.
 * @return Canonical owned facet, or InvalidArgument/InvalidDomain.
 * @throws std::bad_alloc On allocation. Pure/thread-safe; metadata zero is
 * canonicalized, sample bits are untouched. Exact white/basis checks use
 * bounded integer arithmetic independent of the caller's floating environment.
 */
PHOTOSPIDER_API Result<ValueFacet> encode_color_array(
    const ColorArrayDescriptor& description);
/** @brief Decodes canonical runtime color-array-v1, rejecting split sources.
 * @return Owned descriptor or InvalidArgument for malformed/noncanonical data.
 * @throws std::bad_alloc On allocation. Pure/thread-safe; input not retained.
 */
PHOTOSPIDER_API Result<ColorArrayDescriptor> decode_color_array(
    const ValueFacet& facet);
/** @brief Encodes canonical lowercase static color_description String.
 * @note Also accepts rational split CIELCh/OKLCh/HSL descriptions. Such static
 * descriptions cannot be attached to Values. No manual hex is required.
 * @return At most 8192 bytes, or InvalidArgument/InvalidDomain.
 * @throws std::bad_alloc On allocation. Pure/thread-safe; input not retained.
 */
PHOTOSPIDER_API Result<std::string> color_array_parameter(
    const ColorArrayDescriptor& description);
/** @brief Decodes a canonical static String, including rational split sources.
 * @return Owned descriptor or InvalidArgument; no coercion/defaulting.
 * @throws std::bad_alloc On allocation. Pure/thread-safe; input not retained.
 */
PHOTOSPIDER_API Result<ColorArrayDescriptor> color_array_from_parameter(
    const std::string& parameter);
/** @brief Checks runtime model, dtype, channels and positive shape <=2^40.
 * @return InvalidArgument for metadata, TypeMismatch for dtype/shape.
 * @throws std::bad_alloc On diagnostics. Pure/thread-safe, no payload scan.
 */
PHOTOSPIDER_API Status validate_color_array_descriptor(
    const ColorArrayDescriptor& description, const ValueDescriptor& descriptor);
/** @brief Validates complete colors in the supplied regional Value only.
 * @param description Expected interpretation, validated independently of
 * facets.
 * @param value Immutable data; arbitrary valid byte strides are supported.
 * @param numeric_failure Error code for invalid demanded sample values.
 * @param stop Periodic cancellation/currentness probe, borrowed only for call.
 * @return Metadata/type failure, numeric_failure with InvalidDomain or
 * InvalidAssociation, or the probe's stop code. No conversion or clipping.
 * @throws std::bad_alloc On allocation. Thread-safe for immutable inputs;
 * caller floating environment is restored, including exception flags.
 */
PHOTOSPIDER_API Status validate_color_array_value(
    const ColorArrayDescriptor& description, const Value& value,
    ErrorCode numeric_failure = ErrorCode::InvalidArgument,
    const std::function<ErrorCode()>& stop = {});

}  // namespace ps
