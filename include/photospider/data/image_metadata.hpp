#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @file image_metadata.hpp
 * @brief Bounded ordinary DenseImage coordinate and interpretation contracts.
 */

namespace ps {

/** @brief Frozen maximum channel records in one ordinary image descriptor. */
inline constexpr std::size_t kMaximumImageChannels = 4096U;

/** @brief Frozen maximum channel groups in one ordinary image descriptor. */
inline constexpr std::size_t kMaximumImageChannelGroups = 4096U;

/** @brief Frozen maximum member identifiers in one channel group. */
inline constexpr std::size_t kMaximumImageChannelGroupMembers = 4096U;

/** @brief Frozen total membership references in one channel schema. */
inline constexpr std::size_t kMaximumImageChannelGroupMemberships = 65536U;

/** @brief Frozen maximum bytes in one diagnostic channel/group name. */
inline constexpr std::size_t kMaximumImageDiagnosticNameBytes = 255U;

/**
 * @brief Signed immutable half-open coordinate domain of an ordinary image.
 *
 * @throws Nothing for aggregate construction, copying, and comparison.
 * @note A data window is valid only after checked validation proves both axes
 *       are nonempty and their spans are representable by both signed 64-bit
 *       extent arithmetic and `std::size_t`.
 *       This record has no Region domain key and never represents dynamic
 *       work, dirtiness, or validity.
 */
struct ImageBounds final {
  /** @brief Inclusive logical x endpoint. */
  std::int64_t x_begin = 0;

  /** @brief Inclusive logical y endpoint. */
  std::int64_t y_begin = 0;

  /** @brief Exclusive logical x endpoint. */
  std::int64_t x_end = 0;

  /** @brief Exclusive logical y endpoint. */
  std::int64_t y_end = 0;

  /**
   * @brief Compares every signed half-open endpoint.
   * @param other Bounds to compare.
   * @return True when all four endpoints match.
   * @throws Nothing.
   */
  bool operator==(const ImageBounds& other) const noexcept {
    return x_begin == other.x_begin && y_begin == other.y_begin &&
           x_end == other.x_end && y_end == other.y_end;
  }
};

/**
 * @brief Stable nonzero semantic identity of one image channel.
 * @throws Nothing for aggregate construction and comparison.
 * @note The numeric identity, not a diagnostic channel name or position, is
 *       used by sample-domain and statistics records.
 */
struct ChannelId final {
  /** @brief Nonzero stable scalar identity. */
  std::uint64_t value = 0U;

  /**
   * @brief Reports whether this identity is usable as a semantic reference.
   * @return True when the stable scalar is nonzero.
   * @throws Nothing.
   * @note Validation against a particular ChannelSchema is separate.
   */
  bool valid() const noexcept { return value != 0U; }

  /**
   * @brief Compares stable channel identities.
   * @param other Identity to compare.
   * @return True when both scalars match.
   * @throws Nothing.
   */
  bool operator==(const ChannelId& other) const noexcept {
    return value == other.value;
  }

  /**
   * @brief Provides deterministic scalar ordering.
   * @param other Identity to compare.
   * @return True when this scalar sorts before `other`.
   * @throws Nothing.
   */
  bool operator<(const ChannelId& other) const noexcept {
    return value < other.value;
  }
};

/**
 * @brief Stable nonzero semantic identity of one channel group.
 * @throws Nothing for aggregate construction and comparison.
 * @note Color and statistics records bind this identity rather than inferring
 *       a group from channel spelling.
 */
struct ChannelGroupId final {
  /** @brief Nonzero stable scalar identity. */
  std::uint64_t value = 0U;

  /**
   * @brief Reports whether this identity is usable as a semantic reference.
   * @return True when the stable scalar is nonzero.
   * @throws Nothing.
   * @note Validation against a particular ChannelSchema is separate.
   */
  bool valid() const noexcept { return value != 0U; }

  /**
   * @brief Compares stable group identities.
   * @param other Identity to compare.
   * @return True when both scalars match.
   * @throws Nothing.
   */
  bool operator==(const ChannelGroupId& other) const noexcept {
    return value == other.value;
  }

