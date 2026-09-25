#pragma once

#include <optional>
#include <string>

#include "photospider/core/status.hpp"

namespace ps {
/** FMT-09 scalar transfer identity. This does not specify RGB primaries or
 * perform a scene/display reference conversion. */
enum class TransferCurve {
  Linear, PowerGamma, Srgb, Bt709, Bt2020, Bt1886, Pq, HlgOetf, Acescc, Acescct
};
enum class Bt2020Coefficients { Smooth, Rounded10Bit, Rounded12Bit };
struct PHOTOSPIDER_API TransferDefinition final {
  TransferCurve curve = TransferCurve::Linear;
  std::optional<double> gamma;
  std::optional<Bt2020Coefficients> coefficient_variant;
  std::optional<double> black_luminance, white_luminance;
};
/** Canonical, bounded transfer-field text for tensor-description-v4. Bare
 * identifiers encode parameter-free curves. Parameterized records start with
 * fmt09-v1: and preserve the exact supplied binary64 parameter bits.
 * BT.2020's omitted variant resolves to Smooth. Inapplicable fields fail. */
PHOTOSPIDER_API Result<std::string> encode_transfer_definition(
    const TransferDefinition& definition);
/** Accept only the canonical spelling produced by the encoder; does not guess
 * gamma, reference, luminance scale, or a curve from a primary-space label. */
PHOTOSPIDER_API Result<TransferDefinition> decode_transfer_definition(
    const std::string& text);
}  // namespace ps
