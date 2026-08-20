#include "photospider/data/value_artifact.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/canonical_ieee754.hpp"

namespace ps {
namespace {

/** @brief Exact portable metadata magic and format discriminator. */
constexpr std::array<std::byte, 8U> kArtifactMagic{
    std::byte{0x50U}, std::byte{0x53U}, std::byte{0x56U},  // NOLINT
    std::byte{0x41U}, std::byte{0x52U}, std::byte{0x54U},  // NOLINT
    std::byte{1U},    std::byte{0U}};                      // NOLINT

/** @brief Exact canonical named-artifact-set archive discriminator. */
constexpr std::array<std::byte, 8U> kArtifactSetMagic{
    std::byte{0x50U}, std::byte{0x53U}, std::byte{0x56U},  // NOLINT
    std::byte{0x53U}, std::byte{0x45U}, std::byte{0x54U},  // NOLINT
    std::byte{1U},    std::byte{0U}};                      // NOLINT

/** @brief Frozen maximum tensor rank accepted by the portable codec. */
constexpr std::size_t kMaximumArtifactRank = 64U;

/**
 * @brief Throws one uniform malformed-artifact exception.
 * @param message Reader-facing diagnostic.
 * @throws std::invalid_argument unconditionally.
 */
[[noreturn]] void invalid_artifact(const char* message) {
  throw std::invalid_argument(message);
}

/**
 * @brief Converts size_t into the frozen uint64 metadata domain.
 * @param value Local size.
 * @return Exact uint64 value.
 * @throws std::overflow_error on wider platforms when value is too large.
 */
std::uint64_t size_to_u64(std::size_t value) {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Value artifact size exceeds uint64.");
    }
  }
  return static_cast<std::uint64_t>(value);
}

/**
 * @brief Converts uint64 metadata into local size_t without narrowing.
 * @param value Frozen metadata scalar.
 * @return Exact local size.
 * @throws std::overflow_error when local size_t cannot represent value.
 */
std::size_t u64_to_size(std::uint64_t value) {
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::size_t>::max()) {
      throw std::overflow_error("Value artifact size exceeds size_t.");
    }
  }
  return static_cast<std::size_t>(value);
}

/**
 * @brief Converts a bounded size into uint32 without narrowing.
 * @param value Local count.
 * @return Exact uint32 count.
 * @throws std::overflow_error when value exceeds uint32.
 */
std::uint32_t size_to_u32(std::size_t value) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("Value artifact count exceeds uint32.");
  }
  return static_cast<std::uint32_t>(value);
}

/**
 * @brief Checked addition in the portable aggregate byte domain.
 * @param left First nonnegative value.
 * @param right Second nonnegative value.
 * @return Exact sum.
 * @throws std::overflow_error when uint64 would wrap.
 */
std::uint64_t checked_u64_add(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    throw std::overflow_error("Value artifact aggregate size overflowed.");
  }
  return left + right;
}

/**
 * @brief Canonical little-endian bounded metadata writer.
 * @throws std::bad_alloc when output storage cannot grow.
 * @note Every mutating operation enforces the frozen total metadata bound.
 */
class Writer final {
 public:
  /**
   * @brief Appends one unsigned byte.
   * @param value Byte value.
   * @throws std::length_error when metadata exceeds the frozen bound.
   */
  void u8(std::uint8_t value) { raw(&value, sizeof(value)); }

  /**
   * @brief Appends one canonical uint32.
   * @param value Scalar value.
   * @throws std::length_error when metadata exceeds the frozen bound.
   */
  void u32(std::uint32_t value) {
    std::array<std::uint8_t, 4U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::uint8_t>(value >> (8U * index));
    }
    raw(bytes.data(), bytes.size());
  }

  /**
   * @brief Appends one canonical uint64.
   * @param value Scalar value.
   * @throws std::length_error when metadata exceeds the frozen bound.
   */
  void u64(std::uint64_t value) {
    std::array<std::uint8_t, 8U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::uint8_t>(value >> (8U * index));
    }
    raw(bytes.data(), bytes.size());
  }

  /**
   * @brief Appends one canonical signed 64-bit scalar.
   * @param value Scalar value.
   * @throws std::length_error when metadata exceeds the frozen bound.
   */
  void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

  /**
   * @brief Appends one canonical numeric binary32 word.
   * @param value Finite validated scalar.
   * @throws std::invalid_argument when the finite-value invariant is broken.
   * @throws std::length_error when metadata exceeds the frozen bound.
   * @note Signed zero is canonicalized to positive zero. Native object byte
   *       order and floating word order are never inspected.
   */
  void f32(float value) { u32(internal::canonical_binary32_bits(value)); }

  /**
   * @brief Appends one canonical numeric binary64 word.
   * @param value Finite validated scalar.
   * @throws std::invalid_argument when the finite-value invariant is broken.
   * @throws std::length_error when metadata exceeds the frozen bound.
   * @note Signed zero is canonicalized to positive zero. Native object byte
   *       order and floating word order are never inspected.
   */
  void f64(double value) { u64(internal::canonical_binary64_bits(value)); }

  /**
   * @brief Appends a uint32-length-prefixed byte vector.
   * @param bytes Exact owned bytes.
   * @throws std::overflow_error when length exceeds uint32.
   * @throws std::length_error when metadata exceeds the frozen bound.
   */
  void bytes(const std::vector<std::byte>& bytes) {
    u32(size_to_u32(bytes.size()));
    raw(bytes.data(), bytes.size());
  }

  /**
   * @brief Appends a uint32-length-prefixed diagnostic string.
   * @param value Exact string bytes.
   * @throws std::length_error when string or metadata exceeds frozen bounds.
   */
  void string(const std::string& value) {
    if (value.size() > kMaximumValueArtifactStringBytes) {
      throw std::length_error("Value artifact string exceeds its bound.");
    }
    u32(size_to_u32(value.size()));
    raw(value.data(), value.size());
  }

  /**
   * @brief Appends one exact borrowed byte range.
   * @param data Range start, null only for zero size.
   * @param size Byte count.
   * @throws std::invalid_argument for null nonempty input.
   * @throws std::length_error when metadata exceeds the frozen bound.
   * @throws std::bad_alloc when output growth cannot allocate.
   */
  void raw(const void* data, std::size_t size) {
    if (size != 0U && data == nullptr) {
      invalid_artifact("Value artifact writer received null bytes.");
    }
    if (size > kMaximumValueArtifactMetadataBytes - output_.size()) {
      throw std::length_error("Value artifact metadata exceeds its bound.");
    }
    if (size == 0U) {
      return;
    }
    const auto* begin = static_cast<const std::byte*>(data);
    output_.insert(output_.end(), begin, begin + size);
  }

  /**
   * @brief Releases the complete encoded bytes.
   * @return Owned canonical metadata.
   * @throws Nothing under vector move.
   */
  std::vector<std::byte> finish() && { return std::move(output_); }

 private:
  /** @brief Complete canonical output accumulated so far. */
  std::vector<std::byte> output_;
};

/**
 * @brief Transactional bounded canonical metadata reader.
 * @throws Nothing for construction.
 * @note Every read checks complete availability before advancing the cursor.
 */
class Reader final {
 public:
  /**
   * @brief Borrows one complete metadata byte vector.
   * @param bytes Bytes retained by the caller for this reader lifetime.
   * @throws std::length_error when bytes exceed the frozen bound.
   */
  explicit Reader(const std::vector<std::byte>& bytes) : bytes_(bytes) {
    if (bytes.size() > kMaximumValueArtifactMetadataBytes) {
      throw std::length_error("Value artifact metadata exceeds its bound.");
    }
  }

  /** @brief Reads one byte. @return Exact value. */
  std::uint8_t u8() {
    std::uint8_t value = 0U;
    raw(&value, sizeof(value));
    return value;
  }