  /**
   * @brief Provides deterministic scalar ordering.
   * @param other Identity to compare.
   * @return True when this scalar sorts before `other`.
   * @throws Nothing.
   */
  bool operator<(const ChannelGroupId& other) const noexcept {
    return value < other.value;
  }
};

/**
 * @brief One channel-axis entry with stable identity and diagnostic spelling.
 * @throws std::bad_alloc when copying the diagnostic name allocates and fails.
 * @note Equality intentionally ignores `diagnostic_name`; names never select
 *       roles or enter canonical descriptor/content identity.
 */
struct ChannelDescription final {
  /** @brief Stable semantic identity in channel-axis order. */
  ChannelId id;

  /** @brief Optional bounded spelling bytes used only for diagnostics. */
  std::string diagnostic_name;

  /**
   * @brief Compares semantic channel identity.
   * @param other Channel record to compare.
   * @return True when stable IDs match, regardless of diagnostic spelling.
   * @throws Nothing.
   */
  bool operator==(const ChannelDescription& other) const noexcept {
    return id == other.id;
  }
};

/**
 * @brief Bounded stable grouping of ordinary image channels.
 * @throws std::bad_alloc when owned name/member storage allocation fails.
 * @note Members are validated as nonempty, strictly increasing, and present
 *       in the enclosing schema. Equality excludes `diagnostic_name`.
 */
struct ChannelGroupDescription final {
  /** @brief Stable semantic group identity. */
  ChannelGroupId id;

  /** @brief Optional bounded spelling bytes used only for diagnostics. */
  std::string diagnostic_name;

  /** @brief Strictly increasing stable channel identities in this group. */
  std::vector<ChannelId> members;

  /**
   * @brief Compares semantic group identity and membership.
   * @param other Group record to compare.
   * @return True when ID and ordered members match.
   * @throws Nothing under vector equality.
   */
  bool operator==(const ChannelGroupDescription& other) const noexcept {
    return id == other.id && members == other.members;
  }
};

/**
 * @brief Bounded stable channel and group authority for one ordinary image.
 * @throws std::bad_alloc when copied owned records cannot allocate.
 * @note Channel vector order is the channel-axis order. Groups are validated
 *       in strictly increasing group-ID order for one canonical spelling.
 */
struct ChannelSchema final {
  /** @brief One stable record per logical channel-axis element. */
  std::vector<ChannelDescription> channels;

  /** @brief Canonically ordered optional semantic groups. */
  std::vector<ChannelGroupDescription> groups;

  /**
   * @brief Compares complete semantic channel and group facts.
   * @param other Schema to compare.
   * @return True when stable channel order, group IDs, and memberships match.
   * @throws Nothing under vector equality.
   * @note Diagnostic names are ignored by element equality.
   */
  bool operator==(const ChannelSchema& other) const noexcept {
    return channels == other.channels && groups == other.groups;
  }
};

/**
 * @brief Closed version-1 classification of encoded sample values.
 * @throws Nothing for ordinary enum operations.
 * @note This classification does not change storage representability or
 *       authorize conversion.
 */
enum class SampleEncodingKind : std::uint32_t {
  /** @brief Values are interpreted directly in their declared numeric units. */
  Value = 0U,
  /** @brief Values use a normalized sample encoding convention. */
  Normalized = 1U,
  /** @brief Values represent explicit storage-independent code values. */
  CodeValue = 2U,
};

/**
 * @brief Versioned declared encoding classification for ordinary samples.
 * @throws Nothing for aggregate construction and comparison.
 * @note Structural version 1 is the only accepted version in DI-1.
 */
struct SampleEncoding final {
  /** @brief Exact bounded record structural version. */
  std::uint32_t structural_version = 1U;

  /** @brief Closed interpretation kind. */
  SampleEncodingKind kind = SampleEncodingKind::Value;

  /**
   * @brief Compares version and encoding kind.
   * @param other Encoding record to compare.
   * @return True when both fields match.
   * @throws Nothing.
   */
  bool operator==(const SampleEncoding& other) const noexcept {
    return structural_version == other.structural_version && kind == other.kind;
  }
};

/**
 * @brief Closed version-1 meaning of one declared sample interval.
 * @throws Nothing for ordinary enum operations.
 */
enum class SampleDomainKind : std::uint32_t {
  /** @brief Inclusive normalized interval. */
  Normalized = 0U,
  /** @brief Inclusive legal signal interval. */
  Legal = 1U,
  /** @brief Inclusive explicit code-value interval. */
  CodeValue = 2U,
};

/**
 * @brief One finite inclusive declared sample interval.
 * @throws Nothing for aggregate construction and comparison.
 * @note Validation requires `minimum <= maximum`; the interval is declarative
 *       and is never an observed statistic.
 */
struct SampleDomain final {
  /** @brief Declared interval interpretation. */
  SampleDomainKind kind = SampleDomainKind::Normalized;

