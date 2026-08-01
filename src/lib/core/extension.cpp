#include "photospider/data/extension.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/extension_internal.hpp"

namespace ps {
namespace {

/** @brief Canonical stream prefix `PSDV14`, NUL, format version one. */
constexpr std::array<std::byte, 8U> kCanonicalPrefix{
    std::byte{0x50}, std::byte{0x53}, std::byte{0x44},
    std::byte{0x56}, std::byte{0x31}, std::byte{0x34},
    std::byte{0x00}, std::byte{0x01}};  // NOLINT(whitespace/indent_namespace)

/** @brief Artifact metadata prefix `PSAV14`, NUL, format version one. */
constexpr std::array<std::byte, 8U> kArtifactPrefix{
    std::byte{0x50}, std::byte{0x53}, std::byte{0x41},
    std::byte{0x56}, std::byte{0x31}, std::byte{0x34},
    std::byte{0x00}, std::byte{0x01}};  // NOLINT(whitespace/indent_namespace)

/** @brief Canonical descriptor stream domain discriminator. */
constexpr std::uint8_t kDescriptorDomain = 1U;
/** @brief Canonical logical-content stream domain discriminator. */
constexpr std::uint8_t kContentDomain = 2U;
/** @brief Canonical provider-Layout stream domain discriminator. */
constexpr std::uint8_t kLayoutDomain = 3U;

/**
 * @brief Throws one Host-owned typed extension failure.
 * @param code Stable failure category.
 * @param message Reader-facing diagnostic.
 * @throws ExtensionContractError unconditionally.
 */
[[noreturn]] void fail(ExtensionErrorCode code, const char* message) {
  throw ExtensionContractError(code, message);
}

/**
 * @brief Checks one unsigned addition against a caller bound.
 * @param left First nonnegative term.
 * @param right Second nonnegative term.
 * @param maximum Inclusive permitted sum.
 * @return Exact checked sum.
 * @throws ExtensionContractError when addition overflows or exceeds maximum.
 */
std::size_t checked_bounded_add(std::size_t left, std::size_t right,
                                std::size_t maximum) {
  if (right > maximum || left > maximum - right) {
    fail(ExtensionErrorCode::InvalidEnvelope,
         "Extension metadata exceeds its checked size bound.");
  }
  return left + right;
}

/**
 * @brief Converts one bounded uint64 byte count to size_t.
 * @param value Candidate count.
 * @return Exactly representable size_t value.
 * @throws ExtensionContractError when the Host cannot represent the count.
 */
std::size_t checked_size(std::uint64_t value) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Serialized extension count exceeds Host size_t.");
  }
  return static_cast<std::size_t>(value);
}

/**
 * @brief Reports whether an extension kind is one frozen typed namespace.
 * @param kind Candidate kind.
 * @return True for Schema, Facet, or Layout.
 * @throws Nothing.
 */
bool valid_definition_kind(ExtensionDefinitionKind kind) noexcept {
  return kind == ExtensionDefinitionKind::Schema ||
         kind == ExtensionDefinitionKind::Facet ||
         kind == ExtensionDefinitionKind::Layout;
}

/**
 * @brief Validates one byte-preserving typed extension record.
 * @param record Candidate record.
 * @param expected Required typed namespace.
 * @throws ExtensionContractError for invalid kind, identity, version, or size.
 * @note Payload bytes are never interpreted or normalized.
 */
void validate_extension_record(const ExtensionRecord& record,
                               ExtensionDefinitionKind expected) {
  if (!valid_definition_kind(record.kind) || record.kind != expected) {
    fail(ExtensionErrorCode::InvalidEnvelope,
         "Extension record uses the wrong typed definition namespace.");
  }
  if (!record.identity.valid() || record.structural_version == 0U) {
    fail(ExtensionErrorCode::InvalidEnvelope,
         "Extension identity and structural version must be nonzero.");
  }
  if (record.payload.size() > kMaximumExtensionPayloadBytes) {
    fail(ExtensionErrorCode::InvalidEnvelope,
         "Extension payload exceeds the V-14 byte bound.");
  }
}

/**
 * @brief Validates Layout metadata without dereferencing actual buffers.
 * @param layout Candidate provider-defined Layout.
 * @throws ExtensionContractError for framing, limits, overflow, or overlap.
 * @note Full publication separately checks indices and ranges against supplied
 * BufferHandle sizes.
 */