  /** @brief Reads one canonical uint32. @return Exact value. */
  std::uint32_t u32() {
    std::array<std::uint8_t, 4U> bytes{};
    raw(bytes.data(), bytes.size());
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      value |= static_cast<std::uint32_t>(bytes[index]) << (8U * index);
    }
    return value;
  }

  /** @brief Reads one canonical uint64. @return Exact value. */
  std::uint64_t u64() {
    std::array<std::uint8_t, 8U> bytes{};
    raw(bytes.data(), bytes.size());
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
    }
    return value;
  }

  /** @brief Reads one canonical signed scalar. @return Exact value. */
  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

  /**
   * @brief Reads one canonical finite numeric binary32 word.
   * @return Exact native scalar with positive canonical zero.
   * @throws std::invalid_argument for nonfinite or negative-zero spellings.
   */
  float f32() { return internal::decode_canonical_binary32(u32()); }

  /**
   * @brief Reads one canonical finite numeric binary64 word.
   * @return Exact native scalar with positive canonical zero.
   * @throws std::invalid_argument for nonfinite or negative-zero spellings.
   */
  double f64() { return internal::decode_canonical_binary64(u64()); }

  /**
   * @brief Reads one bounded length-prefixed byte vector.
   * @return Owned bytes.
   * @throws std::length_error for excessive field length.
   */
  std::vector<std::byte> bytes() {
    const std::size_t size = u32();
    if (size > kMaximumExtensionMetadataBytes) {
      throw std::length_error("Value artifact byte field exceeds its bound.");
    }
    std::vector<std::byte> result(size);
    raw(result.data(), result.size());
    return result;
  }

  /**
   * @brief Reads one bounded length-prefixed string.
   * @return Owned exact string bytes.
   * @throws std::length_error for excessive field length.
   */
  std::string string() {
    const std::size_t size = u32();
    if (size > kMaximumValueArtifactStringBytes) {
      throw std::length_error("Value artifact string exceeds its bound.");
    }
    std::string result(size, '\0');
    raw(result.data(), result.size());
    return result;
  }

  /**
   * @brief Reads exact bytes into caller storage.
   * @param output Destination, null only for zero size.
   * @param size Exact requested count.
   * @throws std::invalid_argument for null nonempty output or truncation.
   */
  void raw(void* output, std::size_t size) {
    if (size != 0U && output == nullptr) {
      invalid_artifact("Value artifact reader received null storage.");
    }
    if (size > bytes_.size() - offset_) {
      invalid_artifact("Value artifact metadata is truncated.");
    }
    if (size != 0U) {
      std::memcpy(output, bytes_.data() + offset_, size);
    }
    offset_ += size;
  }

  /**
   * @brief Requires complete input consumption.
   * @throws std::invalid_argument when trailing bytes remain.
   */
  void finish() const {
    if (offset_ != bytes_.size()) {
      invalid_artifact("Value artifact metadata has trailing bytes.");
    }
  }

 private:
  /** @brief Borrowed complete metadata bytes. */
  const std::vector<std::byte>& bytes_;
  /** @brief Next unread byte offset. */
  std::size_t offset_ = 0U;
};

/**
 * @brief Computes exact SHA-256 over one payload byte vector.
 * @param payload Complete immutable bytes.
 * @return Exact digest.
 * @throws std::runtime_error when OpenSSL digest setup/update/final fails.
 * @throws std::bad_alloc when OpenSSL context allocation fails.
 */
ArtifactPayloadDigest payload_digest(const std::vector<std::byte>& payload) {
  using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  Context context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context) {
    throw std::bad_alloc();
  }
  ArtifactPayloadDigest digest;
  unsigned int size = 0U;
  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), payload.data(), payload.size()) != 1 ||
      EVP_DigestFinal_ex(context.get(),
                         reinterpret_cast<unsigned char*>(digest.bytes.data()),
                         &size) != 1 ||
      size != digest.bytes.size()) {
    throw std::runtime_error("SHA-256 payload digest failed.");
  }
  return digest;
}

/**
 * @brief Converts unavailable canonical content into a typed artifact failure.
 * @param result Explicit content-digest result.
 * @return Available digest.
 * @throws ExtensionContractError when canonical content is unavailable.
 */
ContentDigest require_content_digest(const ContentDigestResult& result) {
  if (result.state == ContentDigestState::Available &&
      result.digest.has_value()) {
    return *result.digest;
  }
  ExtensionErrorCode code = ExtensionErrorCode::ProviderRejected;
  switch (result.state) {
    case ContentDigestState::MissingProvider:
      code = ExtensionErrorCode::MissingProvider;
      break;
    case ContentDigestState::UnsupportedSchemaVersion:
      code = ExtensionErrorCode::UnsupportedSchemaVersion;
      break;
    case ContentDigestState::PayloadUnavailable:
      code = ExtensionErrorCode::PayloadUnavailable;
      break;
    case ContentDigestState::InvalidDescriptor:
      code = ExtensionErrorCode::InvalidEnvelope;
      break;
    case ContentDigestState::ProviderFailure:
    case ContentDigestState::Available:
      code = ExtensionErrorCode::ProviderRejected;
      break;
  }
  throw ExtensionContractError(
      code, result.diagnostic.empty()
                ? "Canonical Value content digest is unavailable."
                : result.diagnostic);
}

/**
 * @brief Returns a stable representative role for one provider buffer.
 * @param layout Valid provider-defined Layout.
 * @param buffer_index Dense buffer index.
 * @return Lowest referenced nonzero logical role, or zero when unreferenced.
 * @throws Nothing.
 * @note Complete role/range identity remains in ProviderDefinedLayout; this
 *       field is a convenient per-file classification only.
 */
std::uint32_t provider_buffer_role(const ProviderDefinedLayout& layout,
                                   std::uint32_t buffer_index) noexcept {
  std::uint32_t role = 0U;
  for (const BufferEnvelope& envelope : layout.buffers) {
    if (envelope.buffer_index == buffer_index &&
        (role == 0U || envelope.logical_role < role)) {
      role = envelope.logical_role;
    }
  }
  return role;
}

/**
 * @brief Computes the canonical descriptor digest retained by an envelope.
 * @param envelope Complete representation metadata.
 * @return Exact canonical identity.
 * @throws Artifact or extension validation and digest failures unchanged.
 */
DescriptorDigest canonical_descriptor_digest(
    const ValueArtifactEnvelope& envelope);

/**
 * @brief Computes the canonical storage-Layout digest retained by an envelope.
 * @param envelope Complete representation metadata.
 * @return Exact canonical identity.
 * @throws Artifact or extension validation and digest failures unchanged.
 */
StorageLayoutDigest canonical_layout_digest(
    const ValueArtifactEnvelope& envelope);

/**
 * @brief Validates a bounded nonempty portable identity string.
 * @param value Candidate exact bytes.
 * @param label Stable field label used in diagnostics.
 * @throws std::invalid_argument or std::length_error for malformed text.
 * @note UTF-8 well-formedness remains the owning protocol's responsibility;
 *       embedded NUL is forbidden at every portable boundary.
 */
void validate_artifact_text(const std::string& value, const char* label) {
  if (value.empty() || value.find('\0') != std::string::npos) {
    throw std::invalid_argument(std::string(label) +
                                " must be nonempty and contain no NUL.");
  }
  if (value.size() > kMaximumValueArtifactStringBytes) {
    throw std::length_error(std::string(label) + " exceeds its bound.");
  }
}

/**
 * @brief Validates an optional owner-supplied artifact join.
 * @param value Optional bounded identity.
 * @param label Stable field label.
 * @throws std::invalid_argument or std::length_error for malformed text.
 */
void validate_optional_artifact_text(const std::optional<std::string>& value,
                                     const char* label) {
  if (value.has_value()) {
    validate_artifact_text(*value, label);
  }
}

/**
 * @brief Validates exact envelope shape without reading payload bytes.
 * @param envelope Candidate portable metadata.
 * @return Nothing.
 * @throws std::invalid_argument, std::overflow_error, std::length_error, or
 *         ExtensionContractError for malformed facts.
 */
