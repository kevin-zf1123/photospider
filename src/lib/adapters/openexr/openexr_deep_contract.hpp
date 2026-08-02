#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "photospider/data/extension.hpp"

/**
 * @file openexr_deep_contract.hpp
 * @brief Source-private canonical metadata contract for the optional OpenEXR
 * deep-scanline provider and adapter.
 */

namespace ps::openexr_deep {

/** @brief Structural version shared by every V-15 private payload. */
constexpr std::uint32_t kStructuralVersion = 1U;
/** @brief Logical-role number for the per-site uint32 count buffer. */
constexpr std::uint32_t kCountsBufferRole = 1U;
/** @brief Logical-role number for the uint64 prefix-offset buffer. */
constexpr std::uint32_t kOffsetsBufferRole = 2U;
/** @brief Smallest permitted logical-role number for channel sample buffers. */
constexpr std::uint32_t kFirstChannelBufferRole = 1024U;
/** @brief Maximum explicitly mapped channels in the bounded first vertical. */
constexpr std::size_t kMaximumChannels = kMaximumExtensionBuffers - 2U;
static_assert(sizeof(float) == sizeof(std::uint32_t),
              "OpenEXR deep contract requires 32-bit float");
static_assert(std::numeric_limits<float>::is_iec559,
              "OpenEXR deep contract requires IEEE-754 float");
/** @brief OpenEXR header attribute carrying explicit identity mappings. */
constexpr const char* kMappingAttributeName = "photospider.deep.channel_map.v1";
/** @brief Exact line-oriented mapping attribute version marker. */
constexpr std::string_view kMappingAttributeMarker =
    "photospider-openexr-deep-map-v1";  // NOLINT(whitespace/indent_namespace)
/** @brief Maximum encoded bytes for one canonical mapping record. */
constexpr std::size_t kMaximumMappingRecordSize = 588U;

/**
 * @brief Computes the bounded complete mapping-attribute size.
 * @return Marker, newline, and maximum canonical channel records in bytes.
 * @throws Nothing.
 */
constexpr std::size_t maximum_mapping_attribute_size() noexcept {
  return kMappingAttributeMarker.size() + 1U +
         kMaximumChannels * kMaximumMappingRecordSize;
}

/** @brief Maximum complete mapping-attribute size accepted by the decoder. */
constexpr std::size_t kMaximumMappingAttributeSize =
    maximum_mapping_attribute_size();  // NOLINT(whitespace/indent_namespace)

/** @brief Permanent identity of the optional repository provider. */
constexpr ExtensionIdentity kProviderIdentity{
    0x50534f50454e4558ULL,
    0x522d444545502d31ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Permanent VariableSampleField Schema identity. */
constexpr ExtensionIdentity kVariableSampleFieldSchemaIdentity{
    0x505356415253414dULL,
    0x504c454649454c44ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Permanent ImageFacet definition identity. */
constexpr ExtensionIdentity kImageFacetIdentity{
    0x5053494d41474546ULL,
    0x414345542d563135ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Permanent DeepSampleFacet definition identity. */
constexpr ExtensionIdentity kDeepSampleFacetIdentity{
    0x5053444545504641ULL,
    0x4345542d56313531ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Permanent multi-buffer Layout definition identity. */
constexpr ExtensionIdentity kLayoutIdentity{
    0x5053444545504c41ULL,
    0x594f55542d563135ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Property identity for row-major logical-pixel count. */
constexpr ExtensionIdentity kLogicalSiteCountProperty{
    0x5053444545505349ULL,
    0x5445434f554e5431ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief Property identity for the shared declared deep-sample count. */
constexpr ExtensionIdentity kDeclaredSampleCountProperty{
    0x5053444545505341ULL,
    0x4d504c45434e5431ULL};  // NOLINT(whitespace/indent_namespace)
/** @brief TensorSlice domain identity for zero-based x/y logical pixels. */
constexpr ExtensionIdentity kLogicalPixelRegionDomain{
    0x5053444545505245ULL,
    0x47494f4e58593131ULL};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Signed half-open two-dimensional bounds.
 * @throws Nothing for value operations.
 */
struct SignedBounds final {
  /** @brief Inclusive minimum x coordinate. */
  std::int64_t min_x = 0;
  /** @brief Inclusive minimum y coordinate. */
  std::int64_t min_y = 0;
  /** @brief Exclusive maximum x coordinate. */
  std::int64_t max_x = 0;
  /** @brief Exclusive maximum y coordinate. */
  std::int64_t max_y = 0;

  /**
   * @brief Compares all four signed limits.
   * @param other Bounds to compare.
   * @return True when every limit matches.
   * @throws Nothing.
   */
  constexpr bool operator==(const SignedBounds& other) const noexcept {
    return min_x == other.min_x && min_y == other.min_y &&
           max_x == other.max_x && max_y == other.max_y;
  }
};

/**
 * @brief One explicitly identified OpenEXR channel mapping.
 * @throws std::bad_alloc when copied diagnostic-name storage cannot allocate.
 * @note The name is diagnostic only; both identities carry semantics.
 */
struct ChannelMapping final {
  /** @brief OpenEXR file-channel name used only for codec lookup/diagnostics.
   */
  std::string diagnostic_name;
  /** @brief Permanent channel identity independent of the file name. */
  ExtensionIdentity channel_identity;
  /** @brief Permanent semantic-role identity independent of the file name. */
  ExtensionIdentity semantic_role_identity;
  /** @brief Nonzero provider Layout role for this channel buffer. */
  std::uint32_t buffer_role = 0U;

  /**
   * @brief Compares the complete explicit mapping.
   * @param other Mapping to compare.
   * @return True when diagnostic and semantic fields all match.
   * @throws Nothing under string equality.
   */
  bool operator==(const ChannelMapping& other) const noexcept {
    return diagnostic_name == other.diagnostic_name &&
           channel_identity == other.channel_identity &&
           semantic_role_identity == other.semantic_role_identity &&
           buffer_role == other.buffer_role;
  }
};

/**
 * @brief Complete logical metadata shared by descriptor and Layout payloads.
 * @throws std::bad_alloc when copied channel storage cannot allocate.
 */
struct DeepMetadata final {
  /** @brief Signed half-open data window that enumerates logical sites. */
  SignedBounds data_window;
  /** @brief Signed half-open display window preserved independently. */
  SignedBounds display_window;
  /** @brief Explicit mappings sorted by permanent channel identity. */
  std::vector<ChannelMapping> channels;
  /** @brief Checked row-major site count derived from the data window. */
  std::uint64_t logical_site_count = 0U;
  /** @brief Checked shared terminal prefix offset. */
  std::uint64_t sample_count = 0U;
};

/**
 * @brief Typed private payload kind used by exact binary framing.
 * @throws Nothing for enum operations.
 */
enum class PayloadKind : std::uint32_t {
  /** @brief VariableSampleField Schema payload. */
  Schema = 0x53434831U,
  /** @brief ImageFacet signed-window payload. */
  ImageFacet = 0x494d4731U,
  /** @brief DeepSampleFacet explicit-channel payload. */
  DeepFacet = 0x44455031U,
  /** @brief Provider-defined multi-buffer Layout payload. */
  Layout = 0x4c415931U,
};

/**
 * @brief Appends one little-endian uint32 to a private canonical payload.
 * @param output Destination vector.
 * @param value Scalar to append.
 * @throws std::bad_alloc when destination growth fails.
 */
inline void append_u32(std::vector<std::byte>* output, std::uint32_t value) {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    output->push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

/**
 * @brief Appends one little-endian uint64 to a private canonical payload.
 * @param output Destination vector.
 * @param value Scalar to append.
 * @throws std::bad_alloc when destination growth fails.
 */
inline void append_u64(std::vector<std::byte>* output, std::uint64_t value) {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    output->push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

/**
 * @brief Appends one signed scalar by its exact two's-complement bits.
 * @param output Destination vector.
 * @param value Signed value to append.
 * @throws std::bad_alloc when destination growth fails.
 */
inline void append_i64(std::vector<std::byte>* output, std::int64_t value) {
  append_u64(output, static_cast<std::uint64_t>(value));
}

/**
 * @brief Appends one permanent identity in high-word then low-word order.
 * @param output Destination vector.
 * @param identity Valid identity to append.
 * @throws std::bad_alloc when destination growth fails.
 */
inline void append_identity(std::vector<std::byte>* output,
                            ExtensionIdentity identity) {
  append_u64(output, identity.high);
  append_u64(output, identity.low);
}

/**
 * @brief Appends all signed bounds limits in x/y minimum/maximum order.
 * @param output Destination vector.
 * @param bounds Bounds to append.
 * @throws std::bad_alloc when destination growth fails.
 */
inline void append_bounds(std::vector<std::byte>* output,
                          const SignedBounds& bounds) {
  append_i64(output, bounds.min_x);
  append_i64(output, bounds.min_y);
  append_i64(output, bounds.max_x);
  append_i64(output, bounds.max_y);
}

/**
 * @brief Checked cursor over one borrowed private payload.
 * @throws std::invalid_argument for truncated or malformed fields.
 * @note The cursor never retains the borrowed payload beyond its own lifetime.
 */
class PayloadCursor final {
 public:
  /**
   * @brief Starts exact bounded payload decoding.
   * @param data Borrowed first byte, nullable only for zero size.
   * @param size Exact byte count.
   * @throws std::invalid_argument for a null/nonempty pair.
   */
  PayloadCursor(const std::byte* data, std::size_t size)
      : data_(data), size_(size) {
    if (data_ == nullptr && size_ != 0U) {
      throw std::invalid_argument("OpenEXR deep payload pointer is null.");
    }
  }

  /**
   * @brief Reads one little-endian uint32.
   * @return Decoded value.
   * @throws std::invalid_argument when fewer than four bytes remain.
   */
  std::uint32_t read_u32() {
    require(4U);
    std::uint32_t result = 0U;
    for (std::uint32_t index = 0U; index < 4U; ++index) {
      result |= static_cast<std::uint32_t>(data_[offset_ + index])
                << (index * 8U);
    }
    offset_ += 4U;
    return result;
  }

  /**
   * @brief Reads one little-endian uint64.
   * @return Decoded value.
   * @throws std::invalid_argument when fewer than eight bytes remain.
   */
  std::uint64_t read_u64() {
    require(8U);
    std::uint64_t result = 0U;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
      result |= static_cast<std::uint64_t>(data_[offset_ + index])
                << (index * 8U);
    }
    offset_ += 8U;
    return result;
  }

  /**
   * @brief Reads one signed scalar from exact two's-complement bits.
   * @return Decoded signed value.
   * @throws std::invalid_argument when fewer than eight bytes remain.
   */
  std::int64_t read_i64() {
    const std::uint64_t bits = read_u64();
    if (bits <=
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return static_cast<std::int64_t>(bits);
    }
    return -1 - static_cast<std::int64_t>(
                    std::numeric_limits<std::uint64_t>::max() - bits);
  }

  /**
   * @brief Reads one permanent identity.
   * @return High/low identity pair.
   * @throws std::invalid_argument when the payload is truncated.
   */
  ExtensionIdentity read_identity() { return {read_u64(), read_u64()}; }

  /**
   * @brief Reads one signed bounds record.
   * @return Four decoded limits.
   * @throws std::invalid_argument when the payload is truncated.
   */
  SignedBounds read_bounds() {
    return {read_i64(), read_i64(), read_i64(), read_i64()};
  }

  /**
   * @brief Reads one exact byte string.
   * @param length Number of bytes to copy.
   * @return Owned string containing those bytes.
   * @throws std::invalid_argument when insufficient bytes remain.
   * @throws std::bad_alloc when output allocation fails.
   */
  std::string read_string(std::size_t length) {
    require(length);
    const auto* begin = reinterpret_cast<const char*>(data_ + offset_);
    std::string result(begin, length);
    offset_ += length;
    return result;
  }

  /**
   * @brief Requires exact payload exhaustion.
   * @throws std::invalid_argument when trailing bytes remain.
   */
  void require_end() const {
    if (offset_ != size_) {
      throw std::invalid_argument(
          "OpenEXR deep payload contains trailing bytes.");
    }
  }

 private:
  /**
   * @brief Ensures the next bounded field exists.
   * @param count Required bytes.
   * @throws std::invalid_argument on truncation or arithmetic overflow.
   */
  void require(std::size_t count) const {
    if (count > size_ - offset_) {
      throw std::invalid_argument("OpenEXR deep payload is truncated.");
    }
  }

  /** @brief Borrowed immutable payload start. */
  const std::byte* data_ = nullptr;
  /** @brief Exact payload length. */
  std::size_t size_ = 0U;
  /** @brief Next unread byte offset. */
  std::size_t offset_ = 0U;
};

/**
 * @brief Validates one signed half-open rectangle and computes its site count.
 * @param bounds Bounds to validate.
 * @return Positive checked width multiplied by height.
 * @throws std::invalid_argument for empty or inverted bounds.
 * @throws std::overflow_error when the site product overflows.
 */
inline std::uint64_t checked_site_count(const SignedBounds& bounds) {
  if (bounds.max_x <= bounds.min_x || bounds.max_y <= bounds.min_y) {
    throw std::invalid_argument("OpenEXR deep bounds are empty or inverted.");
  }
  // Ordered signed endpoints have a mathematical distance in [1, UINT64_MAX].
  // Unsigned subtraction computes that distance without signed overflow,
  // including the complete INT64_MIN..INT64_MAX span.
  const auto width_u64 = static_cast<std::uint64_t>(bounds.max_x) -
                         static_cast<std::uint64_t>(bounds.min_x);
  const auto height_u64 = static_cast<std::uint64_t>(bounds.max_y) -
                          static_cast<std::uint64_t>(bounds.min_y);
  if (height_u64 != 0U &&
      width_u64 > std::numeric_limits<std::uint64_t>::max() / height_u64) {
    throw std::overflow_error("OpenEXR deep logical-site count overflows.");
  }
  return width_u64 * height_u64;
}

/**
 * @brief Validates bounded explicit mappings and normalizes identity order.
 * @param channels Mappings to validate and sort.
 * @return Valid mappings ordered by channel identity.
 * @throws std::invalid_argument for empty/oversized, invalid, or duplicate
 * names, identities, semantic roles, or buffer roles.
 * @throws std::bad_alloc when result allocation fails.
 */
inline std::vector<ChannelMapping> normalize_channels(
    std::vector<ChannelMapping> channels) {
  if (channels.empty() || channels.size() > kMaximumChannels) {
    throw std::invalid_argument("OpenEXR deep channel count is unsupported.");
  }
  for (const ChannelMapping& channel : channels) {
    if (channel.diagnostic_name.empty() ||
        channel.diagnostic_name.size() > 255U ||
        channel.diagnostic_name.find('\0') != std::string::npos ||
        !channel.channel_identity.valid() ||
        !channel.semantic_role_identity.valid() ||
        channel.buffer_role < kFirstChannelBufferRole) {
      throw std::invalid_argument("OpenEXR deep channel mapping is malformed.");
    }
  }
  std::sort(channels.begin(), channels.end(),
            [](const ChannelMapping& left, const ChannelMapping& right) {
              return left.channel_identity < right.channel_identity;
            });
  for (std::size_t index = 1U; index < channels.size(); ++index) {
    const ChannelMapping& previous = channels[index - 1U];
    const ChannelMapping& current = channels[index];
    if (previous.channel_identity == current.channel_identity) {
      throw std::invalid_argument(
          "OpenEXR deep channel identity is duplicate.");
    }
  }
  for (std::size_t left = 0U; left < channels.size(); ++left) {
    for (std::size_t right = left + 1U; right < channels.size(); ++right) {
      if (channels[left].diagnostic_name == channels[right].diagnostic_name ||
          channels[left].semantic_role_identity ==
              channels[right].semantic_role_identity ||
          channels[left].buffer_role == channels[right].buffer_role) {
        throw std::invalid_argument(
            "OpenEXR deep channel mapping contains a duplicate field.");
      }
    }
  }
  return channels;
}

/**
 * @brief Appends a fixed payload header.
 * @param output Destination vector.
 * @param kind Exact expected payload kind.
 * @throws std::bad_alloc when destination growth fails.
 */
inline void append_payload_header(std::vector<std::byte>* output,
                                  PayloadKind kind) {
  append_u32(output, static_cast<std::uint32_t>(kind));
  append_u32(output, kStructuralVersion);
}

/**
 * @brief Validates and consumes one fixed payload header.
 * @param cursor Cursor positioned at the header.
 * @param expected Expected payload kind.
 * @throws std::invalid_argument for kind or version mismatch.
 */
inline void read_payload_header(PayloadCursor* cursor, PayloadKind expected) {
  if (cursor->read_u32() != static_cast<std::uint32_t>(expected) ||
      cursor->read_u32() != kStructuralVersion) {
    throw std::invalid_argument("OpenEXR deep payload kind/version mismatch.");
  }
}

/**
 * @brief Serializes the VariableSampleField Schema payload.
 * @param metadata Valid complete logical metadata.
 * @return Canonical private payload.
 * @throws std::invalid_argument for invalid metadata.
 * @throws std::bad_alloc when output allocation fails.
 */
inline std::vector<std::byte> encode_schema_payload(
    const DeepMetadata& metadata) {
  if (checked_site_count(metadata.data_window) != metadata.logical_site_count ||
      metadata.channels.empty()) {
    throw std::invalid_argument(
        "OpenEXR deep Schema metadata is inconsistent.");
  }
  (void)checked_site_count(metadata.display_window);
  std::vector<std::byte> output;
  append_payload_header(&output, PayloadKind::Schema);
  append_bounds(&output, metadata.data_window);
  append_bounds(&output, metadata.display_window);
  append_u32(&output, static_cast<std::uint32_t>(metadata.channels.size()));
  append_u32(&output, 0U);
  return output;
}

/**
 * @brief Serializes the ImageFacet payload.
 * @param metadata Valid complete logical metadata.
 * @return Canonical private payload.
 * @throws std::invalid_argument for invalid metadata.
 * @throws std::bad_alloc when output allocation fails.
 */
inline std::vector<std::byte> encode_image_facet_payload(
    const DeepMetadata& metadata) {
  (void)checked_site_count(metadata.data_window);
  (void)checked_site_count(metadata.display_window);
  std::vector<std::byte> output;
  append_payload_header(&output, PayloadKind::ImageFacet);
  append_bounds(&output, metadata.data_window);
  append_bounds(&output, metadata.display_window);
  append_u32(&output, 0U);  // x axis
  append_u32(&output, 1U);  // y axis
  return output;
}

/**
 * @brief Appends one complete explicit channel mapping.
 * @param output Destination vector.
 * @param channel Valid mapping.
 * @throws std::bad_alloc when destination growth fails.
 */
inline void append_channel(std::vector<std::byte>* output,
                           const ChannelMapping& channel) {
  append_u32(output,
             static_cast<std::uint32_t>(channel.diagnostic_name.size()));
  append_u32(output, channel.buffer_role);
  append_identity(output, channel.channel_identity);
  append_identity(output, channel.semantic_role_identity);
  const auto* begin =
      reinterpret_cast<const std::byte*>(channel.diagnostic_name.data());
  output->insert(output->end(), begin, begin + channel.diagnostic_name.size());
}

/**
 * @brief Reads one complete explicit channel mapping.
 * @param cursor Cursor positioned at a mapping record.
 * @return Decoded mapping.
 * @throws std::invalid_argument for truncation or invalid length.
 * @throws std::bad_alloc when name storage cannot allocate.
 */
inline ChannelMapping read_channel(PayloadCursor* cursor) {
  const std::uint32_t name_size = cursor->read_u32();
  ChannelMapping result;
  result.buffer_role = cursor->read_u32();
  result.channel_identity = cursor->read_identity();
  result.semantic_role_identity = cursor->read_identity();
  if (name_size == 0U || name_size > 255U) {
    throw std::invalid_argument("OpenEXR deep channel name length is invalid.");
  }
  result.diagnostic_name = cursor->read_string(name_size);
  return result;
}

/**
 * @brief Serializes the DeepSampleFacet explicit mapping payload.
 * @param metadata Valid complete logical metadata.
 * @return Canonical private payload.
 * @throws std::invalid_argument for invalid channel metadata.
 * @throws std::bad_alloc when output allocation fails.
 */
inline std::vector<std::byte> encode_deep_facet_payload(
    const DeepMetadata& metadata) {
  const std::vector<ChannelMapping> channels =
      normalize_channels(metadata.channels);
  std::vector<std::byte> output;
  append_payload_header(&output, PayloadKind::DeepFacet);
  append_u32(&output, static_cast<std::uint32_t>(channels.size()));
  append_u32(&output, 0U);
  for (const ChannelMapping& channel : channels) {
    append_channel(&output, channel);
  }
  return output;
}

/**
 * @brief Serializes the provider-defined Layout metadata payload.
 * @param metadata Valid complete logical metadata.
 * @return Canonical private payload.
 * @throws std::invalid_argument for invalid metadata.
 * @throws std::bad_alloc when output allocation fails.
 */
inline std::vector<std::byte> encode_layout_payload(
    const DeepMetadata& metadata) {
  const std::vector<ChannelMapping> channels =
      normalize_channels(metadata.channels);
  if (checked_site_count(metadata.data_window) != metadata.logical_site_count) {
    throw std::invalid_argument("OpenEXR deep Layout site count is invalid.");
  }
  std::vector<std::byte> output;
  append_payload_header(&output, PayloadKind::Layout);
  append_u64(&output, metadata.logical_site_count);
  append_u64(&output, metadata.sample_count);
  append_u32(&output, static_cast<std::uint32_t>(channels.size()));
  append_u32(&output, 0U);
  for (const ChannelMapping& channel : channels) {
    append_channel(&output, channel);
  }
  return output;
}

/**
 * @brief Decodes and validates the Schema payload.
 * @param data Borrowed payload start.
 * @param size Exact payload length.
 * @return Metadata containing windows, channel count placeholders, and site
 * count.
 * @throws std::invalid_argument or std::overflow_error for malformed input.
 * @throws std::bad_alloc when channel placeholders cannot allocate.
 */
inline DeepMetadata decode_schema_payload(const std::byte* data,
                                          std::size_t size) {
  PayloadCursor cursor(data, size);
  read_payload_header(&cursor, PayloadKind::Schema);
  DeepMetadata result;
  result.data_window = cursor.read_bounds();
  result.display_window = cursor.read_bounds();
  const std::uint32_t channel_count = cursor.read_u32();
  if (cursor.read_u32() != 0U || channel_count == 0U ||
      channel_count > kMaximumChannels) {
    throw std::invalid_argument(
        "OpenEXR deep Schema channel count is invalid.");
  }
  cursor.require_end();
  result.logical_site_count = checked_site_count(result.data_window);
  (void)checked_site_count(result.display_window);
  result.channels.resize(channel_count);
  return result;
}

/**
 * @brief Decodes and validates the ImageFacet payload.
 * @param data Borrowed payload start.
 * @param size Exact payload length.
 * @return Metadata containing both signed windows.
 * @throws std::invalid_argument or std::overflow_error for malformed input.
 */
inline DeepMetadata decode_image_facet_payload(const std::byte* data,
                                               std::size_t size) {
  PayloadCursor cursor(data, size);
  read_payload_header(&cursor, PayloadKind::ImageFacet);
  DeepMetadata result;
  result.data_window = cursor.read_bounds();
  result.display_window = cursor.read_bounds();
  if (cursor.read_u32() != 0U || cursor.read_u32() != 1U) {
    throw std::invalid_argument("OpenEXR deep ImageFacet axes are invalid.");
  }
  cursor.require_end();
  result.logical_site_count = checked_site_count(result.data_window);
  (void)checked_site_count(result.display_window);
  return result;
}

/**
 * @brief Decodes one DeepSampleFacet mapping payload.
 * @param data Borrowed payload start.
 * @param size Exact payload length.
 * @return Normalized explicit channel mappings.
 * @throws std::invalid_argument for malformed or duplicate mappings.
 * @throws std::bad_alloc when mapping storage cannot allocate.
 */
inline std::vector<ChannelMapping> decode_deep_facet_payload(
    const std::byte* data, std::size_t size) {
  PayloadCursor cursor(data, size);
  read_payload_header(&cursor, PayloadKind::DeepFacet);
  const std::uint32_t channel_count = cursor.read_u32();
  if (cursor.read_u32() != 0U || channel_count == 0U ||
      channel_count > kMaximumChannels) {
    throw std::invalid_argument("OpenEXR deep Facet channel count is invalid.");
  }
  std::vector<ChannelMapping> channels;
  channels.reserve(channel_count);
  for (std::uint32_t index = 0U; index < channel_count; ++index) {
    channels.push_back(read_channel(&cursor));
  }
  cursor.require_end();
  return normalize_channels(std::move(channels));
}

/**
 * @brief Decodes one provider-defined Layout payload.
 * @param data Borrowed payload start.
 * @param size Exact payload length.
 * @return Site/sample totals and normalized explicit mappings.
 * @throws std::invalid_argument for malformed input.
 * @throws std::bad_alloc when mapping storage cannot allocate.
 */
inline DeepMetadata decode_layout_payload(const std::byte* data,
                                          std::size_t size) {
  PayloadCursor cursor(data, size);
  read_payload_header(&cursor, PayloadKind::Layout);
  DeepMetadata result;
  result.logical_site_count = cursor.read_u64();
  result.sample_count = cursor.read_u64();
  const std::uint32_t channel_count = cursor.read_u32();
  if (cursor.read_u32() != 0U || result.logical_site_count == 0U ||
      channel_count == 0U || channel_count > kMaximumChannels) {
    throw std::invalid_argument("OpenEXR deep Layout metadata is invalid.");
  }
  result.channels.reserve(channel_count);
  for (std::uint32_t index = 0U; index < channel_count; ++index) {
    result.channels.push_back(read_channel(&cursor));
  }
  cursor.require_end();
  result.channels = normalize_channels(std::move(result.channels));
  return result;
}

/**
 * @brief Returns lowercase hexadecimal for one borrowed byte sequence.
 * @param bytes Bytes to encode.
 * @return Two ASCII characters per byte.
 * @throws std::bad_alloc when result allocation fails.
 */
inline std::string hex_encode(std::string_view bytes) {
  constexpr char kDigits[] = "0123456789abcdef";
  std::string output;
  output.reserve(bytes.size() * 2U);
  for (unsigned char byte : bytes) {
    output.push_back(kDigits[(byte >> 4U) & 0x0fU]);
    output.push_back(kDigits[byte & 0x0fU]);
  }
  return output;
}

/**
 * @brief Decodes one lowercase/uppercase hexadecimal byte string.
 * @param encoded Even-length hexadecimal input.
 * @return Exact decoded bytes.
 * @throws std::invalid_argument for odd length or a non-hexadecimal digit.
 * @throws std::bad_alloc when result allocation fails.
 */
inline std::string hex_decode(std::string_view encoded) {
  if ((encoded.size() % 2U) != 0U) {
    throw std::invalid_argument("OpenEXR deep mapping hex length is odd.");
  }
  /** @brief Converts one checked hexadecimal character to its nibble. */
  const auto nibble = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
      return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    throw std::invalid_argument("OpenEXR deep mapping contains non-hex text.");
  };
  std::string output;
  output.reserve(encoded.size() / 2U);
  for (std::size_t index = 0U; index < encoded.size(); index += 2U) {
    output.push_back(static_cast<char>((nibble(encoded[index]) << 4U) |
                                       nibble(encoded[index + 1U])));
  }
  return output;
}

/**
 * @brief Formats one identity as exactly 32 hexadecimal digits.
 * @param identity Valid identity to format.
 * @return Fixed-width lowercase representation.
 * @throws std::invalid_argument for the zero identity.
 * @throws std::bad_alloc when output allocation fails.
 */
inline std::string format_identity(ExtensionIdentity identity) {
  if (!identity.valid()) {
    throw std::invalid_argument("OpenEXR deep mapping identity is zero.");
  }
  const auto append_word = [](std::string* output, std::uint64_t word) {
    constexpr char kDigits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
      output->push_back(kDigits[(word >> shift) & 0x0fU]);
    }
  };
  std::string output;
  output.reserve(32U);
  append_word(&output, identity.high);
  append_word(&output, identity.low);
  return output;
}

/**
 * @brief Parses exactly 32 hexadecimal digits as one identity.
 * @param encoded Fixed-width hexadecimal representation.
 * @return Valid high/low identity.
 * @throws std::invalid_argument for bad width, digit, or zero identity.
 */
inline ExtensionIdentity parse_identity(std::string_view encoded) {
  if (encoded.size() != 32U) {
    throw std::invalid_argument(
        "OpenEXR deep mapping identity width is invalid.");
  }
  const std::string bytes = hex_decode(encoded);
  ExtensionIdentity identity;
  for (std::size_t index = 0U; index < 8U; ++index) {
    identity.high =
        (identity.high << 8U) | static_cast<unsigned char>(bytes[index]);
    identity.low =
        (identity.low << 8U) | static_cast<unsigned char>(bytes[index + 8U]);
  }
  if (!identity.valid()) {
    throw std::invalid_argument("OpenEXR deep mapping identity is zero.");
  }
  return identity;
}

/**
 * @brief Serializes explicit channel mappings into one OpenEXR string value.
 * @param channels Valid mappings.
 * @return Version marker plus one deterministic line per normalized mapping.
 * @throws std::invalid_argument for malformed mappings.
 * @throws std::bad_alloc when output allocation fails.
 */
inline std::string encode_mapping_attribute(
    std::vector<ChannelMapping> channels) {
  channels = normalize_channels(std::move(channels));
  std::string output(kMappingAttributeMarker);
  output.push_back('\n');
  for (const ChannelMapping& channel : channels) {
    output.append(hex_encode(channel.diagnostic_name));
    output.push_back('|');
    output.append(format_identity(channel.channel_identity));
    output.push_back('|');
    output.append(format_identity(channel.semantic_role_identity));
    output.push_back('|');
    output.append(std::to_string(channel.buffer_role));
    output.push_back('\n');
  }
  return output;
}

/**
 * @brief Splits one mapping record into exactly four delimited fields.
 * @param line Nonempty record without a trailing newline.
 * @return Four borrowed fields.
 * @throws std::invalid_argument for missing, extra, or empty fields.
 */
inline std::array<std::string_view, 4U> split_mapping_line(
    std::string_view line) {
  std::array<std::string_view, 4U> fields;
  std::size_t start = 0U;
  for (std::size_t index = 0U; index < fields.size(); ++index) {
    const std::size_t delimiter = line.find('|', start);
    if (index + 1U == fields.size()) {
      if (delimiter != std::string_view::npos) {
        throw std::invalid_argument(
            "OpenEXR deep mapping line has extra fields.");
      }
      fields[index] = line.substr(start);
    } else {
      if (delimiter == std::string_view::npos) {
        throw std::invalid_argument(
            "OpenEXR deep mapping line is missing fields.");
      }
      fields[index] = line.substr(start, delimiter - start);
      start = delimiter + 1U;
    }
    if (fields[index].empty()) {
      throw std::invalid_argument("OpenEXR deep mapping field is empty.");
    }
  }
  return fields;
}

/**
 * @brief Parses one canonical decimal uint32 buffer role without allocation.
 * @param encoded Nonempty decimal field emitted by std::to_string.
 * @return Exact uint32 role.
 * @throws std::invalid_argument for noncanonical text or overflow.
 */
inline std::uint32_t parse_buffer_role(std::string_view encoded) {
  if (encoded.empty() || encoded.size() > 10U ||
      (encoded.size() > 1U && encoded.front() == '0')) {
    throw std::invalid_argument("OpenEXR deep mapping role is invalid.");
  }
  std::uint64_t role = 0U;
  for (char digit : encoded) {
    if (digit < '0' || digit > '9') {
      throw std::invalid_argument("OpenEXR deep mapping role is invalid.");
    }
    role = role * 10U + static_cast<std::uint64_t>(digit - '0');
    if (role > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("OpenEXR deep mapping role is invalid.");
    }
  }
  return static_cast<std::uint32_t>(role);
}

/**
 * @brief Parses one exact versioned OpenEXR mapping attribute.
 * @param encoded Attribute string value.
 * @return Valid normalized explicit mappings.
 * @throws std::invalid_argument for malformed framing, identities, roles, or
 * duplicates.
 * @throws std::bad_alloc when mapping storage cannot allocate.
 */
inline std::vector<ChannelMapping> decode_mapping_attribute(
    std::string_view encoded) {
  if (encoded.size() > kMaximumMappingAttributeSize) {
    throw std::invalid_argument(
        "OpenEXR deep mapping attribute exceeds its bounded size.");
  }
  const std::size_t first_newline = encoded.find('\n');
  if (first_newline == std::string_view::npos ||
      encoded.substr(0U, first_newline) != kMappingAttributeMarker) {
    throw std::invalid_argument(
        "OpenEXR deep mapping attribute version is unsupported.");
  }
  std::vector<ChannelMapping> channels;
  std::size_t start = first_newline + 1U;
  while (start < encoded.size()) {
    const std::size_t newline = encoded.find('\n', start);
    const std::size_t end =
        newline == std::string_view::npos ? encoded.size() : newline;
    if (end == start) {
      throw std::invalid_argument(
          "OpenEXR deep mapping contains an empty line.");
    }
    const auto fields = split_mapping_line(encoded.substr(start, end - start));
    if (channels.size() >= kMaximumChannels || fields[0].size() > 510U) {
      throw std::invalid_argument(
          "OpenEXR deep mapping channel count or name is unsupported.");
    }
    ChannelMapping channel;
    channel.diagnostic_name = hex_decode(fields[0]);
    channel.channel_identity = parse_identity(fields[1]);
    channel.semantic_role_identity = parse_identity(fields[2]);
    channel.buffer_role = parse_buffer_role(fields[3]);
    channels.push_back(std::move(channel));
    if (newline == std::string_view::npos) {
      start = encoded.size();
    } else {
      start = newline + 1U;
    }
  }
  return normalize_channels(std::move(channels));
}

/**
 * @brief Builds the exact provider-defined descriptor records for metadata.
 * @param metadata Valid complete metadata.
 * @return Schema followed by ImageFacet and DeepSampleFacet.
 * @throws std::invalid_argument for inconsistent metadata.
 * @throws std::bad_alloc when payload storage cannot allocate.
 */
inline DataDescriptorEnvelope make_descriptor(const DeepMetadata& metadata) {
  DataDescriptorEnvelope descriptor;
  descriptor.schema = {ExtensionDefinitionKind::Schema,
                       kVariableSampleFieldSchemaIdentity, kStructuralVersion,
                       encode_schema_payload(metadata)};
  descriptor.facets.push_back({ExtensionDefinitionKind::Facet,
                               kImageFacetIdentity, kStructuralVersion,
                               encode_image_facet_payload(metadata)});
  descriptor.facets.push_back({ExtensionDefinitionKind::Facet,
                               kDeepSampleFacetIdentity, kStructuralVersion,
                               encode_deep_facet_payload(metadata)});
  return descriptor;
}

/**
 * @brief Builds exact provider Layout metadata and checked semantic lengths.
 * @param metadata Valid complete metadata.
 * @return Layout definition plus one full-range envelope per nonempty buffer.
 * @throws std::overflow_error when a byte length is not representable.
 * @throws std::invalid_argument for inconsistent metadata.
 * @throws std::bad_alloc when output storage cannot allocate.
 * @note A zero shared sample total retains channel mapping metadata but emits
 * no zero-length channel envelope.
 */
inline ProviderDefinedLayout make_layout(const DeepMetadata& metadata) {
  const auto checked_bytes = [](std::uint64_t count,
                                std::uint64_t element_bytes) -> std::uint64_t {
    if (count > std::numeric_limits<std::uint64_t>::max() / element_bytes) {
      throw std::overflow_error("OpenEXR deep buffer byte count overflows.");
    }
    return count * element_bytes;
  };
  ProviderDefinedLayout layout;
  layout.definition = {ExtensionDefinitionKind::Layout, kLayoutIdentity,
                       kStructuralVersion, encode_layout_payload(metadata)};
  layout.buffers.push_back(
      {0U, kCountsBufferRole, 0U,
       checked_bytes(metadata.logical_site_count, sizeof(std::uint32_t))});
  if (metadata.logical_site_count ==
      std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("OpenEXR deep offset count overflows.");
  }
  layout.buffers.push_back(
      {1U, kOffsetsBufferRole, 0U,
       checked_bytes(metadata.logical_site_count + 1U, sizeof(std::uint64_t))});
  const std::vector<ChannelMapping> channels =
      normalize_channels(metadata.channels);
  if (metadata.sample_count != 0U) {
    const std::uint64_t sample_bytes =
        checked_bytes(metadata.sample_count, sizeof(float));
    for (std::size_t index = 0U; index < channels.size(); ++index) {
      layout.buffers.push_back({static_cast<std::uint32_t>(index + 2U),
                                channels[index].buffer_role, 0U, sample_bytes});
    }
  }
  return layout;
}

}  // namespace ps::openexr_deep
