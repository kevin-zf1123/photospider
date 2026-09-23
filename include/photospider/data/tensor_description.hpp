#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "photospider/data/color_profile_identity.hpp"
#include "photospider/data/value.hpp"

namespace ps {

/** @brief One declared component; fields describe stored samples but certify
 * no sample-domain property. Empty fields are allowed. */
struct PHOTOSPIDER_API TensorChannelDescription final {
  std::string name;
  std::string role;
  std::string unit;
};

/** @brief One logical axis description, independent of physical layout. */
struct PHOTOSPIDER_API TensorAxisDescription final {
  std::string name;
  std::string unit;
  double origin = 0;
  double step = 1;
};

/** @brief Composable tensor interpretation carried by the version-one
 * photospider.tensor-description facet. The facet does not establish sample
 * validity, color completeness, or image storage. A channel table, when
 * present, is ordered by the declared channel axis. A component describes
 * a selected channel after that axis has been removed.
 *
 * All text is strict UTF-8 of at most 128 bytes. Encoding is bounded to
 * 4096 bytes so the invocation-local replacement fits a String parameter.
 * The metadata is immutable after Value publication
 * and may be read concurrently. Owned profile resources remain separate
 * ResourceBindings; these fields cannot manufacture resource ownership.
 */
struct PHOTOSPIDER_API TensorDescription final {
  std::optional<std::uint32_t> channel_axis;
  std::vector<TensorChannelDescription> channels;
  std::optional<TensorChannelDescription> component;
  std::vector<TensorAxisDescription> axes;
  std::string model;
  std::string primaries;
  std::string transfer;
  std::string reference;
  std::string association;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<ColorProfileIdentity> profile;
};

/** @brief Encode a bounded canonical tensor description. Returns
 * InvalidArgument for malformed text or structure; throws std::bad_alloc on
 * allocation failure. Pure and thread-safe. */
PHOTOSPIDER_API Result<ValueFacet> encode_tensor_description(
    const TensorDescription& description);
/** @brief Decode only the canonical version-one facet. Returns
 * InvalidArgument for old, malformed or noncanonical bytes. */
PHOTOSPIDER_API Result<TensorDescription> decode_tensor_description(
    const ValueFacet& facet);
/** @brief Encode an invocation-local replacement as lowercase hex of the
 * canonical facet payload. The parameter is copied by the compiler and
 * contributes to invocation identity. */
PHOTOSPIDER_API Result<std::string> tensor_description_parameter(
    const TensorDescription& description);
/** @brief Parse the canonical invocation-local replacement. */
PHOTOSPIDER_API Result<TensorDescription> tensor_description_from_parameter(
    const std::string& parameter);
/** @brief Validate rank/axis/table alignment without reading tensor samples. */
PHOTOSPIDER_API Status validate_tensor_description(
    const TensorDescription& description, const ValueDescriptor& descriptor);

}  // namespace ps