void validate_envelope(const ValueArtifactEnvelope& envelope) {
  if (envelope.structural_version != kValueArtifactEnvelopeVersion) {
    invalid_artifact("Unsupported Value artifact envelope version.");
  }
  if (envelope.output_name.empty() ||
      envelope.output_name.size() > kMaximumValueArtifactNameBytes ||
      envelope.output_name.find('\0') != std::string::npos) {
    invalid_artifact("Value artifact output name is invalid.");
  }
  validate_optional_artifact_text(envelope.joins.artifact_identity,
                                  "artifact identity");
  validate_optional_artifact_text(envelope.joins.commit_identity,
                                  "commit identity");
  validate_optional_artifact_text(envelope.joins.slot_identity,
                                  "slot identity");
  if (envelope.statistics_references.size() >
      kMaximumValueArtifactStatisticsReferences) {
    throw std::length_error(
        "Value artifact statistics-reference count exceeds its bound.");
  }
  for (const ValueArtifactStatisticsReference& reference :
       envelope.statistics_references) {
    if (!envelope.content_digest.has_value() ||
        reference.content_digest.algorithm !=
            CanonicalDigestAlgorithm::Sha256CanonicalV1 ||
        reference.algorithm_version == 0U) {
      invalid_artifact("Value artifact statistics reference is malformed.");
    }
    validate_artifact_text(reference.algorithm, "statistics algorithm");
    validate_artifact_text(reference.artifact_identity,
                           "statistics artifact identity");
    if (!(reference.content_digest == *envelope.content_digest)) {
      invalid_artifact(
          "Value artifact statistics reference has a foreign content digest.");
    }
  }
  if (envelope.content_digest.has_value() &&
      envelope.content_digest->algorithm !=
          CanonicalDigestAlgorithm::Sha256CanonicalV1) {
    invalid_artifact("Unsupported Value artifact content digest algorithm.");
  }
  if (envelope.descriptor_digest.algorithm !=
          CanonicalDigestAlgorithm::Sha256CanonicalV1 ||
      envelope.storage_layout_digest.algorithm !=
          CanonicalDigestAlgorithm::Sha256CanonicalV1) {
    invalid_artifact("Unsupported Value artifact metadata digest algorithm.");
  }
  if (envelope.buffers.empty() ||
      envelope.buffers.size() > kMaximumExtensionBuffers) {
    invalid_artifact("Value artifact buffer count is outside bounds.");
  }
  std::uint64_t aggregate = 0U;
  std::uint64_t previous_end = 0U;
  std::vector<std::uint32_t> roles;
  roles.reserve(envelope.buffers.size());
  for (std::size_t index = 0U; index < envelope.buffers.size(); ++index) {
    const ValueArtifactBuffer& buffer = envelope.buffers[index];
    if (buffer.index != index || buffer.byte_size == 0U ||
        buffer.required_alignment == 0U ||
        buffer.required_alignment > kMaximumValueArtifactAlignment ||
        (buffer.required_alignment & (buffer.required_alignment - 1U)) != 0U ||
        buffer.artifact_offset % buffer.required_alignment != 0U ||
        buffer.artifact_offset < previous_end) {
      invalid_artifact("Value artifact buffer table is not canonical.");
    }
    previous_end = checked_u64_add(buffer.artifact_offset, buffer.byte_size);
    aggregate = checked_u64_add(aggregate, buffer.byte_size);
    if (aggregate > kMaximumValueArtifactPayloadBytes) {
      throw std::length_error("Value artifact payload exceeds its bound.");
    }
    if (buffer.logical_role != 0U) {
      if (std::find(roles.begin(), roles.end(), buffer.logical_role) !=
          roles.end()) {
        invalid_artifact("Value artifact buffer roles are not unique.");
      }
      roles.push_back(buffer.logical_role);
    }
  }

  if (envelope.representation == ValueRepresentationKind::DenseTensor) {
    if (!envelope.dense_descriptor.has_value() ||
        envelope.provider_descriptor.has_value() ||
        envelope.provider_layout.has_value() || envelope.buffers.size() != 1U ||
        envelope.buffers.front().logical_role != 0U) {
      invalid_artifact("DenseTensor artifact metadata is inconsistent.");
    }
    const DenseTensorDescriptor& descriptor = *envelope.dense_descriptor;
    if (descriptor.shape.empty() ||
        descriptor.shape.size() > kMaximumArtifactRank) {
      invalid_artifact("DenseTensor artifact rank is outside bounds.");
    }
    validate_dense_tensor_image_metadata(descriptor, envelope.image_facet);
    if (envelope.layout_kind == StorageLayoutKind::Strided) {
      if (!envelope.strided_layout.has_value() ||
          envelope.blocked_layout.has_value()) {
        invalid_artifact("Strided artifact Layout fields are inconsistent.");
      }
      (void)dense_tensor_element_bytes(descriptor);
      if (envelope.strided_layout->byte_strides.size() !=
          descriptor.shape.size()) {
        invalid_artifact("Strided artifact rank differs from descriptor.");
      }
    } else if (envelope.layout_kind == StorageLayoutKind::Blocked) {
      if (!envelope.blocked_layout.has_value() ||
          envelope.strided_layout.has_value() ||
          envelope.image_facet.has_value()) {
        invalid_artifact("Blocked artifact Layout fields are inconsistent.");
      }
    } else {
      invalid_artifact("DenseTensor artifact has an unknown Layout kind.");
    }
    if (!(envelope.descriptor_digest ==
          canonical_descriptor_digest(envelope)) ||
        !(envelope.storage_layout_digest ==
          canonical_layout_digest(envelope))) {
      invalid_artifact("DenseTensor artifact metadata digest disagrees.");
    }
    return;
  }

  if (envelope.representation != ValueRepresentationKind::ProviderDefined ||
      envelope.layout_kind != StorageLayoutKind::ProviderDefined ||
      envelope.dense_descriptor.has_value() ||
      envelope.image_facet.has_value() || envelope.strided_layout.has_value() ||
      envelope.blocked_layout.has_value() ||
      !envelope.provider_descriptor.has_value() ||
      !envelope.provider_layout.has_value()) {
    invalid_artifact("Provider-defined artifact metadata is inconsistent.");
  }
  validate_data_descriptor_envelope(*envelope.provider_descriptor);
  std::vector<std::size_t> sizes;
  sizes.reserve(envelope.buffers.size());
  for (const ValueArtifactBuffer& buffer : envelope.buffers) {
    sizes.push_back(u64_to_size(buffer.byte_size));
  }
  validate_provider_defined_layout(*envelope.provider_layout, sizes);
  for (const ValueArtifactBuffer& buffer : envelope.buffers) {
    if (buffer.logical_role !=
        provider_buffer_role(*envelope.provider_layout, buffer.index)) {
      invalid_artifact("Provider artifact buffer role is inconsistent.");
    }
  }
  if (!(envelope.descriptor_digest == canonical_descriptor_digest(envelope)) ||
      !(envelope.storage_layout_digest == canonical_layout_digest(envelope))) {
    invalid_artifact("Provider artifact metadata digest disagrees.");
  }
}

/**
 * @brief Encodes one ImageBounds record.
 * @param writer Destination writer.
 * @param bounds Exact signed half-open bounds.
 */
void encode_bounds(Writer* writer, const ImageBounds& bounds) {
  writer->i64(bounds.x_begin);
  writer->i64(bounds.y_begin);
  writer->i64(bounds.x_end);
  writer->i64(bounds.y_end);
}

/**
 * @brief Decodes one ImageBounds record.
 * @param reader Source reader.
 * @return Exact signed half-open bounds.
 */
ImageBounds decode_bounds(Reader* reader) {
  return ImageBounds{reader->i64(), reader->i64(), reader->i64(),
                     reader->i64()};
}

/**
 * @brief Encodes one complete DenseTensor descriptor.
 * @param writer Destination writer.
 * @param descriptor Valid descriptor.
 */
void encode_dense_descriptor(Writer* writer,
                             const DenseTensorDescriptor& descriptor) {
  writer->u32(size_to_u32(descriptor.shape.size()));
  for (std::size_t extent : descriptor.shape) {
    writer->u64(size_to_u64(extent));
  }
  writer->u32(static_cast<std::uint32_t>(descriptor.element_semantics));
  writer->u32(static_cast<std::uint32_t>(descriptor.storage_encoding.kind));
  writer->u32(descriptor.storage_encoding.bit_width);
  writer->u8(descriptor.quantization.has_value() ? 1U : 0U);
  if (descriptor.quantization.has_value()) {
    writer->u32(size_to_u32(descriptor.quantization->block_shape.size()));
    for (std::size_t extent : descriptor.quantization->block_shape) {
      writer->u64(size_to_u64(extent));
    }
    writer->u32(size_to_u32(descriptor.quantization->scales.size()));
    for (float scale : descriptor.quantization->scales) {
      writer->f32(scale);
    }
  }
}

/**
 * @brief Decodes one bounded DenseTensor descriptor.
 * @param reader Source reader.
 * @return Fully owned descriptor.
 */