void validate_layout_metadata(const ProviderDefinedLayout& layout) {
  validate_extension_record(layout.definition, ExtensionDefinitionKind::Layout);
  if (layout.buffers.empty() ||
      layout.buffers.size() > kMaximumBufferEnvelopes) {
    fail(ExtensionErrorCode::InvalidEnvelope,
         "Provider-defined Layout requires a bounded nonempty envelope set.");
  }
  for (std::size_t index = 0U; index < layout.buffers.size(); ++index) {
    const BufferEnvelope& envelope = layout.buffers[index];
    if (envelope.buffer_index >= kMaximumExtensionBuffers ||
        envelope.logical_role == 0U || envelope.length == 0U) {
      fail(ExtensionErrorCode::InvalidEnvelope,
           "Layout envelope has an invalid buffer index, role, or length.");
    }
    if (envelope.offset >
        std::numeric_limits<std::uint64_t>::max() - envelope.length) {
      fail(ExtensionErrorCode::InvalidBinding,
           "Layout envelope offset plus length overflows uint64.");
    }
    const std::uint64_t envelope_end = envelope.offset + envelope.length;
    for (std::size_t prior_index = 0U; prior_index < index; ++prior_index) {
      const BufferEnvelope& prior = layout.buffers[prior_index];
      if (prior.buffer_index != envelope.buffer_index ||
          prior.logical_role != envelope.logical_role) {
        continue;
      }
      const std::uint64_t prior_end = prior.offset + prior.length;
      if (envelope.offset < prior_end && prior.offset < envelope_end) {
        fail(ExtensionErrorCode::InvalidBinding,
             "Equal-role Layout envelopes overlap in one buffer.");
      }
    }
  }
}

/**
 * @brief Fixed-allocation SHA-256 implementation for canonical identities.
 *
 * @note The implementation follows FIPS 180-4 and accepts bytes incrementally.
 * It owns no external library or provider state.
 */
class Sha256 final {
 public:
  /**
   * @brief Initializes the eight SHA-256 chaining words.
   * @throws Nothing.
   */
  Sha256() noexcept
      : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
               0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

  /**
   * @brief Incorporates one borrowed byte segment.
   * @param data First byte, or null only when size is zero.
   * @param size Exact byte count.
   * @throws ExtensionContractError if SHA-256 bit length would overflow.
   * @note Input is consumed before return and never retained.
   */
  void update(const std::byte* data, std::size_t size) {
    if (size != 0U && data == nullptr) {
      fail(ExtensionErrorCode::InvalidEnvelope,
           "Canonical hash segment has a null nonempty pointer.");
    }
    if (size >
        (std::numeric_limits<std::uint64_t>::max() / 8U) - total_bytes_) {
      fail(ExtensionErrorCode::InvalidEnvelope,
           "Canonical hash input exceeds SHA-256 length framing.");
    }
    total_bytes_ += static_cast<std::uint64_t>(size);
    while (size != 0U) {
      const std::size_t copied = std::min(size, block_.size() - block_size_);
      std::memcpy(block_.data() + block_size_, data, copied);
      block_size_ += copied;
      data += copied;
      size -= copied;
      if (block_size_ == block_.size()) {
        transform(block_.data());
        block_size_ = 0U;
      }
    }
  }