  /** @brief Finite inclusive lower endpoint. */
  double minimum = 0.0;

  /** @brief Finite inclusive upper endpoint. */
  double maximum = 1.0;

  /**
   * @brief Compares the exact declared interval.
   * @param other Domain to compare.
   * @return True when kind and binary floating values compare equal.
   * @throws Nothing.
   */
  bool operator==(const SampleDomain& other) const noexcept {
    return kind == other.kind && minimum == other.minimum &&
           maximum == other.maximum;
  }
};

/**
 * @brief Per-channel replacement for the default declared sample domain.
 * @throws Nothing for aggregate construction and comparison.
 */
struct ChannelSampleDomain final {
  /** @brief Stable channel identity receiving the override. */
  ChannelId channel;

  /** @brief Complete replacement declared domain. */
  SampleDomain domain;

  /**
   * @brief Compares channel identity and complete domain.
   * @param other Override to compare.
   * @return True when both records match.
   * @throws Nothing.
   */
  bool operator==(const ChannelSampleDomain& other) const noexcept {
    return channel == other.channel && domain == other.domain;
  }
};

/**
 * @brief Versioned bounded declared sample interpretation for one image.
 * @throws std::bad_alloc when copied override storage cannot allocate.
 * @note Overrides are validated in strictly increasing ChannelId order and
 *       require an enclosing ChannelSchema. Nothing here is an observation.
 */
struct SampleDomainFacet final {
  /** @brief Exact bounded record structural version. */
  std::uint32_t structural_version = 1U;

  /** @brief Storage-independent sample encoding classification. */
  SampleEncoding encoding;

  /** @brief Domain used by channels without an override. */
  SampleDomain default_domain;

  /** @brief Canonically ordered bounded per-channel replacements. */
  std::vector<ChannelSampleDomain> per_channel;

  /**
   * @brief Compares complete versioned sample interpretation.
   * @param other Facet to compare.
   * @return True when version, encoding, default, and overrides match.
   * @throws Nothing under vector equality.
   */
  bool operator==(const SampleDomainFacet& other) const noexcept {
    return structural_version == other.structural_version &&
           encoding == other.encoding &&
           default_domain == other.default_domain &&
           per_channel == other.per_channel;
  }
};

/**
 * @brief Closed version-1 color transfer-function classification.
 * @throws Nothing for ordinary enum operations.
 */
enum class ColorTransferFunction : std::uint32_t {
  /** @brief Scene-linear light values. */
  SceneLinear = 0U,
  /** @brief IEC sRGB transfer function. */
  Srgb = 1U,
  /** @brief ITU-R Rec.709 transfer function. */
  Rec709 = 2U,
  /** @brief Perceptual quantizer transfer function. */
  Pq = 3U,
  /** @brief Hybrid log-gamma transfer function. */
  Hlg = 4U,
};

/**
 * @brief Closed version-1 color-primary classification.
 * @throws Nothing for ordinary enum operations.
 */
enum class ColorPrimaries : std::uint32_t {
  /** @brief Rec.709/sRGB primaries with D65 white. */
  Rec709 = 0U,
  /** @brief Display P3 primaries with D65 white. */
  DisplayP3D65 = 1U,
  /** @brief Rec.2020 primaries with D65 white. */
  Rec2020 = 2U,
  /** @brief ACES AP0 primaries. */
  AcesAp0 = 3U,
  /** @brief ACES AP1 primaries. */
  AcesAp1 = 4U,
};

/**
 * @brief Versioned color interpretation bound to one stable channel group.
 * @throws Nothing for aggregate construction and comparison.
 * @note Validation requires the group to exist and contain at least one
 *       channel. This record never infers RGB or alpha from names.
 */
struct ColorFacet final {
  /** @brief Exact bounded record structural version. */
  std::uint32_t structural_version = 1U;

  /** @brief Stable channel group carrying these color components. */
  ChannelGroupId channel_group;

  /** @brief Explicit scene-linear or nonlinear transfer function. */
  ColorTransferFunction transfer = ColorTransferFunction::SceneLinear;

  /** @brief Explicit color-primary set. */
  ColorPrimaries primaries = ColorPrimaries::Rec709;