DenseTensorDescriptor decode_dense_descriptor(Reader* reader) {
  DenseTensorDescriptor descriptor;
  const std::size_t rank = reader->u32();
  if (rank == 0U || rank > kMaximumArtifactRank) {
    invalid_artifact("DenseTensor artifact rank is outside bounds.");
  }
  descriptor.shape.reserve(rank);
  for (std::size_t index = 0U; index < rank; ++index) {
    descriptor.shape.push_back(u64_to_size(reader->u64()));
  }
  descriptor.element_semantics = static_cast<ElementSemantics>(reader->u32());
  descriptor.storage_encoding.kind =
      static_cast<StorageEncodingKind>(reader->u32());
  descriptor.storage_encoding.bit_width = reader->u32();
  const std::uint8_t quantized = reader->u8();
  if (quantized > 1U) {
    invalid_artifact("DenseTensor quantization flag is invalid.");
  }
  if (quantized != 0U) {
    QuantizationSchema quantization;
    const std::size_t block_rank = reader->u32();
    if (block_rank == 0U || block_rank > kMaximumArtifactRank) {
      invalid_artifact("DenseTensor quantization rank is outside bounds.");
    }
    quantization.block_shape.reserve(block_rank);
    for (std::size_t index = 0U; index < block_rank; ++index) {
      quantization.block_shape.push_back(u64_to_size(reader->u64()));
    }
    const std::size_t scale_count = reader->u32();
    if (scale_count > kMaximumExtensionMetadataBytes / sizeof(float)) {
      throw std::length_error("DenseTensor scale count exceeds its bound.");
    }
    quantization.scales.reserve(scale_count);
    for (std::size_t index = 0U; index < scale_count; ++index) {
      quantization.scales.push_back(reader->f32());
    }
    descriptor.quantization = std::move(quantization);
  }
  return descriptor;
}

/**
 * @brief Encodes one complete ImageFacet.
 * @param writer Destination writer.
 * @param facet Valid complete ordinary-image metadata.
 */
void encode_image_facet(Writer* writer, const ImageFacet& facet) {
  writer->u64(size_to_u64(facet.x_axis));
  writer->u64(size_to_u64(facet.y_axis));
  writer->u8(facet.channel_axis.has_value() ? 1U : 0U);
  if (facet.channel_axis.has_value()) {
    writer->u64(size_to_u64(*facet.channel_axis));
  }
  encode_bounds(writer, facet.data_window);
  writer->u8(facet.display_window.has_value() ? 1U : 0U);
  if (facet.display_window.has_value()) {
    encode_bounds(writer, *facet.display_window);
  }
  writer->u8(facet.channel_schema.has_value() ? 1U : 0U);
  if (facet.channel_schema.has_value()) {
    writer->u32(size_to_u32(facet.channel_schema->channels.size()));
    for (const ChannelDescription& channel : facet.channel_schema->channels) {
      writer->u64(channel.id.value);
      writer->string(channel.diagnostic_name);
    }
    writer->u32(size_to_u32(facet.channel_schema->groups.size()));
    for (const ChannelGroupDescription& group : facet.channel_schema->groups) {
      writer->u64(group.id.value);
      writer->string(group.diagnostic_name);
      writer->u32(size_to_u32(group.members.size()));
      for (ChannelId member : group.members) {
        writer->u64(member.value);
      }
    }
  }
  writer->u8(facet.sample_domain.has_value() ? 1U : 0U);
  if (facet.sample_domain.has_value()) {
    const SampleDomainFacet& sample = *facet.sample_domain;
    writer->u32(sample.structural_version);
    writer->u32(sample.encoding.structural_version);
    writer->u32(static_cast<std::uint32_t>(sample.encoding.kind));
    writer->u32(static_cast<std::uint32_t>(sample.default_domain.kind));
    writer->f64(sample.default_domain.minimum);
    writer->f64(sample.default_domain.maximum);
    writer->u32(size_to_u32(sample.per_channel.size()));
    for (const ChannelSampleDomain& channel : sample.per_channel) {
      writer->u64(channel.channel.value);
      writer->u32(static_cast<std::uint32_t>(channel.domain.kind));
      writer->f64(channel.domain.minimum);
      writer->f64(channel.domain.maximum);
    }
  }
  writer->u8(facet.color.has_value() ? 1U : 0U);
  if (facet.color.has_value()) {
    writer->u32(facet.color->structural_version);
    writer->u64(facet.color->channel_group.value);
    writer->u32(static_cast<std::uint32_t>(facet.color->transfer));
    writer->u32(static_cast<std::uint32_t>(facet.color->primaries));
  }
}

/**
 * @brief Decodes one bounded complete ImageFacet.
 * @param reader Source reader.
 * @return Fully owned ordinary-image metadata.
 */
ImageFacet decode_image_facet(Reader* reader) {
  ImageFacet facet;
  facet.x_axis = u64_to_size(reader->u64());
  facet.y_axis = u64_to_size(reader->u64());
  const std::uint8_t channel_axis = reader->u8();
  if (channel_axis > 1U) {
    invalid_artifact("ImageFacet channel-axis flag is invalid.");
  }
  if (channel_axis != 0U) {
    facet.channel_axis = u64_to_size(reader->u64());
  }
  facet.data_window = decode_bounds(reader);
  const std::uint8_t display = reader->u8();
  if (display > 1U) {
    invalid_artifact("ImageFacet display-window flag is invalid.");
  }
  if (display != 0U) {
    facet.display_window = decode_bounds(reader);
  }
  const std::uint8_t schema = reader->u8();
  if (schema > 1U) {
    invalid_artifact("ImageFacet channel-schema flag is invalid.");
  }
  if (schema != 0U) {
    ChannelSchema channel_schema;
    const std::size_t channel_count = reader->u32();
    if (channel_count > kMaximumImageChannels) {
      throw std::length_error("ImageFacet channel count exceeds its bound.");
    }
    channel_schema.channels.reserve(channel_count);
    for (std::size_t index = 0U; index < channel_count; ++index) {
      channel_schema.channels.push_back(
          ChannelDescription{ChannelId{reader->u64()}, reader->string()});
    }
    const std::size_t group_count = reader->u32();
    if (group_count > kMaximumImageChannelGroups) {
      throw std::length_error("ImageFacet group count exceeds its bound.");
    }
    channel_schema.groups.reserve(group_count);
    for (std::size_t index = 0U; index < group_count; ++index) {
      ChannelGroupDescription group;
      group.id = ChannelGroupId{reader->u64()};
      group.diagnostic_name = reader->string();
      const std::size_t member_count = reader->u32();
      if (member_count > kMaximumImageChannelGroupMembers) {
        throw std::length_error(
            "ImageFacet group membership exceeds its bound.");
      }
      group.members.reserve(member_count);
      for (std::size_t member = 0U; member < member_count; ++member) {
        group.members.push_back(ChannelId{reader->u64()});
      }
      channel_schema.groups.push_back(std::move(group));
    }
    facet.channel_schema = std::move(channel_schema);
  }
  const std::uint8_t sample = reader->u8();
  if (sample > 1U) {
    invalid_artifact("ImageFacet sample-domain flag is invalid.");
  }
  if (sample != 0U) {
    SampleDomainFacet sample_domain;
    sample_domain.structural_version = reader->u32();
    sample_domain.encoding.structural_version = reader->u32();
    sample_domain.encoding.kind =
        static_cast<SampleEncodingKind>(reader->u32());
    sample_domain.default_domain.kind =
        static_cast<SampleDomainKind>(reader->u32());
    sample_domain.default_domain.minimum = reader->f64();
    sample_domain.default_domain.maximum = reader->f64();
    const std::size_t override_count = reader->u32();
    if (override_count > kMaximumImageChannels) {
      throw std::length_error(
          "ImageFacet sample override count exceeds its bound.");
    }
    sample_domain.per_channel.reserve(override_count);
    for (std::size_t index = 0U; index < override_count; ++index) {
      ChannelSampleDomain channel;
      channel.channel = ChannelId{reader->u64()};
      channel.domain.kind = static_cast<SampleDomainKind>(reader->u32());
      channel.domain.minimum = reader->f64();
      channel.domain.maximum = reader->f64();
      sample_domain.per_channel.push_back(channel);
    }
    facet.sample_domain = std::move(sample_domain);
  }
  const std::uint8_t color = reader->u8();
  if (color > 1U) {
    invalid_artifact("ImageFacet color flag is invalid.");
  }
  if (color != 0U) {
    ColorFacet color_facet;
    color_facet.structural_version = reader->u32();
    color_facet.channel_group = ChannelGroupId{reader->u64()};
    color_facet.transfer = static_cast<ColorTransferFunction>(reader->u32());
    color_facet.primaries = static_cast<ColorPrimaries>(reader->u32());
    facet.color = color_facet;
  }
  return facet;
}

/**
 * @brief Encodes one Strided Layout.
 * @param writer Destination writer.
 * @param layout Exact Layout.
 */