  /**
   * @brief Finalizes padding and returns exact digest bytes.
   * @return Conventional big-endian SHA-256 result.
   * @throws ExtensionContractError only if internal framing is inconsistent.
   * @note The object must not be updated after this call.
   */
  std::array<std::byte, kCanonicalDigestBytes> finish() {
    const std::uint64_t bit_length = total_bytes_ * 8U;
    block_[block_size_++] = std::byte{0x80};
    if (block_size_ > 56U) {
      std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
                block_.end(), std::byte{0});
      transform(block_.data());
      block_size_ = 0U;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_size_),
              block_.begin() + 56, std::byte{0});
    for (std::size_t index = 0U; index < 8U; ++index) {
      block_[56U + index] =
          static_cast<std::byte>((bit_length >> ((7U - index) * 8U)) & 0xffU);
    }
    transform(block_.data());
    block_size_ = 0U;

    std::array<std::byte, kCanonicalDigestBytes> digest{};
    for (std::size_t word = 0U; word < state_.size(); ++word) {
      for (std::size_t octet = 0U; octet < 4U; ++octet) {
        digest[word * 4U + octet] = static_cast<std::byte>(
            (state_[word] >> ((3U - octet) * 8U)) & 0xffU);
      }
    }
    return digest;
  }

 private:
  /**
   * @brief Rotates one 32-bit word right.
   * @param value Input word.
   * @param bits Rotation count in one through thirty-one.
   * @return Rotated word.
   * @throws Nothing.
   */
  static constexpr std::uint32_t rotate_right(std::uint32_t value,
                                              std::uint32_t bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
  }

  /**
   * @brief Compresses one exact 64-byte SHA-256 block.
   * @param block Borrowed complete block.
   * @throws Nothing.
   * @note The caller owns block storage and may reuse it after return.
   */
  void transform(const std::byte* block) noexcept {
    static constexpr std::array<std::uint32_t, 64U> kRoundConstants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
        0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
        0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
        0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
        0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
        0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
        0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
        0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

    std::array<std::uint32_t, 64U> schedule{};
    for (std::size_t index = 0U; index < 16U; ++index) {
      schedule[index] =
          (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(block[index * 4U]))
           << 24U) |
          (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(block[index * 4U + 1U]))
           << 16U) |
          (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(block[index * 4U + 2U]))
           << 8U) |
          static_cast<std::uint32_t>(
              std::to_integer<std::uint8_t>(block[index * 4U + 3U]));
    }
    for (std::size_t index = 16U; index < schedule.size(); ++index) {
      const std::uint32_t before15 = schedule[index - 15U];
      const std::uint32_t before2 = schedule[index - 2U];
      const std::uint32_t sigma0 = rotate_right(before15, 7U) ^
                                   rotate_right(before15, 18U) ^
                                   (before15 >> 3U);
      const std::uint32_t sigma1 = rotate_right(before2, 17U) ^
                                   rotate_right(before2, 19U) ^
                                   (before2 >> 10U);
      schedule[index] =
          schedule[index - 16U] + sigma0 + schedule[index - 7U] + sigma1;
    }

    std::uint32_t a = state_[0U];
    std::uint32_t b = state_[1U];
    std::uint32_t c = state_[2U];
    std::uint32_t d = state_[3U];
    std::uint32_t e = state_[4U];
    std::uint32_t f = state_[5U];
    std::uint32_t g = state_[6U];
    std::uint32_t h = state_[7U];
    for (std::size_t index = 0U; index < schedule.size(); ++index) {
      const std::uint32_t sum1 =
          rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temporary1 =
          h + sum1 + choose + kRoundConstants[index] + schedule[index];
      const std::uint32_t sum0 =
          rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0U] += a;
    state_[1U] += b;
    state_[2U] += c;
    state_[3U] += d;
    state_[4U] += e;
    state_[5U] += f;
    state_[6U] += g;
    state_[7U] += h;
  }

  /** @brief Current eight SHA-256 chaining words. */
  std::array<std::uint32_t, 8U> state_{};
  /** @brief Partial block not yet compressed. */
  std::array<std::byte, 64U> block_{};
  /** @brief Used bytes in `block_`. */
  std::size_t block_size_ = 0U;
  /** @brief Total unpadded input bytes incorporated. */
  std::uint64_t total_bytes_ = 0U;
};

/**
 * @brief Bounded vector writer for canonical and artifact framing.
 * @note Every append checks the configured maximum before mutation.
 */
class ByteWriter final {
 public:
  /**
   * @brief Creates one empty bounded writer.
   * @param maximum Maximum permitted output bytes.
   * @throws Nothing.
   */
  explicit ByteWriter(std::size_t maximum) noexcept : maximum_(maximum) {}

  /**
   * @brief Appends one byte.
   * @param value Byte value.
   * @throws ExtensionContractError when the bound is exhausted.
   * @throws std::bad_alloc when vector growth fails.
   */
  void append_u8(std::uint8_t value) {
    reserve_append(1U);
    bytes_.push_back(static_cast<std::byte>(value));
  }

