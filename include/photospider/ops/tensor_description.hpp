#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "photospider/data/immutable_resource.hpp"
#include "photospider/data/tensor_span.hpp"
#include "photospider/ops/export.hpp"

namespace ps {

/** @brief Finite exact rational endpoint in reduced binary limbs.
 * Numerator and denominator use nonempty little-endian base-2^32 words with
 * no high zero word; denominator is positive and the two magnitudes are
 * coprime. Zero has numerator {0}, denominator {1} and negative=false.
 * The owning vectors are immutable after descriptor publication. This value
 * changes the public C++ ABI of TensorEndpoint and its configuration codec; all
 * producers and consumers must use a matching installed kernel version.
 */
struct TensorRationalEndpoint final {
  std::vector<std::uint32_t> numerator{0};
  std::vector<std::uint32_t> denominator{1};
  bool negative = false;
};
/** @brief Exact endpoint, with numerical finiteness an unchecked premise.
 * Integers never pass through double; binary64
 * retains represented bits, including signed zero. Non-dyadic decoded
 * endpoints use a reduced rational without numerical approximation. */
using TensorEndpoint = std::variant<
    std::int64_t, double,     // NOLINT(whitespace/indent_namespace)
    TensorRationalEndpoint>;  // NOLINT(whitespace/indent_namespace)
/** @brief Affine interpretation of stored codes. stored[0] < stored[1];
 * decoded endpoints differ and may descend. D(x) = decoded[0] +
 * (x-stored[0])*(decoded[1]-decoded[0])/(stored[1]-stored[0]). No clipping or
 * numerical conversion is performed by constructing this configuration. */
struct PHOTOSPIDER_OPS_SDK_API TensorEncoding final {
  std::array<TensorEndpoint, 2> stored{std::int64_t{0}, std::int64_t{1}};
  std::array<TensorEndpoint, 2> decoded{std::int64_t{0}, std::int64_t{1}};
};
/** @brief Same-size internal sampling relative to an explicitly identified
 * grid. Coordinate origin/step/units live in axes. Internal groups require
 * co-sited scale=(1,1), offset=(0,0); external subsampling is a codec concern.
 */
struct PHOTOSPIDER_OPS_SDK_API TensorSampling final {
  std::string grid;
  std::array<double, 2> scale{1, 1};
  std::array<double, 2> offset{0, 0};
};
/** @brief Explicit analytic endpoint assertion for a profile/configured space.
 * It is caller provenance, never an inferred equivalence or validity proof.
 * Component order/units and reference-white convention are part of identity. */
struct PHOTOSPIDER_OPS_SDK_API TensorAnalyticBinding final {
  std::string model, primaries, transfer, reference;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::vector<std::string> roles, units;
  std::string convention = "relative-v1";
};
/** @brief Config-native coordinates identified by a frozen explicit manifest,
 * canonical space name and scene/display reference. The manifest identity
 * includes context and every supplied file, including lookup absence. */
struct PHOTOSPIDER_OPS_SDK_API TensorConfiguredSpace final {
  ResourceIdentity config;
  std::string space;
  std::string reference_space;
};

/** @brief Exact descriptor equality, including binary64 endpoint zero signs. */
PHOTOSPIDER_OPS_SDK_API bool operator==(const TensorEncoding& a,
                                        const TensorEncoding& b);
PHOTOSPIDER_OPS_SDK_API bool operator==(const TensorSampling& a,
                                        const TensorSampling& b);
PHOTOSPIDER_OPS_SDK_API bool operator==(const TensorConfiguredSpace& a,
                                        const TensorConfiguredSpace& b);
PHOTOSPIDER_OPS_SDK_API bool operator==(const TensorAnalyticBinding& a,
                                        const TensorAnalyticBinding& b);

/** @brief Explicit native color-coordinate interpretation (configuration
 * encoding v5). Empty fields make no assertion. scale is relative or absolute
 * (cd/m2 Y); gray_kind is linear_y, encoded_luma, cielab_l or oklab_l. The
 * observer is descriptive and is never converted. NCL entries are exact
 * binary64 Kr,Kb, independent of the underlying RGB primaries/transfer. Hue
 * units remain on the hue component itself. These fields do not certify sample
 * validity.
 */
struct PHOTOSPIDER_OPS_SDK_API TensorModelCoordinates final {
  std::string scale;
  std::string observer;
  std::string gray_kind;
  std::optional<std::array<double, 2>> ncl_coefficients;
};
PHOTOSPIDER_OPS_SDK_API bool operator==(const TensorModelCoordinates& a,
                                        const TensorModelCoordinates& b);

/** @brief Descriptive color provenance, never a sample-validity certificate.
 * Empty fields carry no assertion. A profile identity names an immutable
 * resource; this configuration does not own its bytes.
 */
struct PHOTOSPIDER_OPS_SDK_API TensorInterpretation final {
  std::string model, primaries, transfer, reference, association;
  std::optional<std::array<double, 2>> white;
  std::optional<std::array<double, 6>> primaries_xy;
  std::optional<ResourceIdentity> profile;
  /** @brief relative-v1 stores CIELAB lightness divided by 100. */
  std::string convention = "relative-v1";
  std::optional<TensorConfiguredSpace> configured;
  std::optional<TensorAnalyticBinding> analytic_binding;
  std::optional<TensorModelCoordinates> coordinates = {};
};

/** @brief One declared component; fields describe stored samples but certify
 * no sample-domain property. Empty fields are allowed. */
struct PHOTOSPIDER_OPS_SDK_API TensorChannelDescription final {
  std::string name;
  std::string role;
  std::string unit;
  std::optional<TensorInterpretation> interpretation = {};
  std::optional<TensorEncoding> encoding = {};
  std::optional<TensorSampling> sampling = {};
};

/** @brief One logical axis description, independent of physical layout. */
struct PHOTOSPIDER_OPS_SDK_API TensorAxisDescription final {
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
struct PHOTOSPIDER_OPS_SDK_API TensorColorGroup final {
  std::string name;
  std::vector<std::uint64_t> indices;
  std::vector<TensorChannelDescription> components;
  TensorInterpretation interpretation;
  std::optional<std::uint64_t> alpha;
};

/** @brief Composable tensor interpretation carried by the version-four/five
 * explicit configuration codec. The description does not establish sample
 * validity, color completeness, or image storage. A channel table, when
 * present, is ordered by the declared channel axis. A component describes
 * a selected channel after that axis has been removed.
 *
 * All text is strict UTF-8 of at most 128 bytes. Encoding is bounded to
 * 4096 bytes so the invocation-local replacement fits a String parameter.
 * Descriptions are passed as static operation parameters; Result/TensorSpan do
 * not retain them. Owned profile resources remain in a separate ResourceSet;
 * these fields cannot manufacture resource ownership.
 */
struct PHOTOSPIDER_OPS_SDK_API TensorDescription final {
  /** @brief Retires after all admitted decoder containers. */
  ResourceLease metadata_owner;
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
  std::optional<ResourceIdentity> profile;
  std::vector<TensorColorGroup> groups;
  std::optional<TensorEncoding> encoding;
  std::optional<TensorSampling> sampling;
  std::string convention = "relative-v1";
  std::optional<TensorConfiguredSpace> configured;
  std::optional<TensorAnalyticBinding> analytic_binding;
  std::optional<TensorModelCoordinates> coordinates = {};
};

/** @brief Encode a bounded canonical tensor description. Returns
 * InvalidArgument for malformed text or structure; throws std::bad_alloc on
 * allocation failure. Pure and thread-safe. */
PHOTOSPIDER_OPS_SDK_API Result<std::vector<std::uint8_t>>
encode_tensor_description(const TensorDescription& description);
/** @brief Decode canonical v4/v5 configuration bytes. v5 explicitly carries
 * native model coordinates; v4 keeps its existing meaning. Older formats
 * reject. */
PHOTOSPIDER_OPS_SDK_API Result<TensorDescription> decode_tensor_description(
    const std::vector<std::uint8_t>& bytes);
/** @brief Encode an invocation-local replacement as lowercase hex of the
 * canonical configuration bytes. The parameter is copied by the compiler and
 * contributes to invocation identity. */
PHOTOSPIDER_OPS_SDK_API Result<std::string> tensor_description_parameter(
    const TensorDescription& description);
/** @brief Parse the canonical invocation-local replacement. */
PHOTOSPIDER_OPS_SDK_API Result<TensorDescription>
tensor_description_from_parameter(const std::string& parameter);
/** @brief Validate static rank/axis/table alignment and canonical encoding.
 * Numeric endpoint ranges, finite/positive axis values and color geometry are
 * unchecked caller premises, including for statically supplied parameters.
 * Reads no tensor payload and adds no producer/consumer validation pass. */
PHOTOSPIDER_OPS_SDK_API Status validate_tensor_description(
    const TensorDescription& description, const TensorDescriptor& descriptor);

}  // namespace ps