void encode_strided_layout(Writer* writer, const StridedLayout& layout) {
  writer->u32(size_to_u32(layout.byte_strides.size()));
  for (std::ptrdiff_t stride : layout.byte_strides) {
    static_assert(sizeof(std::ptrdiff_t) <= sizeof(std::int64_t),
                  "artifact stride requires at most 64-bit ptrdiff_t");
    writer->i64(static_cast<std::int64_t>(stride));
  }
  writer->u64(size_to_u64(layout.byte_offset));
}

/**
 * @brief Decodes one bounded Strided Layout.
 * @param reader Source reader.
 * @return Fully owned Layout.
 */
StridedLayout decode_strided_layout(Reader* reader) {
  StridedLayout layout;
  const std::size_t rank = reader->u32();
  if (rank == 0U || rank > kMaximumArtifactRank) {
    invalid_artifact("Strided artifact rank is outside bounds.");
  }
  layout.byte_strides.reserve(rank);
  for (std::size_t index = 0U; index < rank; ++index) {
    const std::int64_t stride = reader->i64();
    if (stride < std::numeric_limits<std::ptrdiff_t>::min() ||
        stride > std::numeric_limits<std::ptrdiff_t>::max()) {
      throw std::overflow_error("Artifact stride exceeds ptrdiff_t.");
    }
    layout.byte_strides.push_back(static_cast<std::ptrdiff_t>(stride));
  }
  layout.byte_offset = u64_to_size(reader->u64());
  return layout;
}

/**
 * @brief Encodes one Blocked Layout.
 * @param writer Destination writer.
 * @param layout Exact Layout.
 */
void encode_blocked_layout(Writer* writer, const BlockedLayout& layout) {
  writer->u32(layout.version);
  writer->u32(size_to_u32(layout.block_shape.size()));
  for (std::size_t extent : layout.block_shape) {
    writer->u64(size_to_u64(extent));
  }
  writer->u32(size_to_u32(layout.block_bit_strides.size()));
  for (std::size_t stride : layout.block_bit_strides) {
    writer->u64(size_to_u64(stride));
  }
  writer->u64(size_to_u64(layout.bit_offset));
  writer->u32(static_cast<std::uint32_t>(layout.bit_order));
}

/**
 * @brief Decodes one bounded Blocked Layout.
 * @param reader Source reader.
 * @return Fully owned Layout.
 */
BlockedLayout decode_blocked_layout(Reader* reader) {
  BlockedLayout layout;
  layout.version = reader->u32();
  const std::size_t shape_count = reader->u32();
  if (shape_count == 0U || shape_count > kMaximumArtifactRank) {
    invalid_artifact("Blocked artifact shape rank is outside bounds.");
  }
  layout.block_shape.reserve(shape_count);
  for (std::size_t index = 0U; index < shape_count; ++index) {
    layout.block_shape.push_back(u64_to_size(reader->u64()));
  }
  const std::size_t stride_count = reader->u32();
  if (stride_count == 0U || stride_count > kMaximumArtifactRank) {
    invalid_artifact("Blocked artifact stride rank is outside bounds.");
  }
  layout.block_bit_strides.reserve(stride_count);
  for (std::size_t index = 0U; index < stride_count; ++index) {
    layout.block_bit_strides.push_back(u64_to_size(reader->u64()));
  }
  layout.bit_offset = u64_to_size(reader->u64());
  layout.bit_order = static_cast<PackedBitOrder>(reader->u32());
  return layout;
}

/** @copydoc canonical_descriptor_digest */
DescriptorDigest canonical_descriptor_digest(
    const ValueArtifactEnvelope& envelope) {
  if (envelope.representation == ValueRepresentationKind::ProviderDefined) {
    return compute_descriptor_digest(*envelope.provider_descriptor);
  }
  Writer writer;
  constexpr std::array<std::byte, 8U> domain{
      std::byte{'P'}, std::byte{'S'}, std::byte{'D'}, std::byte{'E'},
      std::byte{'S'}, std::byte{'C'}, std::byte{1U},  std::byte{0U}};
  writer.raw(domain.data(), domain.size());
  encode_dense_descriptor(&writer, *envelope.dense_descriptor);
  writer.u8(envelope.image_facet.has_value() ? 1U : 0U);
  if (envelope.image_facet.has_value()) {
    encode_image_facet(&writer, *envelope.image_facet);
  }
  const ArtifactPayloadDigest raw = payload_digest(std::move(writer).finish());
  DescriptorDigest result;
  result.bytes = raw.bytes;
  return result;
}

/** @copydoc canonical_layout_digest */
StorageLayoutDigest canonical_layout_digest(
    const ValueArtifactEnvelope& envelope) {
  if (envelope.representation == ValueRepresentationKind::ProviderDefined) {
    return compute_storage_layout_digest(*envelope.provider_layout);
  }
  Writer writer;
  constexpr std::array<std::byte, 8U> domain{
      std::byte{'P'}, std::byte{'S'}, std::byte{'L'}, std::byte{'A'},
      std::byte{'Y'}, std::byte{'O'}, std::byte{1U},  std::byte{0U}};
  writer.raw(domain.data(), domain.size());
  writer.u32(static_cast<std::uint32_t>(envelope.layout_kind));
  if (envelope.layout_kind == StorageLayoutKind::Strided) {
    encode_strided_layout(&writer, *envelope.strided_layout);
  } else {
    encode_blocked_layout(&writer, *envelope.blocked_layout);
  }
  const ArtifactPayloadDigest raw = payload_digest(std::move(writer).finish());
  StorageLayoutDigest result;
  result.bytes = raw.bytes;
  return result;
}

/**
 * @brief Reconstructs a built-in artifact after exact byte validation.
 * @param artifact Validated shape and payload metadata.
 * @return Fresh local built-in Value.
 * @throws Value publication failures unchanged.
 */
Value reconstruct_builtin(const ValueArtifact& artifact) {
  const ValueArtifactEnvelope& envelope = artifact.envelope;
  const std::size_t required_alignment =
      u64_to_size(envelope.buffers.front().required_alignment);
  if (envelope.layout_kind == StorageLayoutKind::Strided) {
    return Value::from_cpu_dense_tensor(
        *envelope.dense_descriptor, envelope.image_facet,
        *envelope.strided_layout, artifact.payloads.front(),
        required_alignment);
  }
  return Value::from_cpu_blocked_dense_tensor(
      *envelope.dense_descriptor, *envelope.blocked_layout,
      artifact.payloads.front(), required_alignment);
}

/**
 * @brief Verifies optional canonical logical content after local publication.
 * @param value Fresh validated local Value.
 * @param expected Optional portable content identity.
 * @throws ExtensionContractError when digest computation is unavailable.
 * @throws std::invalid_argument when the digest disagrees.
 */
void validate_content(const Value& value,
                      const std::optional<ContentDigest>& expected) {
  if (!expected.has_value()) {
    return;
  }
  const ContentDigest actual =
      require_content_digest(compute_content_digest(value));
  if (!(actual == *expected)) {
    invalid_artifact("Value artifact canonical content digest disagrees.");
  }
}

/**
 * @brief Rounds one portable offset up to a positive power-of-two alignment.
 * @param value Current absolute offset.
 * @param alignment Positive power-of-two alignment.
 * @return Smallest aligned offset not below value.
 * @throws std::overflow_error when padding cannot be represented.
 */
std::uint64_t align_artifact_offset(std::uint64_t value,
                                    std::uint64_t alignment) {
  const std::uint64_t mask = alignment - 1U;
  if (value > std::numeric_limits<std::uint64_t>::max() - mask) {
    throw std::overflow_error("Value artifact alignment overflowed.");
  }
  return (value + mask) & ~mask;
}

/**
 * @brief Appends exact bytes to an in-memory artifact-set archive.
 * @param output Destination archive.
 * @param data Borrowed bytes, null only for zero size.
 * @param size Exact byte count.
 * @throws std::invalid_argument for null nonempty data.
 * @throws std::overflow_error when local archive size would wrap.
 * @throws std::bad_alloc when output storage cannot grow.
 */
void append_archive_bytes(std::vector<std::byte>* output, const void* data,
                          std::size_t size) {
  if (size != 0U && data == nullptr) {
    invalid_artifact("Value artifact archive received null bytes.");
  }
  if (size > std::numeric_limits<std::size_t>::max() - output->size()) {
    throw std::overflow_error("Value artifact archive size overflowed.");
  }
  if (size != 0U) {
    const auto* begin = static_cast<const std::byte*>(data);
    output->insert(output->end(), begin, begin + size);
  }
}

/**
 * @brief Appends one canonical little-endian uint32 to an archive.
 * @param output Destination archive.
 * @param value Exact scalar.
 * @throws std::bad_alloc when output storage cannot grow.
 */