  /**
   * @brief Compares complete versioned color interpretation.
   * @param other Facet to compare.
   * @return True when version, group, transfer, and primaries match.
   * @throws Nothing.
   */
  bool operator==(const ColorFacet& other) const noexcept {
    return structural_version == other.structural_version &&
           channel_group == other.channel_group && transfer == other.transfer &&
           primaries == other.primaries;
  }
};

/**
 * @brief Numeric capability of one physical element encoding.
 * @throws Nothing for aggregate construction and comparison.
 * @note The finite endpoints and exceptional-value flags are derived only
 *       from element semantics plus storage encoding. Quantization, declared
 *       sample domains, colors, and observed payload values are excluded.
 */
struct StorageRepresentableRange final {
  /** @brief Lowest finite value representable by the physical encoding. */
  double finite_minimum = 0.0;

  /** @brief Highest finite value representable by the physical encoding. */
  double finite_maximum = 0.0;

  /** @brief Whether the encoding can carry a NaN value. */
  bool supports_nan = false;

  /** @brief Whether the encoding can carry positive infinity. */
  bool supports_positive_infinity = false;

  /** @brief Whether the encoding can carry negative infinity. */
  bool supports_negative_infinity = false;

  /**
   * @brief Compares complete storage numeric capability.
   * @param other Range to compare.
   * @return True when finite endpoints and all flags match.
   * @throws Nothing.
   */
  bool operator==(const StorageRepresentableRange& other) const noexcept {
    return finite_minimum == other.finite_minimum &&
           finite_maximum == other.finite_maximum &&
           supports_nan == other.supports_nan &&
           supports_positive_infinity == other.supports_positive_infinity &&
           supports_negative_infinity == other.supports_negative_infinity;
  }
};

/**
 * @brief Complete bounded ordinary DenseImage interpretation of a tensor.
 *
 * @throws std::bad_alloc when copied channel/sample metadata cannot allocate.
 * @note `data_window` is the immutable logical payload coordinate authority.
 *       `display_window` is presentation metadata. Dynamic work and validity
 *       remain separate RegionSet values. Diagnostic names are excluded from
 *       semantic equality through their nested record equality.
 */
struct ImageFacet final {
  /** @brief Logical axis used as the image x coordinate. */
  std::size_t x_axis = 0U;

  /** @brief Logical axis used as the image y coordinate. */
  std::size_t y_axis = 0U;

  /** @brief Optional logical axis used as the channel coordinate. */
  std::optional<std::size_t> channel_axis;

  /** @brief Required signed half-open logical payload coordinate window. */
  ImageBounds data_window;

  /** @brief Optional independent immutable presentation window. */
  std::optional<ImageBounds> display_window;

  /** @brief Optional stable channel and group identities. */
  std::optional<ChannelSchema> channel_schema;

  /** @brief Optional versioned declared sample interpretation. */
  std::optional<SampleDomainFacet> sample_domain;

  /** @brief Optional versioned color interpretation. */
  std::optional<ColorFacet> color;

  /**
   * @brief Compares every semantic ordinary-image fact.
   * @param other Facet to compare.
   * @return True when axes, windows, stable channel/group data, sample
   *         interpretation, and color interpretation match.
   * @throws Nothing under nested equality.
   * @note Diagnostic channel and group names are intentionally ignored.
   */
  bool operator==(const ImageFacet& other) const noexcept {
    return x_axis == other.x_axis && y_axis == other.y_axis &&
           channel_axis == other.channel_axis &&
           data_window == other.data_window &&
           display_window == other.display_window &&
           channel_schema == other.channel_schema &&
           sample_domain == other.sample_domain && color == other.color;
  }
};

/**
 * @brief Returns the checked positive x span of one image window.
 * @param bounds Candidate signed half-open window.
 * @return Positive width exactly representable by `std::size_t`.
 * @throws std::invalid_argument when x endpoints are empty or reversed.
 * @throws std::overflow_error when the exact span exceeds signed 64-bit
 *         extent arithmetic or `std::size_t`.
 * @note The y endpoints are not inspected.
 */
std::size_t image_bounds_width(const ImageBounds& bounds);

/**
 * @brief Returns the checked positive y span of one image window.
 * @param bounds Candidate signed half-open window.
 * @return Positive height exactly representable by `std::size_t`.
 * @throws std::invalid_argument when y endpoints are empty or reversed.
 * @throws std::overflow_error when the exact span exceeds signed 64-bit
 *         extent arithmetic or `std::size_t`.
 * @note The x endpoints are not inspected.
 */
std::size_t image_bounds_height(const ImageBounds& bounds);

}  // namespace ps