  /**
   * @brief Appends one big-endian uint32.
   * @param value Numeric value.
   * @throws ExtensionContractError when the bound is exhausted.
   * @throws std::bad_alloc when vector growth fails.
   */
  void append_u32(std::uint32_t value) {
    reserve_append(4U);
    for (std::size_t index = 0U; index < 4U; ++index) {
      bytes_.push_back(
          static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU));
    }
  }

  /**
   * @brief Appends one big-endian uint64.
   * @param value Numeric value.
   * @throws ExtensionContractError when the bound is exhausted.
   * @throws std::bad_alloc when vector growth fails.
   */
  void append_u64(std::uint64_t value) {
    reserve_append(8U);
    for (std::size_t index = 0U; index < 8U; ++index) {
      bytes_.push_back(
          static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU));
    }
  }

  /**
   * @brief Appends an exact borrowed byte segment.
   * @param data First byte, or null only when size is zero.
   * @param size Exact byte count.
   * @throws ExtensionContractError for null data or bound overflow.
   * @throws std::bad_alloc when vector growth fails.
   */
  void append(const std::byte* data, std::size_t size) {
    if (size != 0U && data == nullptr) {
      fail(ExtensionErrorCode::InvalidEnvelope,
           "Byte writer received a null nonempty segment.");
    }
    reserve_append(size);
    if (size == 0U) {
      return;
    }
    bytes_.insert(bytes_.end(), data, data + size);
  }

  /**
   * @brief Appends a complete fixed byte array.
   * @tparam Size Compile-time array length.
   * @param bytes Array to append.
   * @throws The same exceptions as `append`.
   */
  template <std::size_t Size>
  void append(const std::array<std::byte, Size>& bytes) {
    append(bytes.data(), bytes.size());
  }

  /**
   * @brief Appends a complete byte vector.
   * @param bytes Vector to copy.
   * @throws The same exceptions as `append`.
   */
  void append(const std::vector<std::byte>& bytes) {
    append(bytes.data(), bytes.size());
  }

  /**
   * @brief Transfers complete writer output.
   * @return Owned bounded byte vector.
   * @throws Nothing under vector move.
   */
  std::vector<std::byte> take() noexcept { return std::move(bytes_); }

  /**
   * @brief Borrows current output for nested framing.
   * @return Current byte vector.
   * @throws Nothing.
   */
  const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

 private:
  /**
   * @brief Checks one pending append before vector mutation.
   * @param count Additional bytes.
   * @throws ExtensionContractError when output would exceed `maximum_`.
   */
  void reserve_append(std::size_t count) const {
    (void)checked_bounded_add(bytes_.size(), count, maximum_);
  }

  /** @brief Owned output bytes. */
  std::vector<std::byte> bytes_;
  /** @brief Inclusive maximum output size. */
  std::size_t maximum_;
};

/**
 * @brief Transactional bounded big-endian artifact reader.
 * @note Methods advance only after complete-range checks.
 */
class ByteReader final {
 public:
  /**
   * @brief Borrows one complete serialized metadata envelope.
   * @param bytes Input retained by the caller for this reader lifetime.
   * @throws ExtensionContractError when input exceeds the hard bound.
   */
  explicit ByteReader(const std::vector<std::byte>& bytes) : bytes_(bytes) {
    if (bytes.size() > kMaximumExtensionMetadataBytes) {
      fail(ExtensionErrorCode::InvalidSerialization,
           "Serialized extension metadata exceeds the V-14 bound.");
    }
  }

  /**
   * @brief Reads one byte.
   * @return Unsigned byte value.
   * @throws ExtensionContractError on truncation.
   */
  std::uint8_t read_u8() {
    require(1U);
    return std::to_integer<std::uint8_t>(bytes_[offset_++]);
  }

  /**
   * @brief Reads one big-endian uint32.
   * @return Decoded value.
   * @throws ExtensionContractError on truncation.
   */
  std::uint32_t read_u32() {
    require(4U);
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
      value = (value << 8U) | std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }
    return value;
  }

  /**
   * @brief Reads one big-endian uint64.
   * @return Decoded value.
   * @throws ExtensionContractError on truncation.
   */
  std::uint64_t read_u64() {
    require(8U);
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
      value = (value << 8U) | std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }
    return value;
  }

  /**
   * @brief Reads an owned exact byte segment.
   * @param size Required byte count.
   * @return Owned byte-preserving copy.
   * @throws ExtensionContractError on truncation.
   * @throws std::bad_alloc when output cannot allocate.
   */
  std::vector<std::byte> read_bytes(std::size_t size) {
    require(size);
    std::vector<std::byte> result(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += size;
    return result;
  }

  /**
   * @brief Reads an exact fixed byte array.
   * @tparam Size Required compile-time byte count.
   * @return Copied fixed array.
   * @throws ExtensionContractError on truncation.
   */
  template <std::size_t Size>
  std::array<std::byte, Size> read_array() {
    require(Size);
    std::array<std::byte, Size> result{};
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), Size,
                result.begin());
    offset_ += Size;
    return result;
  }

  /**
   * @brief Reports whether all input bytes were consumed.
   * @return True when the cursor equals input size.
   * @throws Nothing.
   */
  bool finished() const noexcept { return offset_ == bytes_.size(); }

 private:
  /**
   * @brief Checks one read range without advancing.
   * @param size Required byte count.
   * @throws ExtensionContractError when the range is truncated.
   */
  void require(std::size_t size) const {
    if (size > bytes_.size() - offset_) {
      fail(ExtensionErrorCode::InvalidSerialization,
           "Serialized extension metadata is truncated.");
    }
  }

  /** @brief Borrowed complete serialized input. */
  const std::vector<std::byte>& bytes_;
  /** @brief Current checked cursor. */
  std::size_t offset_ = 0U;
};