void append_archive_u32(std::vector<std::byte>* output, std::uint32_t value) {
  std::array<std::byte, 4U> bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = std::byte{static_cast<std::uint8_t>(value >> (8U * index))};
  }
  append_archive_bytes(output, bytes.data(), bytes.size());
}

/**
 * @brief Bounded cursor for a canonical named-artifact-set archive.
 * @throws Nothing for construction; reads throw for truncation.
 */
class ArtifactSetReader final {
 public:
  /**
   * @brief Borrows one complete archive.
   * @param bytes Stable byte vector retained for this reader lifetime.
   * @throws Nothing.
   */
  explicit ArtifactSetReader(const std::vector<std::byte>& bytes) noexcept
      : bytes_(bytes) {}

  /**
   * @brief Reads exact bytes and advances.
   * @param output Destination, null only for zero size.
   * @param size Requested byte count.
   * @throws std::invalid_argument for null output or truncation.
   */
  void raw(void* output, std::size_t size) {
    if (size != 0U && output == nullptr) {
      invalid_artifact("Value artifact archive reader received null storage.");
    }
    if (size > bytes_.size() - offset_) {
      invalid_artifact("Value artifact set archive is truncated.");
    }
    if (size != 0U) {
      std::memcpy(output, bytes_.data() + offset_, size);
    }
    offset_ += size;
  }

  /** @brief Reads canonical uint32. @return Exact scalar. */
  std::uint32_t u32() {
    std::array<std::byte, 4U> bytes{};
    raw(bytes.data(), bytes.size());
    std::uint32_t result = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      result |= std::to_integer<std::uint32_t>(bytes[index]) << (8U * index);
    }
    return result;
  }

  /**
   * @brief Copies one exact bounded metadata record.
   * @param size Exact record length.
   * @return Owned record bytes.
   * @throws std::invalid_argument on truncation.
   * @throws std::bad_alloc when result allocation fails.
   */
  std::vector<std::byte> bytes(std::size_t size) {
    std::vector<std::byte> result(size);
    raw(result.data(), result.size());
    return result;
  }

  /** @brief Returns the current absolute archive offset. */
  std::size_t offset() const noexcept { return offset_; }

 private:
  /** @brief Borrowed complete archive. */
  const std::vector<std::byte>& bytes_;
  /** @brief Next unread byte offset. */
  std::size_t offset_ = 0U;
};

}  // namespace

/** @copydoc capture_value_artifact */
ValueArtifact capture_value_artifact(std::string output_name,
                                     const Value& value) {
  if (!value.valid()) {
    throw std::invalid_argument("Cannot capture an invalid Value artifact.");
  }
  ValueArtifact artifact;
  artifact.envelope.output_name = std::move(output_name);
  artifact.envelope.representation = value.representation_kind();
  artifact.envelope.layout_kind = value.storage_layout_kind();
  if (value.representation_kind() == ValueRepresentationKind::DenseTensor) {
    artifact.envelope.dense_descriptor = value.dense_tensor_descriptor();
    artifact.envelope.image_facet = value.image_facet();
    if (value.storage_layout_kind() == StorageLayoutKind::Strided) {
      artifact.envelope.strided_layout = value.strided_layout();
    } else if (value.storage_layout_kind() == StorageLayoutKind::Blocked) {
      artifact.envelope.blocked_layout = value.blocked_layout();
    } else {
      invalid_artifact("DenseTensor Value has a provider-defined Layout.");
    }
    const BufferHandle& handle = value.buffer_handle();
    const ReadLease read = handle.acquire_read();
    artifact.payloads.emplace_back(read.data(), read.data() + read.size());
    ValueArtifactBuffer buffer;
    buffer.byte_size = size_to_u64(read.size());
    buffer.required_alignment =
        size_to_u64(value.storage_binding().required_alignment);
    buffer.digest = payload_digest(artifact.payloads.back());
    artifact.envelope.buffers.push_back(buffer);
  } else if (value.representation_kind() ==
             ValueRepresentationKind::ProviderDefined) {
    artifact.envelope.provider_descriptor = value.provider_defined_descriptor();
    artifact.envelope.provider_layout = value.provider_defined_layout();
    artifact.payloads.reserve(value.buffer_count());
    artifact.envelope.buffers.reserve(value.buffer_count());
    for (std::size_t index = 0U; index < value.buffer_count(); ++index) {
      const ProviderReadLease read = value.acquire_provider_read(index);
      artifact.payloads.emplace_back(read.data(), read.data() + read.size());
      ValueArtifactBuffer buffer;
      buffer.index = size_to_u32(index);
      buffer.logical_role = provider_buffer_role(
          value.provider_defined_layout(), size_to_u32(index));
      buffer.required_alignment =
          size_to_u64(value.storage_binding(index).required_alignment);
      const std::uint64_t previous_end =
          index == 0U ? 0U
                      : checked_u64_add(
                            artifact.envelope.buffers.back().artifact_offset,
                            artifact.envelope.buffers.back().byte_size);
      buffer.artifact_offset =
          align_artifact_offset(previous_end, buffer.required_alignment);
      buffer.byte_size = size_to_u64(read.size());
      buffer.digest = payload_digest(artifact.payloads.back());
      artifact.envelope.buffers.push_back(buffer);
    }
  } else {
    invalid_artifact("Value has an unknown representation kind.");
  }
  artifact.envelope.content_digest =
      require_content_digest(compute_content_digest(value));
  artifact.envelope.descriptor_digest =
      canonical_descriptor_digest(artifact.envelope);
  artifact.envelope.storage_layout_digest =
      canonical_layout_digest(artifact.envelope);
  validate_value_artifact(artifact);
  return artifact;
}

/** @copydoc validate_value_artifact */
void validate_value_artifact(const ValueArtifact& artifact) {
  validate_envelope(artifact.envelope);
  if (artifact.payloads.size() != artifact.envelope.buffers.size()) {
    invalid_artifact("Value artifact payload cardinality disagrees.");
  }
  for (std::size_t index = 0U; index < artifact.payloads.size(); ++index) {
    const std::vector<std::byte>& payload = artifact.payloads[index];
    const ValueArtifactBuffer& metadata = artifact.envelope.buffers[index];
    if (payload.empty() || size_to_u64(payload.size()) != metadata.byte_size ||
        !(payload_digest(payload) == metadata.digest)) {
      invalid_artifact("Value artifact payload size or digest disagrees.");
    }
  }
  if (artifact.envelope.representation ==
      ValueRepresentationKind::DenseTensor) {
    const Value value = reconstruct_builtin(artifact);
    validate_content(value, artifact.envelope.content_digest);
  }
}

/** @copydoc reconstruct_value_artifact */
Value reconstruct_value_artifact(const ValueArtifact& artifact,
                                 DataDefinitionRegistry* registry) {
  validate_value_artifact(artifact);
  Value value;
  if (artifact.envelope.representation ==
      ValueRepresentationKind::DenseTensor) {
    value = reconstruct_builtin(artifact);
  } else {
    if (registry == nullptr) {
      throw ExtensionContractError(
          ExtensionErrorCode::MissingProvider,
          "Provider-defined artifact reconstruction requires a registry.");
    }
    std::vector<std::size_t> required_alignments;
    required_alignments.reserve(artifact.envelope.buffers.size());
    for (const ValueArtifactBuffer& buffer : artifact.envelope.buffers) {
      required_alignments.push_back(u64_to_size(buffer.required_alignment));
    }
    value = Value::from_provider_defined_payloads(
        *registry, *artifact.envelope.provider_descriptor,
        *artifact.envelope.provider_layout, artifact.payloads,
        std::move(required_alignments));
  }
  validate_content(value, artifact.envelope.content_digest);
  return value;
}

