#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "photospider/data/color_profile_identity.hpp"
#include "photospider/data/value.hpp"

namespace ps {

/** @brief Exact finite endpoint: integers never pass through double. Binary64
 * values retain their represented bits, including signed zero. */
using TensorEndpoint = std::variant<std::int64_t, double>;
/** @brief Affine interpretation of stored codes. stored[0] < stored[1];
 * decoded endpoints differ and may descend. D(x) = decoded[0] +
 * (x-stored[0])*(decoded[1]-decoded[0])/(stored[1]-stored[0]). No clipping or
 * numerical conversion is performed by attaching this record. */
struct PHOTOSPIDER_API TensorEncoding final {
  std::array<TensorEndpoint, 2> stored{std::int64_t{0}, std::int64_t{1}};
  std::array<TensorEndpoint, 2> decoded{std::int64_t{0}, std::int64_t{1}};
};
/** @brief Same-size internal sampling relative to an explicitly identified
 * grid. Coordinate origin/step/units live in axes. Internal groups require
 * co-sited scale=(1,1), offset=(0,0); external subsampling is a codec concern.
 */
struct PHOTOSPIDER_API TensorSampling final {
  std::string grid;
  std::array<double, 2> scale{1, 1};
  std::array<double, 2> offset{0, 0};
};
/** @brief Explicit analytic endpoint assertion for a profile/configured space.
 * It is caller provenance, never an inferred equivalence or validity proof.
 * Component order/units and reference-white convention are part of identity. */
struct PHOTOSPIDER_API TensorAnalyticBinding final {
  std::string model, primaries, transfer, reference;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::vector<std::string> roles, units;
  std::string convention = "relative-v1";
};
/** @brief Config-native coordinates identified by a frozen explicit manifest,
 * canonical space name and scene/display reference. The manifest identity
 * includes context and every supplied file, including lookup absence. */
struct PHOTOSPIDER_API TensorConfiguredSpace final {
  ColorProfileIdentity config;
  std::string space;
  std::string reference_space;
};

/** @brief Exact descriptor equality, including binary64 endpoint zero signs. */
PHOTOSPIDER_API bool operator==(const TensorEncoding& a,
                                const TensorEncoding& b);
PHOTOSPIDER_API bool operator==(const TensorSampling& a,
                                const TensorSampling& b);
PHOTOSPIDER_API bool operator==(const TensorConfiguredSpace& a,
                                const TensorConfiguredSpace& b);
PHOTOSPIDER_API bool operator==(const TensorAnalyticBinding& a,
                                const TensorAnalyticBinding& b);

/** @brief Descriptive color provenance, never a sample-validity certificate.
 * Empty fields carry no assertion. Profiles are immutable owned resources.
 */
struct PHOTOSPIDER_API TensorInterpretation final {
  std::string model, primaries, transfer, reference, association;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<ColorProfileIdentity> profile;
  /** @brief relative-v1 stores CIELAB lightness divided by 100. */
  std::string convention = "relative-v1";
  std::optional<TensorConfiguredSpace> configured;
  std::optional<TensorAnalyticBinding> analytic_binding;
};

/** @brief One declared component; fields describe stored samples but certify
 * no sample-domain property. Empty fields are allowed. */
struct PHOTOSPIDER_API TensorChannelDescription final {
  std::string name;
  std::string role;
  std::string unit;
  std::optional<TensorInterpretation> interpretation = {};
  std::optional<TensorEncoding> encoding = {};
  std::optional<TensorSampling> sampling = {};
};

/** @brief One logical axis description, independent of physical layout. */
struct PHOTOSPIDER_API TensorAxisDescription final {
  std::string name;
  std::string unit;
  double origin = 0;
  double step = 1;
};

/** @brief Explicit complete color group. components correspond to indices;
 * alpha, when present, is an internal index outside the color components.
 * Group fields and explicitly supplied channel fields must agree. No sample
 * validation or numerical conversion is implied by this description.
 */
struct PHOTOSPIDER_API TensorColorGroup final {
  std::string name;
  std::vector<std::uint64_t> indices;
  std::vector<TensorChannelDescription> components;
  TensorInterpretation interpretation;
  std::optional<std::uint64_t> alpha;
};

/** @brief Composable tensor interpretation carried by the version-three
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
  std::vector<TensorColorGroup> groups;
  std::optional<TensorEncoding> encoding;
  std::optional<TensorSampling> sampling;
  std::string convention = "relative-v1";
  std::optional<TensorConfiguredSpace> configured;
  std::optional<TensorAnalyticBinding> analytic_binding;
};

/** @brief Encode a bounded canonical tensor description. Returns
 * InvalidArgument for malformed text or structure; throws std::bad_alloc on
 * allocation failure. Pure and thread-safe. */
PHOTOSPIDER_API Result<ValueFacet> encode_tensor_description(
    const TensorDescription& description);
/** @brief Decode only the canonical version-three facet. Returns
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