/**
 * @brief Encodes one extension record in canonical big-endian form.
 * @param record Valid byte-preserving record.
 * @return Owned nested canonical bytes.
 * @throws ExtensionContractError for invalid framing or bound overflow.
 * @throws std::bad_alloc when output cannot allocate.
 */
std::vector<std::byte> encode_extension_record(const ExtensionRecord& record) {
  ByteWriter writer(kMaximumExtensionMetadataBytes);
  writer.append_u32(static_cast<std::uint32_t>(record.kind));
  writer.append_u64(record.identity.high);
  writer.append_u64(record.identity.low);
  writer.append_u32(record.structural_version);
  writer.append_u64(static_cast<std::uint64_t>(record.payload.size()));
  writer.append(record.payload);
  return writer.take();
}

/**
 * @brief Decodes one extension record from the artifact reader.
 * @param reader Transactional input cursor.
 * @param expected Required typed namespace.
 * @return Owned byte-preserving record.
 * @throws ExtensionContractError for malformed kind, framing, or limits.
 * @throws std::bad_alloc when payload storage cannot allocate.
 */
ExtensionRecord decode_extension_record(ByteReader& reader,
                                        ExtensionDefinitionKind expected) {
  const auto raw_kind = reader.read_u32();
  ExtensionDefinitionKind kind;
  switch (raw_kind) {
    case static_cast<std::uint32_t>(ExtensionDefinitionKind::Schema):
      kind = ExtensionDefinitionKind::Schema;
      break;
    case static_cast<std::uint32_t>(ExtensionDefinitionKind::Facet):
      kind = ExtensionDefinitionKind::Facet;
      break;
    case static_cast<std::uint32_t>(ExtensionDefinitionKind::Layout):
      kind = ExtensionDefinitionKind::Layout;
      break;
    default:
      fail(ExtensionErrorCode::InvalidSerialization,
           "Serialized extension uses an unknown definition kind.");
  }
  ExtensionRecord record;
  record.kind = kind;
  record.identity = {reader.read_u64(), reader.read_u64()};
  record.structural_version = reader.read_u32();
  const std::size_t payload_size = checked_size(reader.read_u64());
  if (payload_size > kMaximumExtensionPayloadBytes) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Serialized extension payload exceeds the V-14 bound.");
  }
  record.payload = reader.read_bytes(payload_size);
  validate_extension_record(record, expected);
  return record;
}

/**
 * @brief Writes one canonical TLV field into a SHA state.
 * @param hash Mutable Host-owned SHA state.
 * @param tag Exact one-byte field tag.
 * @param payload Exact field payload.
 * @throws ExtensionContractError if SHA length framing overflows.
 */
void hash_field(Sha256& hash, std::uint8_t tag,
                const std::vector<std::byte>& payload) {
  const std::byte tag_byte = static_cast<std::byte>(tag);
  hash.update(&tag_byte, 1U);
  std::array<std::byte, 8U> length{};
  const std::uint64_t payload_size = static_cast<std::uint64_t>(payload.size());
  for (std::size_t index = 0U; index < length.size(); ++index) {
    length[index] =
        static_cast<std::byte>((payload_size >> ((7U - index) * 8U)) & 0xffU);
  }
  hash.update(length.data(), length.size());
  hash.update(payload.data(), payload.size());
}

/**
 * @brief Initializes one canonical stream with fixed prefix and domain.
 * @param hash Mutable fresh SHA state.
 * @param domain Exact one-byte typed digest domain.
 * @throws ExtensionContractError if SHA framing cannot be represented.
 */
void begin_canonical_hash(Sha256& hash, std::uint8_t domain) {
  hash.update(kCanonicalPrefix.data(), kCanonicalPrefix.size());
  const std::byte domain_byte = static_cast<std::byte>(domain);
  hash.update(&domain_byte, 1U);
}

/**
 * @brief Builds canonical sorted Facet sequence bytes.
 * @param facets Valid unique Facet records in preserved input order.
 * @return Count-prefixed canonical sequence.
 * @throws std::bad_alloc when pointer/order or output storage cannot allocate.
 */