/** @copydoc encode_value_artifact_envelope */
std::vector<std::byte> encode_value_artifact_envelope(
    const ValueArtifactEnvelope& envelope) {
  validate_envelope(envelope);
  Writer writer;
  writer.raw(kArtifactMagic.data(), kArtifactMagic.size());
  writer.u32(envelope.structural_version);
  writer.string(envelope.output_name);
  writer.u32(static_cast<std::uint32_t>(envelope.representation));
  writer.u32(static_cast<std::uint32_t>(envelope.layout_kind));
  writer.u8(envelope.image_facet.has_value() ? 1U : 0U);
  writer.u8(envelope.content_digest.has_value() ? 1U : 0U);
  if (envelope.representation == ValueRepresentationKind::DenseTensor) {
    encode_dense_descriptor(&writer, *envelope.dense_descriptor);
    if (envelope.image_facet.has_value()) {
      encode_image_facet(&writer, *envelope.image_facet);
    }
    if (envelope.layout_kind == StorageLayoutKind::Strided) {
      encode_strided_layout(&writer, *envelope.strided_layout);
    } else {
      encode_blocked_layout(&writer, *envelope.blocked_layout);
    }
  } else {
    ExtensionArtifactEnvelope extension;
    extension.descriptor = *envelope.provider_descriptor;
    extension.layout = *envelope.provider_layout;
    extension.descriptor_digest = envelope.descriptor_digest;
    extension.storage_layout_digest = envelope.storage_layout_digest;
    extension.content_digest = envelope.content_digest;
    writer.bytes(encode_extension_artifact(extension));
  }
  if (envelope.content_digest.has_value()) {
    writer.u32(static_cast<std::uint32_t>(envelope.content_digest->algorithm));
    writer.raw(envelope.content_digest->bytes.data(),
               envelope.content_digest->bytes.size());
  }
  writer.u32(static_cast<std::uint32_t>(envelope.descriptor_digest.algorithm));
  writer.raw(envelope.descriptor_digest.bytes.data(),
             envelope.descriptor_digest.bytes.size());
  writer.u32(
      static_cast<std::uint32_t>(envelope.storage_layout_digest.algorithm));
  writer.raw(envelope.storage_layout_digest.bytes.data(),
             envelope.storage_layout_digest.bytes.size());
  writer.u8(envelope.joins.artifact_identity.has_value() ? 1U : 0U);
  if (envelope.joins.artifact_identity.has_value()) {
    writer.string(*envelope.joins.artifact_identity);
  }
  writer.u8(envelope.joins.commit_identity.has_value() ? 1U : 0U);
  if (envelope.joins.commit_identity.has_value()) {
    writer.string(*envelope.joins.commit_identity);
  }
  writer.u8(envelope.joins.slot_identity.has_value() ? 1U : 0U);
  if (envelope.joins.slot_identity.has_value()) {
    writer.string(*envelope.joins.slot_identity);
  }
  writer.u32(size_to_u32(envelope.statistics_references.size()));
  for (const ValueArtifactStatisticsReference& reference :
       envelope.statistics_references) {
    writer.u32(static_cast<std::uint32_t>(reference.content_digest.algorithm));
    writer.raw(reference.content_digest.bytes.data(),
               reference.content_digest.bytes.size());
    writer.raw(reference.query_digest.bytes.data(),
               reference.query_digest.bytes.size());
    writer.string(reference.algorithm);
    writer.u32(reference.algorithm_version);
    writer.string(reference.artifact_identity);
  }
  writer.u32(size_to_u32(envelope.buffers.size()));
  for (const ValueArtifactBuffer& buffer : envelope.buffers) {
    writer.u32(buffer.index);
    writer.u32(buffer.logical_role);
    writer.u64(buffer.artifact_offset);
    writer.u64(buffer.byte_size);
    writer.u64(buffer.required_alignment);
    writer.raw(buffer.digest.bytes.data(), buffer.digest.bytes.size());
  }
  return std::move(writer).finish();
}

/** @copydoc decode_value_artifact_envelope */
ValueArtifactEnvelope decode_value_artifact_envelope(
    const std::vector<std::byte>& bytes) {
  Reader reader(bytes);
  std::array<std::byte, kArtifactMagic.size()> magic{};
  reader.raw(magic.data(), magic.size());
  if (magic != kArtifactMagic) {
    invalid_artifact("Value artifact metadata magic is invalid.");
  }
  ValueArtifactEnvelope envelope;
  envelope.structural_version = reader.u32();
  envelope.output_name = reader.string();
  envelope.representation = static_cast<ValueRepresentationKind>(reader.u32());
  envelope.layout_kind = static_cast<StorageLayoutKind>(reader.u32());
  const std::uint8_t has_image = reader.u8();
  const std::uint8_t has_content = reader.u8();
  if (has_image > 1U || has_content > 1U) {
    invalid_artifact("Value artifact optional-field flag is invalid.");
  }
  if (envelope.representation == ValueRepresentationKind::DenseTensor) {
    envelope.dense_descriptor = decode_dense_descriptor(&reader);
    if (has_image != 0U) {
      envelope.image_facet = decode_image_facet(&reader);
    }
    if (envelope.layout_kind == StorageLayoutKind::Strided) {
      envelope.strided_layout = decode_strided_layout(&reader);
    } else if (envelope.layout_kind == StorageLayoutKind::Blocked) {
      envelope.blocked_layout = decode_blocked_layout(&reader);
    } else {
      invalid_artifact("DenseTensor artifact Layout kind is invalid.");
    }
  } else if (envelope.representation ==
             ValueRepresentationKind::ProviderDefined) {
    if (has_image != 0U ||
        envelope.layout_kind != StorageLayoutKind::ProviderDefined) {
      invalid_artifact("Provider-defined artifact flags are invalid.");
    }
    const ExtensionArtifactEnvelope extension =
        decode_extension_artifact(reader.bytes());
    envelope.provider_descriptor = extension.descriptor;
    envelope.provider_layout = extension.layout;
  } else {
    invalid_artifact("Value artifact representation kind is invalid.");
  }
  if (has_content != 0U) {
    ContentDigest digest;
    digest.algorithm = static_cast<CanonicalDigestAlgorithm>(reader.u32());
    reader.raw(digest.bytes.data(), digest.bytes.size());
    envelope.content_digest = digest;
  }
  envelope.descriptor_digest.algorithm =
      static_cast<CanonicalDigestAlgorithm>(reader.u32());
  reader.raw(envelope.descriptor_digest.bytes.data(),
             envelope.descriptor_digest.bytes.size());
  envelope.storage_layout_digest.algorithm =
      static_cast<CanonicalDigestAlgorithm>(reader.u32());
  reader.raw(envelope.storage_layout_digest.bytes.data(),
             envelope.storage_layout_digest.bytes.size());
  const std::uint8_t has_artifact = reader.u8();
  if (has_artifact > 1U) {
    invalid_artifact("Value artifact join flag is invalid.");
  }
  if (has_artifact != 0U) {
    envelope.joins.artifact_identity = reader.string();
  }
  const std::uint8_t has_commit = reader.u8();
  if (has_commit > 1U) {
    invalid_artifact("Value artifact join flag is invalid.");
  }
  if (has_commit != 0U) {
    envelope.joins.commit_identity = reader.string();
  }
  const std::uint8_t has_slot = reader.u8();
  if (has_slot > 1U) {
    invalid_artifact("Value artifact join flag is invalid.");
  }
  if (has_slot != 0U) {
    envelope.joins.slot_identity = reader.string();
  }
  const std::size_t statistics_count = reader.u32();
  if (statistics_count > kMaximumValueArtifactStatisticsReferences) {
    throw std::length_error(
        "Value artifact statistics-reference count exceeds its bound.");
  }
  envelope.statistics_references.reserve(statistics_count);
  for (std::size_t index = 0U; index < statistics_count; ++index) {
    ValueArtifactStatisticsReference reference;
    reference.content_digest.algorithm =
        static_cast<CanonicalDigestAlgorithm>(reader.u32());
    reader.raw(reference.content_digest.bytes.data(),
               reference.content_digest.bytes.size());
    reader.raw(reference.query_digest.bytes.data(),
               reference.query_digest.bytes.size());
    reference.algorithm = reader.string();
    reference.algorithm_version = reader.u32();
    reference.artifact_identity = reader.string();
    envelope.statistics_references.push_back(std::move(reference));
  }
  const std::size_t buffer_count = reader.u32();
  if (buffer_count == 0U || buffer_count > kMaximumExtensionBuffers) {
    invalid_artifact("Value artifact buffer count is outside bounds.");
  }
  envelope.buffers.reserve(buffer_count);
  for (std::size_t index = 0U; index < buffer_count; ++index) {
    ValueArtifactBuffer buffer;
    buffer.index = reader.u32();
    buffer.logical_role = reader.u32();
    buffer.artifact_offset = reader.u64();
    buffer.byte_size = reader.u64();
    buffer.required_alignment = reader.u64();
    reader.raw(buffer.digest.bytes.data(), buffer.digest.bytes.size());
    envelope.buffers.push_back(buffer);
  }
  reader.finish();
  validate_envelope(envelope);
  return envelope;
}

/** @copydoc compute_artifact_payload_digest */
ArtifactPayloadDigest compute_artifact_payload_digest(
    const std::vector<std::byte>& bytes) {
  return payload_digest(bytes);
}

