#pragma once
#include <array>
#include <optional>
#include <string>

#include "photospider/core/status.hpp"
#include "photospider/ops/export.hpp"
namespace ps::format {
/** @brief Static RGB primary and white geometry for FMT-10.
 *
 * `primaries_xy` stores `{Rx, Ry, Gx, Gy, Bx, By}` and `white` stores `{x, y}`.
 * A named preset may be accompanied by coordinate assertions, which must match
 * its canonical values exactly. Custom geometry requires both arrays. Geometry
 * defines neither a transfer function nor a luminance scaling. */
struct RgbBasis final {
  std::string preset;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<std::array<double, 2>> white;
};
/** @brief Encode exact static RGB geometry for workflow parameters.
 *
 * Presets use their canonical names. Custom geometry is encoded as bounded
 * `basis-v1:` text containing lowercase hexadecimal Float64 words in
 * big-endian field order; signed zeros are canonicalized. Singular, malformed
 * or non-finite geometry and insufficient exact-work capacity return an error.
 *
 * @param basis Preset or complete custom primary/white geometry.
 * @return Canonical static parameter text on success. */
PHOTOSPIDER_OPS_SDK_API Result<std::string> rgb_basis_parameter(
    const RgbBasis& basis);
/** @brief Encode a finite, valid white chromaticity as canonical bounded
 * `xy-v1:` Float64 words. Invalid xy or insufficient exact-work capacity
 * returns an error.
 *
 * @param white White chromaticity `{x, y}`.
 * @return Canonical static parameter text on success. */
PHOTOSPIDER_OPS_SDK_API Result<std::string> xyz_white_parameter(
    const std::array<double, 2>& white);
}  // namespace ps::format