std::vector<std::byte> canonical_facets(
    const std::vector<ExtensionRecord>& facets) {
  std::vector<const ExtensionRecord*> sorted;
  sorted.reserve(facets.size());
  for (const ExtensionRecord& facet : facets) {
    sorted.push_back(&facet);
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const ExtensionRecord* left, const ExtensionRecord* right) {
              if (left->identity != right->identity) {
                return left->identity < right->identity;
              }
              if (left->structural_version != right->structural_version) {
                return left->structural_version < right->structural_version;
              }
              return std::lexicographical_compare(
                  left->payload.begin(), left->payload.end(),
                  right->payload.begin(), right->payload.end());
            });
  ByteWriter writer(kMaximumExtensionMetadataBytes);
  writer.append_u32(static_cast<std::uint32_t>(sorted.size()));
  for (const ExtensionRecord* facet : sorted) {
    const std::vector<std::byte> encoded = encode_extension_record(*facet);
    writer.append_u64(static_cast<std::uint64_t>(encoded.size()));
    writer.append(encoded);
  }
  return writer.take();
}

/**
 * @brief Builds canonical sorted generic buffer-envelope bytes.
 * @param envelopes Valid provider Layout envelope set.
 * @return Count-prefixed canonical sequence.
 * @throws std::bad_alloc when sorted/output storage cannot allocate.
 */
std::vector<std::byte> canonical_buffer_envelopes(
    const std::vector<BufferEnvelope>& envelopes) {
  std::vector<BufferEnvelope> sorted = envelopes;
  std::sort(sorted.begin(), sorted.end(),
            [](const BufferEnvelope& left, const BufferEnvelope& right) {
              if (left.logical_role != right.logical_role) {
                return left.logical_role < right.logical_role;
              }
              if (left.buffer_index != right.buffer_index) {
                return left.buffer_index < right.buffer_index;
              }
              if (left.offset != right.offset) {
                return left.offset < right.offset;
              }
              return left.length < right.length;
            });
  ByteWriter writer(kMaximumExtensionMetadataBytes);
  writer.append_u32(static_cast<std::uint32_t>(sorted.size()));
  for (const BufferEnvelope& envelope : sorted) {
    writer.append_u32(envelope.buffer_index);
    writer.append_u32(envelope.logical_role);
    writer.append_u64(envelope.offset);
    writer.append_u64(envelope.length);
  }
  return writer.take();
}

/**
 * @brief Writes one typed digest into artifact framing.
 * @tparam Digest Descriptor, Content, or StorageLayout digest type.
 * @param writer Mutable bounded artifact writer.
 * @param digest Typed digest to serialize.
 * @throws ExtensionContractError for an unapproved algorithm or size bound.
 * @throws std::bad_alloc when output cannot allocate.
 */
template <typename Digest>
void encode_digest(ByteWriter& writer, const Digest& digest) {
  if (digest.algorithm != CanonicalDigestAlgorithm::Sha256CanonicalV1) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Artifact uses an unapproved canonical digest algorithm.");
  }
  writer.append_u32(static_cast<std::uint32_t>(digest.algorithm));
  writer.append(digest.bytes);
}

/**
 * @brief Reads and validates one digest payload.
 * @tparam Digest Descriptor, Content, or StorageLayout digest type.
 * @param reader Transactional artifact reader.
 * @return Typed SHA-256 canonical-v1 digest.
 * @throws ExtensionContractError for an unknown algorithm or truncation.
 */
template <typename Digest>
Digest decode_digest(ByteReader& reader) {
  const std::uint32_t algorithm = reader.read_u32();
  if (algorithm !=
      static_cast<std::uint32_t>(CanonicalDigestAlgorithm::Sha256CanonicalV1)) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Artifact digest algorithm is unsupported.");
  }
  Digest digest;
  digest.algorithm = CanonicalDigestAlgorithm::Sha256CanonicalV1;
  digest.bytes = reader.read_array<kCanonicalDigestBytes>();
  return digest;
}

}  // namespace

/** @copydoc validate_data_descriptor_envelope */
void validate_data_descriptor_envelope(
    const DataDescriptorEnvelope& descriptor) {
  validate_extension_record(descriptor.schema, ExtensionDefinitionKind::Schema);
  if (descriptor.facets.size() > kMaximumExtensionFacets) {
    fail(ExtensionErrorCode::InvalidEnvelope,
         "Descriptor Facet count exceeds the V-14 bound.");
  }
  for (std::size_t index = 0U; index < descriptor.facets.size(); ++index) {
    const ExtensionRecord& facet = descriptor.facets[index];
    validate_extension_record(facet, ExtensionDefinitionKind::Facet);
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (descriptor.facets[prior].identity == facet.identity) {
        fail(ExtensionErrorCode::InvalidEnvelope,
             "Descriptor contains duplicate Facet identity.");
      }
    }
  }
}