/** @copydoc encode_named_value_artifact_set */
std::vector<std::byte> encode_named_value_artifact_set(
    const NamedValueArtifactSet& artifacts) {
  if (artifacts.values.size() > kMaximumNamedValueArtifacts) {
    throw std::length_error(
        "Named Value artifact set exceeds its count bound.");
  }

  NamedValueArtifactSet normalized = artifacts;
  std::string previous_name;
  bool first = true;
  std::uint64_t aggregate_payload = 0U;
  std::vector<std::vector<std::byte>> encoded_envelopes;
  encoded_envelopes.reserve(normalized.values.size());
  for (ValueArtifact& artifact : normalized.values) {
    if (!first && !(previous_name < artifact.envelope.output_name)) {
      invalid_artifact(
          "Named Value artifact names are not strictly increasing.");
    }
    previous_name = artifact.envelope.output_name;
    first = false;
    std::uint64_t local_offset = 0U;
    for (ValueArtifactBuffer& buffer : artifact.envelope.buffers) {
      local_offset =
          align_artifact_offset(local_offset, buffer.required_alignment);
      buffer.artifact_offset = local_offset;
      local_offset = checked_u64_add(local_offset, buffer.byte_size);
      aggregate_payload = checked_u64_add(aggregate_payload, buffer.byte_size);
      if (aggregate_payload > kMaximumValueArtifactPayloadBytes) {
        throw std::length_error(
            "Named Value artifact payload exceeds its bound.");
      }
    }
    validate_value_artifact(artifact);
    encoded_envelopes.push_back(
        encode_value_artifact_envelope(artifact.envelope));
  }

  std::uint64_t metadata_size =
      static_cast<std::uint64_t>(kArtifactSetMagic.size()) + 8U;
  for (const std::vector<std::byte>& envelope : encoded_envelopes) {
    metadata_size = checked_u64_add(
        metadata_size, checked_u64_add(4U, size_to_u64(envelope.size())));
  }
  if (metadata_size > kMaximumValueArtifactMetadataBytes) {
    throw std::length_error(
        "Named Value artifact set metadata exceeds its bound.");
  }

  std::uint64_t payload_offset = metadata_size;
  for (std::size_t value_index = 0U; value_index < normalized.values.size();
       ++value_index) {
    ValueArtifact& artifact = normalized.values[value_index];
    for (ValueArtifactBuffer& buffer : artifact.envelope.buffers) {
      payload_offset =
          align_artifact_offset(payload_offset, buffer.required_alignment);
      buffer.artifact_offset = payload_offset;
      payload_offset = checked_u64_add(payload_offset, buffer.byte_size);
    }
    encoded_envelopes[value_index] =
        encode_value_artifact_envelope(artifact.envelope);
  }
  if (payload_offset > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error(
        "Named Value artifact archive exceeds local address space.");
  }

  std::vector<std::byte> output;
  output.reserve(static_cast<std::size_t>(payload_offset));
  append_archive_bytes(&output, kArtifactSetMagic.data(),
                       kArtifactSetMagic.size());
  append_archive_u32(&output, kNamedValueArtifactSetArchiveVersion);
  append_archive_u32(&output, size_to_u32(encoded_envelopes.size()));
  for (const std::vector<std::byte>& envelope : encoded_envelopes) {
    append_archive_u32(&output, size_to_u32(envelope.size()));
    append_archive_bytes(&output, envelope.data(), envelope.size());
  }
  if (output.size() != metadata_size) {
    throw std::logic_error(
        "Named Value artifact metadata sizing became inconsistent.");
  }

  for (const ValueArtifact& artifact : normalized.values) {
    for (std::size_t index = 0U; index < artifact.payloads.size(); ++index) {
      const ValueArtifactBuffer& buffer = artifact.envelope.buffers[index];
      const std::size_t offset = u64_to_size(buffer.artifact_offset);
      if (output.size() > offset) {
        throw std::logic_error(
            "Named Value artifact payload offsets overlap metadata.");
      }
      output.resize(offset, std::byte{0U});
      append_archive_bytes(&output, artifact.payloads[index].data(),
                           artifact.payloads[index].size());
    }
  }
  if (output.size() != payload_offset) {
    throw std::logic_error(
        "Named Value artifact archive sizing became inconsistent.");
  }
  return output;
}

/** @copydoc decode_named_value_artifact_set */
NamedValueArtifactSet decode_named_value_artifact_set(
    const std::vector<std::byte>& bytes) {
  if (bytes.size() < kArtifactSetMagic.size() + 8U) {
    invalid_artifact("Named Value artifact set archive is truncated.");
  }
  if (size_to_u64(bytes.size()) >
      checked_u64_add(kMaximumValueArtifactPayloadBytes,
                      kMaximumValueArtifactMetadataBytes)) {
    throw std::length_error(
        "Named Value artifact set archive exceeds its bound.");
  }

  ArtifactSetReader reader(bytes);
  std::array<std::byte, kArtifactSetMagic.size()> magic{};
  reader.raw(magic.data(), magic.size());
  if (magic != kArtifactSetMagic ||
      reader.u32() != kNamedValueArtifactSetArchiveVersion) {
    invalid_artifact("Named Value artifact set magic/version is invalid.");
  }
  const std::size_t value_count = reader.u32();
  if (value_count > kMaximumNamedValueArtifacts) {
    throw std::length_error(
        "Named Value artifact set exceeds its count bound.");
  }

  NamedValueArtifactSet result;
  result.values.reserve(value_count);
  std::string previous_name;
  bool first = true;
  for (std::size_t index = 0U; index < value_count; ++index) {
    const std::size_t envelope_size = reader.u32();
    if (envelope_size == 0U ||
        envelope_size > kMaximumValueArtifactMetadataBytes ||
        reader.offset() > kMaximumValueArtifactMetadataBytes - envelope_size) {
      throw std::length_error(
          "Named Value artifact envelope size exceeds its bound.");
    }
    ValueArtifact artifact;
    artifact.envelope =
        decode_value_artifact_envelope(reader.bytes(envelope_size));
    if (!first && !(previous_name < artifact.envelope.output_name)) {
      invalid_artifact(
          "Named Value artifact names are not strictly increasing.");
    }
    previous_name = artifact.envelope.output_name;
    first = false;
    result.values.push_back(std::move(artifact));
  }

  const std::size_t metadata_end = reader.offset();
  if (metadata_end > kMaximumValueArtifactMetadataBytes) {
    throw std::length_error(
        "Named Value artifact set metadata exceeds its bound.");
  }
  std::uint64_t cursor = size_to_u64(metadata_end);
  std::uint64_t aggregate_payload = 0U;
  for (ValueArtifact& artifact : result.values) {
    artifact.payloads.reserve(artifact.envelope.buffers.size());
    for (const ValueArtifactBuffer& buffer : artifact.envelope.buffers) {
      const std::uint64_t expected_offset =
          align_artifact_offset(cursor, buffer.required_alignment);
      if (buffer.artifact_offset != expected_offset ||
          buffer.artifact_offset > size_to_u64(bytes.size())) {
        invalid_artifact(
            "Named Value artifact payload spans are noncanonical.");
      }
      const std::size_t gap_begin = u64_to_size(cursor);
      const std::size_t gap_end = u64_to_size(buffer.artifact_offset);
      if (std::any_of(bytes.begin() + gap_begin, bytes.begin() + gap_end,
                      [](std::byte value) { return value != std::byte{0U}; })) {
        invalid_artifact("Named Value artifact alignment padding is nonzero.");
      }
      const std::uint64_t end =
          checked_u64_add(buffer.artifact_offset, buffer.byte_size);
      if (end > size_to_u64(bytes.size())) {
        invalid_artifact("Named Value artifact payload span is truncated.");
      }
      const std::size_t begin_index = u64_to_size(buffer.artifact_offset);
      const std::size_t end_index = u64_to_size(end);
      artifact.payloads.emplace_back(bytes.begin() + begin_index,
                                     bytes.begin() + end_index);
      aggregate_payload = checked_u64_add(aggregate_payload, buffer.byte_size);
      if (aggregate_payload > kMaximumValueArtifactPayloadBytes) {
        throw std::length_error(
            "Named Value artifact payload exceeds its bound.");
      }
      cursor = end;
    }
    validate_value_artifact(artifact);
  }
  if (cursor != size_to_u64(bytes.size())) {
    invalid_artifact("Named Value artifact set archive has trailing bytes.");
  }
  return result;
}

}  // namespace ps
