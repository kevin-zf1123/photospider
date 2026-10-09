#pragma once

#include <optional>
#include <string>

#include "photospider/core/status.hpp"

namespace ps {
/** FMT-09 scalar transfer identity and its static parameters.
 *
 * A transfer curve maps one scalar encoding to its corresponding linear
 * quantity or back. It does not specify RGB primaries or perform a
 * scene/display reference conversion. `gamma` is used only by PowerGamma;
 * `coefficient_variant` only by Bt2020; and `black_luminance` plus
 * `white_luminance` only by Bt1886. */
enum class TransferCurve {
  Linear,
  PowerGamma,
  Srgb,
  Bt709,
  Bt2020,
  Bt1886,
  Pq,
  HlgOetf,
  Acescc,
  Acescct
};
enum class Bt2020Coefficients { Smooth, Rounded10Bit, Rounded12Bit };
struct PHOTOSPIDER_API TransferDefinition final {
  TransferCurve curve = TransferCurve::Linear;
  std::optional<double> gamma;
  std::optional<Bt2020Coefficients> coefficient_variant;
  std::optional<double> black_luminance, white_luminance;
};
/** Encode one complete transfer identity in canonical bounded text suitable for
 * a tensor-description transfer field. Parameter-free curves use their bare
 * identifiers. Parameterized records use the `fmt09-v1:` prefix and preserve
 * exact supplied Float64 parameter bits. An omitted BT.2020 variant resolves
 * to Smooth. Inapplicable fields, non-finite parameters, or invalid parameter
 * relationships return InvalidArgument with InvalidDomain reason.
 *
 * @param definition Curve and its applicable parameters.
 * @return Canonical transfer identity text on success. */
PHOTOSPIDER_API Result<std::string> encode_transfer_definition(
    const TransferDefinition& definition);
/** Decode a canonical transfer identity produced by
 * encode_transfer_definition. The decoder does not infer gamma, reference,
 * luminance scale, or a curve from a primary-space label.
 *
 * @param text Bounded canonical transfer identity text.
 * @return The curve and applicable parameters on success. Malformed or
 * noncanonical text returns InvalidArgument with InvalidDomain reason. */
PHOTOSPIDER_API Result<TransferDefinition> decode_transfer_definition(
    const std::string& text);
}  // namespace ps