/** @copydoc validate_provider_defined_layout */
void validate_provider_defined_layout(
    const ProviderDefinedLayout& layout,
    const std::vector<std::size_t>& buffer_sizes) {
  validate_layout_metadata(layout);
  if (buffer_sizes.empty() || buffer_sizes.size() > kMaximumExtensionBuffers) {
    fail(ExtensionErrorCode::InvalidBinding,
         "Provider-defined Value requires a bounded nonempty buffer set.");
  }
  for (std::size_t size : buffer_sizes) {
    if (size == 0U) {
      fail(ExtensionErrorCode::InvalidBinding,
           "Provider-defined Value contains an empty buffer.");
    }
  }
  for (const BufferEnvelope& envelope : layout.buffers) {
    if (envelope.buffer_index >= buffer_sizes.size()) {
      fail(ExtensionErrorCode::InvalidBinding,
           "Layout envelope references an absent buffer.");
    }
    if (envelope.offset > std::numeric_limits<std::size_t>::max() ||
        envelope.length > std::numeric_limits<std::size_t>::max()) {
      fail(ExtensionErrorCode::InvalidBinding,
           "Layout envelope cannot be represented by Host size_t.");
    }
    const std::size_t offset = static_cast<std::size_t>(envelope.offset);
    const std::size_t length = static_cast<std::size_t>(envelope.length);
    const std::size_t size = buffer_sizes[envelope.buffer_index];
    if (offset > size || length > size - offset) {
      fail(ExtensionErrorCode::InvalidBinding,
           "Layout envelope exceeds its referenced BufferHandle.");
    }
  }
}

/** @copydoc compute_descriptor_digest */
DescriptorDigest compute_descriptor_digest(
    const DataDescriptorEnvelope& descriptor) {
  validate_data_descriptor_envelope(descriptor);
  Sha256 hash;
  begin_canonical_hash(hash, kDescriptorDomain);
  hash_field(hash, 1U, encode_extension_record(descriptor.schema));
  hash_field(hash, 2U, canonical_facets(descriptor.facets));
  DescriptorDigest digest;
  digest.bytes = hash.finish();
  return digest;
}

/** @copydoc compute_storage_layout_digest */
StorageLayoutDigest compute_storage_layout_digest(
    const ProviderDefinedLayout& layout) {
  validate_layout_metadata(layout);
  Sha256 hash;
  begin_canonical_hash(hash, kLayoutDomain);
  hash_field(hash, 1U, encode_extension_record(layout.definition));
  hash_field(hash, 2U, canonical_buffer_envelopes(layout.buffers));
  StorageLayoutDigest digest;
  digest.bytes = hash.finish();
  return digest;
}

namespace internal {

/** @copydoc compute_content_digest_from_canonical_bytes */
ContentDigest compute_content_digest_from_canonical_bytes(
    const DescriptorDigest& descriptor,
    const std::vector<std::byte>& canonical_content) {
  if (descriptor.algorithm != CanonicalDigestAlgorithm::Sha256CanonicalV1) {
    fail(ExtensionErrorCode::InvalidEnvelope,
         "Content identity requires SHA-256 canonical descriptor input.");
  }
  Sha256 hash;
  begin_canonical_hash(hash, kContentDomain);
  ByteWriter descriptor_field(kMaximumExtensionMetadataBytes);
  descriptor_field.append_u32(static_cast<std::uint32_t>(descriptor.algorithm));
  descriptor_field.append(descriptor.bytes);
  hash_field(hash, 1U, descriptor_field.bytes());
  hash_field(hash, 2U, canonical_content);
  ContentDigest digest;
  digest.bytes = hash.finish();
  return digest;
}

}  // namespace internal

/** @copydoc encode_extension_artifact */
std::vector<std::byte> encode_extension_artifact(
    const ExtensionArtifactEnvelope& envelope) {
  validate_data_descriptor_envelope(envelope.descriptor);
  validate_layout_metadata(envelope.layout);
  const DescriptorDigest expected_descriptor =
      compute_descriptor_digest(envelope.descriptor);
  const StorageLayoutDigest expected_layout =
      compute_storage_layout_digest(envelope.layout);
  if (envelope.descriptor_digest.has_value() &&
      !(*envelope.descriptor_digest == expected_descriptor)) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Artifact DescriptorDigest does not match its descriptor.");
  }
  if (envelope.storage_layout_digest.has_value() &&
      !(*envelope.storage_layout_digest == expected_layout)) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Artifact StorageLayoutDigest does not match its Layout.");
  }

  ByteWriter writer(kMaximumExtensionMetadataBytes);
  writer.append(kArtifactPrefix);
  writer.append(encode_extension_record(envelope.descriptor.schema));
  writer.append_u32(
      static_cast<std::uint32_t>(envelope.descriptor.facets.size()));
  for (const ExtensionRecord& facet : envelope.descriptor.facets) {
    writer.append(encode_extension_record(facet));
  }
  writer.append(encode_extension_record(envelope.layout.definition));
  writer.append_u32(static_cast<std::uint32_t>(envelope.layout.buffers.size()));
  for (const BufferEnvelope& buffer : envelope.layout.buffers) {
    writer.append_u32(buffer.buffer_index);
    writer.append_u32(buffer.logical_role);
    writer.append_u64(buffer.offset);
    writer.append_u64(buffer.length);
  }
  std::uint8_t flags = 0U;
  if (envelope.descriptor_digest.has_value()) {
    flags |= 1U;
  }
  if (envelope.content_digest.has_value()) {
    flags |= 2U;
  }
  if (envelope.storage_layout_digest.has_value()) {
    flags |= 4U;
  }
  writer.append_u8(flags);
  if (envelope.descriptor_digest.has_value()) {
    encode_digest(writer, *envelope.descriptor_digest);
  }
  if (envelope.content_digest.has_value()) {
    encode_digest(writer, *envelope.content_digest);
  }
  if (envelope.storage_layout_digest.has_value()) {
    encode_digest(writer, *envelope.storage_layout_digest);
  }
  return writer.take();
}

/** @copydoc decode_extension_artifact */
ExtensionArtifactEnvelope decode_extension_artifact(
    const std::vector<std::byte>& bytes) {
  ByteReader reader(bytes);
  if (reader.read_array<kArtifactPrefix.size()>() != kArtifactPrefix) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Artifact extension prefix or format version is invalid.");
  }

  ExtensionArtifactEnvelope envelope;
  envelope.descriptor.schema =
      decode_extension_record(reader, ExtensionDefinitionKind::Schema);
  const std::size_t facet_count = reader.read_u32();
  if (facet_count > kMaximumExtensionFacets) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Serialized Facet count exceeds the V-14 bound.");
  }
  envelope.descriptor.facets.reserve(facet_count);
  for (std::size_t index = 0U; index < facet_count; ++index) {
    envelope.descriptor.facets.push_back(
        decode_extension_record(reader, ExtensionDefinitionKind::Facet));
  }
  envelope.layout.definition =
      decode_extension_record(reader, ExtensionDefinitionKind::Layout);
  const std::size_t buffer_count = reader.read_u32();
  if (buffer_count == 0U || buffer_count > kMaximumBufferEnvelopes) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Serialized Layout envelope count is outside the V-14 bound.");
  }
  envelope.layout.buffers.reserve(buffer_count);
  for (std::size_t index = 0U; index < buffer_count; ++index) {
    envelope.layout.buffers.push_back({reader.read_u32(), reader.read_u32(),
                                       reader.read_u64(), reader.read_u64()});
  }
  const std::uint8_t flags = reader.read_u8();
  if ((flags & static_cast<std::uint8_t>(~7U)) != 0U) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Serialized artifact digest flags contain unknown bits.");
  }
  if ((flags & 1U) != 0U) {
    envelope.descriptor_digest = decode_digest<DescriptorDigest>(reader);
  }
  if ((flags & 2U) != 0U) {
    envelope.content_digest = decode_digest<ContentDigest>(reader);
  }
  if ((flags & 4U) != 0U) {
    envelope.storage_layout_digest = decode_digest<StorageLayoutDigest>(reader);
  }
  if (!reader.finished()) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Serialized extension artifact contains trailing bytes.");
  }

  validate_data_descriptor_envelope(envelope.descriptor);
  validate_layout_metadata(envelope.layout);
  const DescriptorDigest expected_descriptor =
      compute_descriptor_digest(envelope.descriptor);
  const StorageLayoutDigest expected_layout =
      compute_storage_layout_digest(envelope.layout);
  if (envelope.descriptor_digest.has_value() &&
      !(*envelope.descriptor_digest == expected_descriptor)) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Decoded DescriptorDigest does not match descriptor bytes.");
  }
  if (envelope.storage_layout_digest.has_value() &&
      !(*envelope.storage_layout_digest == expected_layout)) {
    fail(ExtensionErrorCode::InvalidSerialization,
         "Decoded StorageLayoutDigest does not match Layout bytes.");
  }
  return envelope;
}

}  // namespace ps
