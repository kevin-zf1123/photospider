/**
 * @file isolated_cpu_invocation_protocol.cpp
 * @brief Implements the bounded pointer-free isolated CPU invocation codec.
 */
#include "execution/isolation/isolated_cpu_invocation_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/extension_internal.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/value.hpp"

namespace ps::execution {
namespace {

/** @brief Fixed big-endian packet magic spelling ASCII `PSI2`. */
constexpr std::uint32_t kIsolatedCpuPacketMagic = 0x50534932U;

/**
 * @brief Closed packet kinds in protocol version two.
 * @throws Nothing for ordinary enum operations.
 */
enum class IsolatedCpuPacketKind : std::uint16_t {
  /** @brief Host-to-runtime invocation request. */
  Request = 1U,
  /** @brief Runtime-to-Host invocation response. */
  Response = 2U,
};

/**
 * @brief Raises one source-private fail-closed protocol error.
 * @param message Stable rejection diagnostic.
 * @return Never returns.
 * @throws IsolatedCpuProtocolError always.
 */
[[noreturn]] void fail(const std::string& message) {
  throw IsolatedCpuProtocolError(message);
}

/**
 * @brief Adds two unsigned transport-domain values without wraparound.
 * @param left First value.
 * @param right Second value.
 * @return Exact sum.
 * @throws IsolatedCpuProtocolError when the sum exceeds uint64.
 */
std::uint64_t checked_add(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    fail("isolated CPU invocation arithmetic overflowed");
  }
  return left + right;
}

/**
 * @brief Multiplies two unsigned transport-domain values without wraparound.
 * @param left First value.
 * @param right Second value.
 * @return Exact product.
 * @throws IsolatedCpuProtocolError when the product exceeds uint64.
 */
std::uint64_t checked_multiply(std::uint64_t left, std::uint64_t right) {
  if (right != 0U && left > std::numeric_limits<std::uint64_t>::max() / right) {
    fail("isolated CPU invocation arithmetic overflowed");
  }
  return left * right;
}

/**
 * @brief Converts local size_t to the fixed uint64 transport domain.
 * @param value Local byte/count value.
 * @return Exact transport representation.
 * @throws std::overflow_error on a platform with wider size_t content.
 */
std::uint64_t to_u64(std::size_t value) {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(
          "isolated CPU invocation local size exceeds uint64");
    }
  }
  return static_cast<std::uint64_t>(value);
}

/**
 * @brief Returns the exact unsigned magnitude of a signed stride.
 * @param value Signed protocol stride, including INT64_MIN.
 * @return Exact nonnegative magnitude.
 * @throws Nothing.
 */
std::uint64_t signed_magnitude(std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

/**
 * @brief Bounded big-endian payload writer with explicit scalar encodings.
 * @throws std::bad_alloc when bounded storage growth cannot allocate.
 * @note Native struct layout never enters the output.
 */
class ByteWriter final {
 public:
  /**
   * @brief Reserves a small bounded initial payload capacity.
   * @throws std::bad_alloc when reserve cannot allocate.
   */
  ByteWriter() { bytes_.reserve(1024U); }

  /**
   * @brief Appends one unsigned byte.
   * @param value Exact byte value.
   * @throws std::length_error when the packet bound would be exceeded.
   */
  void put_u8(std::uint8_t value) { put_byte(static_cast<std::byte>(value)); }

  /**
   * @brief Appends one closed boolean byte.
   * @param value Boolean value.
   * @throws std::length_error when the packet bound would be exceeded.
   */
  void put_bool(bool value) { put_u8(value ? 1U : 0U); }

  /**
   * @brief Appends one big-endian unsigned 16-bit value.
   * @param value Exact scalar.
   * @throws std::length_error when the packet bound would be exceeded.
   */
  void put_u16(std::uint16_t value) {
    for (int shift = 8; shift >= 0; shift -= 8) {
      put_u8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
  }

  /**
   * @brief Appends one big-endian unsigned 32-bit value.
   * @param value Exact scalar.
   * @throws std::length_error when the packet bound would be exceeded.
   */
  void put_u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      put_u8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
  }

  /**
   * @brief Appends one big-endian unsigned 64-bit value.
   * @param value Exact scalar.
   * @throws std::length_error when the packet bound would be exceeded.
   */
  void put_u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      put_u8(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
  }

  /**
   * @brief Appends one canonical sign-plus-magnitude int64.
   * @param value Exact signed scalar.
   * @throws std::length_error when the packet bound would be exceeded.
   * @note Zero always receives a false sign byte.
   */
  void put_i64(std::int64_t value) {
    put_bool(value < 0);
    put_u64(signed_magnitude(value));
  }

  /**
   * @brief Appends one finite IEEE-754 binary64 value.
   * @param value Validated finite scalar.
   * @throws std::length_error when the packet bound would be exceeded.
   */
  void put_f64(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t),
                  "protocol requires 64-bit double");
    static_assert(std::numeric_limits<double>::is_iec559,
                  "protocol requires IEEE-754 double");
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u64(bits);
  }

  /**
   * @brief Appends one fixed raw byte range.
   * @param bytes First byte, null only for zero size.
   * @param size Exact byte count.
   * @throws std::invalid_argument for null nonempty input.
   * @throws std::length_error when the packet bound would be exceeded.
   * @throws std::bad_alloc when bounded storage growth cannot allocate.
   */
  void put_raw(const std::byte* bytes, std::size_t size) {
    if (size != 0U && bytes == nullptr) {
      throw std::invalid_argument(
          "isolated CPU invocation writer received null bytes");
    }
    require_capacity(size);
    bytes_.insert(bytes_.end(), bytes, bytes + size);
  }

  /**
   * @brief Appends one length-prefixed bounded string.
   * @param value Exact opaque bytes.
   * @param maximum Inclusive field maximum.
   * @throws std::length_error for field or packet bound overflow.
   * @throws std::bad_alloc when bounded storage growth cannot allocate.
   */
  void put_string(const std::string& value, std::size_t maximum) {
    if (value.size() > maximum ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error(
          "isolated CPU invocation string exceeds its bound");
    }
    put_u32(static_cast<std::uint32_t>(value.size()));
    put_raw(reinterpret_cast<const std::byte*>(value.data()), value.size());
  }

  /**
   * @brief Transfers the completed byte vector.
   * @return Exact encoded bytes.
   * @throws Nothing.
   */
  std::vector<std::byte> finish() noexcept { return std::move(bytes_); }

 private:
  /**
   * @brief Appends one byte after aggregate-bound validation.
   * @param value Exact byte.
   * @throws std::length_error when the packet maximum would be exceeded.
   */
  void put_byte(std::byte value) {
    require_capacity(1U);
    bytes_.push_back(value);
  }

  /**
   * @brief Validates one pending append against the complete packet maximum.
   * @param additional Pending byte count.
   * @throws std::length_error on checked bound overflow.
   */
  void require_capacity(std::size_t additional) const {
    if (additional > kMaximumIsolatedCpuPacketBytes - bytes_.size()) {
      throw std::length_error(
          "isolated CPU invocation packet exceeds its bound");
    }
  }

  /** @brief Exact encoded bytes under construction. */
  std::vector<std::byte> bytes_;
};

/**
 * @brief Transactional bounded reader for one exact packet or payload.
 * @throws IsolatedCpuProtocolError for truncation or malformed scalars.
 * @note The reader borrows immutable bytes and derives no external pointer.
 */
class ByteReader final {
 public:
  /**
   * @brief Borrows one exact byte range.
   * @param bytes First byte, null only when size is zero.
   * @param size Exact available byte count.
   * @throws std::invalid_argument for null nonempty input.
   */
  ByteReader(const std::byte* bytes, std::size_t size)
      : bytes_(bytes), size_(size) {
    if (bytes_ == nullptr && size_ != 0U) {
      throw std::invalid_argument(
          "isolated CPU invocation reader received null bytes");
    }
  }

  /**
   * @brief Reads one unsigned byte.
   * @return Exact value.
   * @throws IsolatedCpuProtocolError for truncation.
   */
  std::uint8_t get_u8() { return std::to_integer<std::uint8_t>(*take(1U)); }

  /**
   * @brief Reads one closed boolean byte.
   * @return Decoded boolean.
   * @throws IsolatedCpuProtocolError for truncation or values above one.
   */
  bool get_bool() {
    const std::uint8_t value = get_u8();
    if (value > 1U) {
      fail("isolated CPU invocation boolean is invalid");
    }
    return value != 0U;
  }

  /**
   * @brief Reads one big-endian unsigned 16-bit value.
   * @return Exact scalar.
   * @throws IsolatedCpuProtocolError for truncation.
   */
  std::uint16_t get_u16() {
    std::uint16_t value = 0U;
    for (std::size_t index = 0U; index < 2U; ++index) {
      value = static_cast<std::uint16_t>((value << 8U) | get_u8());
    }
    return value;
  }

  /**
   * @brief Reads one big-endian unsigned 32-bit value.
   * @return Exact scalar.
   * @throws IsolatedCpuProtocolError for truncation.
   */
  std::uint32_t get_u32() {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
      value = (value << 8U) | get_u8();
    }
    return value;
  }

  /**
   * @brief Reads one big-endian unsigned 64-bit value.
   * @return Exact scalar.
   * @throws IsolatedCpuProtocolError for truncation.
   */
  std::uint64_t get_u64() {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
      value = (value << 8U) | get_u8();
    }
    return value;
  }

  /**
   * @brief Reads one canonical sign-plus-magnitude int64.
   * @return Exact signed value.
   * @throws IsolatedCpuProtocolError for negative zero or excess magnitude.
   */
  std::int64_t get_i64() {
    const bool negative = get_bool();
    const std::uint64_t magnitude = get_u64();
    if (negative && magnitude == 0U) {
      fail("isolated CPU invocation signed integer uses negative zero");
    }
    const std::uint64_t minimum_magnitude =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
        1U;
    if ((!negative &&
         magnitude > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) ||
        (negative && magnitude > minimum_magnitude)) {
      fail("isolated CPU invocation signed integer exceeds int64");
    }
    if (!negative) {
      return static_cast<std::int64_t>(magnitude);
    }
    if (magnitude == minimum_magnitude) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
  }

  /**
   * @brief Reads one finite IEEE-754 binary64 value.
   * @return Exact scalar bits interpreted as double.
   * @throws IsolatedCpuProtocolError for non-finite content.
   */
  double get_f64() {
    static_assert(sizeof(double) == sizeof(std::uint64_t),
                  "protocol requires 64-bit double");
    static_assert(std::numeric_limits<double>::is_iec559,
                  "protocol requires IEEE-754 double");
    const std::uint64_t bits = get_u64();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) {
      fail("isolated CPU invocation floating parameter is not finite");
    }
    return value;
  }

  /**
   * @brief Reads one bounded length-prefixed string.
   * @param maximum Inclusive field maximum.
   * @return Newly owned exact bytes.
   * @throws IsolatedCpuProtocolError for length overflow or truncation.
   * @throws std::bad_alloc when copied storage cannot allocate.
   */
  std::string get_string(std::size_t maximum) {
    const std::size_t size = get_u32();
    if (size > maximum) {
      fail("isolated CPU invocation string exceeds its bound");
    }
    const std::byte* bytes = take(size);
    return std::string(reinterpret_cast<const char*>(bytes), size);
  }

  /**
   * @brief Copies one exact fixed raw range.
   * @param output Destination, null only for zero size.
   * @param size Exact byte count.
   * @throws std::invalid_argument for null nonempty output.
   * @throws IsolatedCpuProtocolError for truncation.
   */
  void get_raw(std::byte* output, std::size_t size) {
    if (output == nullptr && size != 0U) {
      throw std::invalid_argument(
          "isolated CPU invocation reader received null output");
    }
    const std::byte* source = take(size);
    std::copy(source, source + size, output);
  }

  /**
   * @brief Returns the unread byte count.
   * @return Exact remaining bytes.
   * @throws Nothing.
   */
  std::size_t remaining() const noexcept { return size_ - offset_; }

  /**
   * @brief Rejects any trailing bytes.
   * @throws IsolatedCpuProtocolError when unread bytes remain.
   */
  void finish() const {
    if (offset_ != size_) {
      fail("isolated CPU invocation packet has trailing bytes");
    }
  }

 private:
  /**
   * @brief Borrows and advances over one exact byte range.
   * @param size Required bytes.
   * @return Borrowed first byte.
   * @throws IsolatedCpuProtocolError for truncation.
   */
  const std::byte* take(std::size_t size) {
    if (size > size_ - offset_) {
      fail("isolated CPU invocation packet is truncated");
    }
    const std::byte* result = bytes_ + offset_;
    offset_ += size;
    return result;
  }

  /** @brief Borrowed immutable first byte. */
  const std::byte* bytes_ = nullptr;
  /** @brief Exact borrowed byte count. */
  std::size_t size_ = 0U;
  /** @brief Next unread byte offset. */
  std::size_t offset_ = 0U;
};

/**
 * @brief One decoded packet header and independently owned payload.
 * @throws Nothing for ordinary value operations.
 */
struct DecodedPacket final {
  /** @brief Closed validated packet kind. */
  IsolatedCpuPacketKind kind = IsolatedCpuPacketKind::Request;
  /** @brief Exact bounded payload bytes. */
  std::vector<std::byte> payload;
};

/**
 * @brief Parses one closed packet kind.
 * @param value Numeric wire value.
 * @return Valid typed kind.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuPacketKind parse_packet_kind(std::uint16_t value) {
  switch (static_cast<IsolatedCpuPacketKind>(value)) {
    case IsolatedCpuPacketKind::Request:
    case IsolatedCpuPacketKind::Response:
      return static_cast<IsolatedCpuPacketKind>(value);
  }
  fail("isolated CPU invocation packet kind is invalid");
}

/**
 * @brief Wraps one bounded payload in the fixed protocol-v2 header.
 * @param kind Closed packet kind.
 * @param payload Exact payload bytes.
 * @return Complete packet.
 * @throws std::length_error for aggregate overflow.
 * @throws std::bad_alloc when packet storage cannot allocate.
 */
std::vector<std::byte> encode_packet(IsolatedCpuPacketKind kind,
                                     const std::vector<std::byte>& payload) {
  if (payload.size() >
      kMaximumIsolatedCpuPacketBytes - kIsolatedCpuPacketHeaderBytes) {
    throw std::length_error(
        "isolated CPU invocation payload exceeds packet bound");
  }
  ByteWriter writer;
  writer.put_u32(kIsolatedCpuPacketMagic);
  writer.put_u16(kIsolatedCpuInvocationProtocolVersion);
  writer.put_u16(static_cast<std::uint16_t>(kind));
  writer.put_u32(static_cast<std::uint32_t>(payload.size()));
  writer.put_raw(payload.data(), payload.size());
  return writer.finish();
}

/**
 * @brief Validates and separates one complete packet.
 * @param packet Exact received bytes.
 * @return Closed kind and copied exact payload.
 * @throws IsolatedCpuProtocolError for size, magic, version, or framing errors.
 * @throws std::bad_alloc when payload copy cannot allocate.
 */
DecodedPacket decode_packet(const std::vector<std::byte>& packet) {
  if (packet.size() < kIsolatedCpuPacketHeaderBytes ||
      packet.size() > kMaximumIsolatedCpuPacketBytes) {
    fail("isolated CPU invocation packet size is invalid");
  }
  ByteReader reader(packet.data(), packet.size());
  if (reader.get_u32() != kIsolatedCpuPacketMagic) {
    fail("isolated CPU invocation packet magic is invalid");
  }
  if (reader.get_u16() != kIsolatedCpuInvocationProtocolVersion) {
    fail("isolated CPU invocation protocol version is unsupported");
  }
  const IsolatedCpuPacketKind kind = parse_packet_kind(reader.get_u16());
  const std::size_t payload_size = reader.get_u32();
  if (payload_size != reader.remaining()) {
    fail("isolated CPU invocation packet length is inconsistent");
  }
  std::vector<std::byte> payload(payload_size);
  reader.get_raw(payload.data(), payload.size());
  reader.finish();
  return DecodedPacket{kind, std::move(payload)};
}

/**
 * @brief Rejects empty, oversized, or embedded-NUL text.
 * @param value Text bytes to inspect.
 * @param maximum Inclusive byte maximum.
 * @param label Stable diagnostic field name.
 * @throws IsolatedCpuProtocolError for malformed text.
 */
void validate_text(const std::string& value, std::size_t maximum,
                   const char* label) {
  if (value.empty() || value.size() > maximum ||
      value.find('\0') != std::string::npos) {
    fail(std::string("isolated CPU invocation ") + label + " is invalid");
  }
}

/**
 * @brief Encodes one fixed opaque identity.
 * @param identity Exact 128-bit value.
 * @param writer Non-null output owner.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding allocation or bound errors unchanged.
 */
void encode_opaque_id(const IsolatedCpuOpaqueId& identity, ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU identity writer is null");
  }
  writer->put_raw(identity.bytes.data(), identity.bytes.size());
}

/**
 * @brief Decodes one fixed opaque identity.
 * @param reader Non-null payload reader.
 * @return Exact 128-bit value.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for truncation.
 */
IsolatedCpuOpaqueId decode_opaque_id(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU identity reader is null");
  }
  IsolatedCpuOpaqueId identity;
  reader->get_raw(identity.bytes.data(), identity.bytes.size());
  return identity;
}

/**
 * @brief Encodes the complete exact invocation identity.
 * @param identity Identity tuple.
 * @param writer Non-null output owner.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding allocation or bound errors unchanged.
 */
void encode_identity(const IsolatedCpuInvocationIdentity& identity,
                     ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU identity writer is null");
  }
  encode_opaque_id(identity.tenant_id, writer);
  encode_opaque_id(identity.job_id, writer);
  encode_opaque_id(identity.attempt_id, writer);
  encode_opaque_id(identity.worker_id, writer);
  writer->put_u64(identity.worker_lease_generation);
  encode_opaque_id(identity.plugin_package_id, writer);
  writer->put_u64(identity.plugin_generation);
  encode_opaque_id(identity.invocation_id, writer);
}

/**
 * @brief Decodes the complete exact invocation identity.
 * @param reader Non-null payload reader.
 * @return Newly reconstructed tuple.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for truncation.
 */
IsolatedCpuInvocationIdentity decode_identity(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU identity reader is null");
  }
  IsolatedCpuInvocationIdentity identity;
  identity.tenant_id = decode_opaque_id(reader);
  identity.job_id = decode_opaque_id(reader);
  identity.attempt_id = decode_opaque_id(reader);
  identity.worker_id = decode_opaque_id(reader);
  identity.worker_lease_generation = reader->get_u64();
  identity.plugin_package_id = decode_opaque_id(reader);
  identity.plugin_generation = reader->get_u64();
  identity.invocation_id = decode_opaque_id(reader);
  return identity;
}

/**
 * @brief Parses one closed scalar kind.
 * @param value Numeric wire value.
 * @return Valid kind.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuScalarKind parse_scalar_kind(std::uint8_t value) {
  switch (static_cast<IsolatedCpuScalarKind>(value)) {
    case IsolatedCpuScalarKind::Boolean:
    case IsolatedCpuScalarKind::SignedInteger:
    case IsolatedCpuScalarKind::UnsignedInteger:
    case IsolatedCpuScalarKind::FloatingPoint:
    case IsolatedCpuScalarKind::String:
      return static_cast<IsolatedCpuScalarKind>(value);
  }
  fail("isolated CPU invocation scalar kind is invalid");
}

/**
 * @brief Encodes one canonical scalar parameter.
 * @param parameter Validated parameter.
 * @param writer Non-null output owner.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding allocation or bound errors unchanged.
 */
void encode_parameter(const IsolatedCpuScalarParameter& parameter,
                      ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU parameter writer is null");
  }
  writer->put_string(parameter.name, kMaximumIsolatedCpuParameterNameBytes);
  writer->put_u8(static_cast<std::uint8_t>(parameter.kind));
  switch (parameter.kind) {
    case IsolatedCpuScalarKind::Boolean:
      writer->put_bool(parameter.boolean_value);
      return;
    case IsolatedCpuScalarKind::SignedInteger:
      writer->put_i64(parameter.signed_value);
      return;
    case IsolatedCpuScalarKind::UnsignedInteger:
      writer->put_u64(parameter.unsigned_value);
      return;
    case IsolatedCpuScalarKind::FloatingPoint:
      writer->put_f64(parameter.floating_value);
      return;
    case IsolatedCpuScalarKind::String:
      writer->put_string(parameter.string_value,
                         kMaximumIsolatedCpuParameterStringBytes);
      return;
  }
  fail("isolated CPU invocation scalar kind is invalid");
}

/**
 * @brief Decodes one scalar parameter into canonical inactive-field state.
 * @param reader Non-null payload reader.
 * @return Newly reconstructed parameter.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError or std::bad_alloc from bounded decoding.
 */
IsolatedCpuScalarParameter decode_parameter(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU parameter reader is null");
  }
  IsolatedCpuScalarParameter parameter;
  parameter.name = reader->get_string(kMaximumIsolatedCpuParameterNameBytes);
  parameter.kind = parse_scalar_kind(reader->get_u8());
  switch (parameter.kind) {
    case IsolatedCpuScalarKind::Boolean:
      parameter.boolean_value = reader->get_bool();
      break;
    case IsolatedCpuScalarKind::SignedInteger:
      parameter.signed_value = reader->get_i64();
      break;
    case IsolatedCpuScalarKind::UnsignedInteger:
      parameter.unsigned_value = reader->get_u64();
      break;
    case IsolatedCpuScalarKind::FloatingPoint:
      parameter.floating_value = reader->get_f64();
      break;
    case IsolatedCpuScalarKind::String:
      parameter.string_value =
          reader->get_string(kMaximumIsolatedCpuParameterStringBytes);
      break;
  }
  return parameter;
}

/**
 * @brief Parses one closed recursive configuration kind.
 * @param value Numeric wire value.
 * @return Valid closed kind.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuConfigurationKind parse_configuration_kind(std::uint8_t value) {
  switch (static_cast<IsolatedCpuConfigurationKind>(value)) {
    case IsolatedCpuConfigurationKind::Null:
    case IsolatedCpuConfigurationKind::Boolean:
    case IsolatedCpuConfigurationKind::SignedInteger:
    case IsolatedCpuConfigurationKind::FloatingPoint:
    case IsolatedCpuConfigurationKind::String:
    case IsolatedCpuConfigurationKind::Bytes:
    case IsolatedCpuConfigurationKind::Array:
    case IsolatedCpuConfigurationKind::Object:
      return static_cast<IsolatedCpuConfigurationKind>(value);
  }
  fail("isolated CPU configuration kind is invalid");
}

/**
 * @brief Encodes one flattened recursive configuration node.
 * @param node Canonical node whose inactive fields remain zero/empty.
 * @param writer Nonnull bounded payload writer.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding bound failures unchanged.
 */
void encode_configuration_node(const IsolatedCpuConfigurationNode& node,
                               ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument(
        "isolated CPU configuration-node writer is null");
  }
  writer->put_u8(static_cast<std::uint8_t>(node.kind));
  writer->put_string(node.key, kMaximumIsolatedCpuConfigurationKeyBytes);
  writer->put_bool(node.boolean_value);
  writer->put_i64(node.signed_value);
  writer->put_f64(node.floating_value);
  writer->put_string(node.bytes_value,
                     kMaximumIsolatedCpuConfigurationValueBytes);
  writer->put_u32(node.first_child);
  writer->put_u32(node.child_count);
}

/**
 * @brief Decodes one flattened recursive configuration node.
 * @param reader Nonnull bounded payload reader.
 * @return Fresh node pending whole-tree semantic validation.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError or std::bad_alloc for malformed content.
 */
IsolatedCpuConfigurationNode decode_configuration_node(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument(
        "isolated CPU configuration-node reader is null");
  }
  IsolatedCpuConfigurationNode node;
  node.kind = parse_configuration_kind(reader->get_u8());
  node.key = reader->get_string(kMaximumIsolatedCpuConfigurationKeyBytes);
  node.boolean_value = reader->get_bool();
  node.signed_value = reader->get_i64();
  node.floating_value = reader->get_f64();
  node.bytes_value =
      reader->get_string(kMaximumIsolatedCpuConfigurationValueBytes);
  node.first_child = reader->get_u32();
  node.child_count = reader->get_u32();
  return node;
}

/**
 * @brief Parses one closed capability access mode.
 * @param value Numeric wire value.
 * @return Valid access mode.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuCapabilityAccess parse_capability_access(std::uint8_t value) {
  switch (static_cast<IsolatedCpuCapabilityAccess>(value)) {
    case IsolatedCpuCapabilityAccess::ReadOnly:
    case IsolatedCpuCapabilityAccess::ReadWrite:
      return static_cast<IsolatedCpuCapabilityAccess>(value);
  }
  fail("isolated CPU invocation capability access is invalid");
}

/**
 * @brief Encodes one ordered capability declaration.
 * @param capability Validated declaration.
 * @param writer Non-null output owner.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding bound errors unchanged.
 */
void encode_capability(const IsolatedCpuCapability& capability,
                       ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU capability writer is null");
  }
  writer->put_u64(capability.capability_id);
  writer->put_u8(static_cast<std::uint8_t>(capability.access));
  writer->put_u64(capability.byte_size);
}

/**
 * @brief Decodes one ordered capability declaration.
 * @param reader Non-null payload reader.
 * @return Newly reconstructed declaration.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for malformed content.
 */
IsolatedCpuCapability decode_capability(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU capability reader is null");
  }
  IsolatedCpuCapability capability;
  capability.capability_id = reader->get_u64();
  capability.access = parse_capability_access(reader->get_u8());
  capability.byte_size = reader->get_u64();
  return capability;
}

/**
 * @brief Parses one closed tensor representation kind.
 * @param value Numeric wire value.
 * @return Valid kind.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuTensorKind parse_tensor_kind(std::uint8_t value) {
  if (value != static_cast<std::uint8_t>(IsolatedCpuTensorKind::DenseTensor)) {
    fail("isolated CPU invocation tensor kind is invalid");
  }
  return IsolatedCpuTensorKind::DenseTensor;
}

/**
 * @brief Parses one closed physical layout kind.
 * @param value Numeric wire value.
 * @return Valid kind.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuLayoutKind parse_layout_kind(std::uint8_t value) {
  if (value != static_cast<std::uint8_t>(IsolatedCpuLayoutKind::Strided)) {
    fail("isolated CPU invocation layout kind is invalid");
  }
  return IsolatedCpuLayoutKind::Strided;
}

/**
 * @brief Parses one closed tensor direction.
 * @param value Numeric wire value.
 * @return Valid direction.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuTensorAccess parse_tensor_access(std::uint8_t value) {
  switch (static_cast<IsolatedCpuTensorAccess>(value)) {
    case IsolatedCpuTensorAccess::InputReadOnly:
    case IsolatedCpuTensorAccess::OutputWriteOnly:
      return static_cast<IsolatedCpuTensorAccess>(value);
  }
  fail("isolated CPU invocation tensor access is invalid");
}

/**
 * @brief Parses one closed descriptor readiness state.
 * @param value Numeric wire value.
 * @return Valid state.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuTensorReadiness parse_tensor_readiness(std::uint8_t value) {
  switch (static_cast<IsolatedCpuTensorReadiness>(value)) {
    case IsolatedCpuTensorReadiness::ReadyInput:
    case IsolatedCpuTensorReadiness::WritableOutput:
    case IsolatedCpuTensorReadiness::ReadyOutputCandidate:
      return static_cast<IsolatedCpuTensorReadiness>(value);
  }
  fail("isolated CPU invocation tensor readiness is invalid");
}

/**
 * @brief Parses one closed descriptor ownership claim.
 * @param value Numeric wire value.
 * @return Valid claim.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuTensorOwnership parse_tensor_ownership(std::uint8_t value) {
  switch (static_cast<IsolatedCpuTensorOwnership>(value)) {
    case IsolatedCpuTensorOwnership::HostInput:
    case IsolatedCpuTensorOwnership::RuntimeOutput:
    case IsolatedCpuTensorOwnership::HostOutputCandidate:
      return static_cast<IsolatedCpuTensorOwnership>(value);
  }
  fail("isolated CPU invocation tensor ownership is invalid");
}

/**
 * @brief Parses one closed logical element semantics value.
 * @param value Numeric wire value.
 * @return Valid semantics.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuElementSemantics parse_element_semantics(std::uint8_t value) {
  switch (static_cast<IsolatedCpuElementSemantics>(value)) {
    case IsolatedCpuElementSemantics::UnsignedInteger:
    case IsolatedCpuElementSemantics::SignedInteger:
    case IsolatedCpuElementSemantics::FloatingPoint:
      return static_cast<IsolatedCpuElementSemantics>(value);
  }
  fail("isolated CPU invocation element semantics is invalid");
}

/**
 * @brief Parses the closed scalar storage encoding.
 * @param value Numeric wire value.
 * @return NativeScalar for the sole supported value.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuStorageEncoding parse_storage_encoding(std::uint8_t value) {
  if (value !=
      static_cast<std::uint8_t>(IsolatedCpuStorageEncoding::NativeScalar)) {
    fail("isolated CPU invocation storage encoding is invalid");
  }
  return IsolatedCpuStorageEncoding::NativeScalar;
}

/**
 * @brief Encodes one optional canonical ContentDigest.
 * @param digest Optional digest.
 * @param writer Non-null output owner.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding bound errors unchanged.
 */
void encode_content_binding(const std::optional<ContentDigest>& digest,
                            ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU digest writer is null");
  }
  writer->put_bool(digest.has_value());
  if (!digest.has_value()) {
    return;
  }
  writer->put_u32(static_cast<std::uint32_t>(digest->algorithm));
  writer->put_raw(digest->bytes.data(), digest->bytes.size());
}

/**
 * @brief Decodes one optional approved ContentDigest.
 * @param reader Non-null payload reader.
 * @return Optional exact digest.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for an unsupported algorithm or truncation.
 */
std::optional<ContentDigest> decode_content_binding(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU digest reader is null");
  }
  if (!reader->get_bool()) {
    return std::nullopt;
  }
  if (reader->get_u32() !=
      static_cast<std::uint32_t>(CanonicalDigestAlgorithm::Sha256CanonicalV1)) {
    fail("isolated CPU invocation digest algorithm is unsupported");
  }
  ContentDigest digest;
  digest.algorithm = CanonicalDigestAlgorithm::Sha256CanonicalV1;
  reader->get_raw(digest.bytes.data(), digest.bytes.size());
  return digest;
}

/**
 * @brief Reads one bounded vector count before allocation.
 * @param reader Nonnull payload reader.
 * @param maximum Inclusive count maximum.
 * @param label Stable diagnostic label.
 * @return Valid local count.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for a value above maximum.
 */
std::size_t decode_count(ByteReader* reader, std::size_t maximum,
                         const char* label);

/**
 * @brief Encodes one signed half-open image window.
 * @param bounds Complete public window.
 * @param writer Nonnull bounded payload writer.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding bound failures unchanged.
 */
void encode_image_bounds(const ImageBounds& bounds, ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU image-bounds writer is null");
  }
  writer->put_i64(bounds.x_begin);
  writer->put_i64(bounds.y_begin);
  writer->put_i64(bounds.x_end);
  writer->put_i64(bounds.y_end);
}

/**
 * @brief Decodes one signed half-open image window.
 * @param reader Nonnull bounded payload reader.
 * @return Exact reconstructed endpoints.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for truncation.
 */
ImageBounds decode_image_bounds(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU image-bounds reader is null");
  }
  ImageBounds bounds;
  bounds.x_begin = reader->get_i64();
  bounds.y_begin = reader->get_i64();
  bounds.x_end = reader->get_i64();
  bounds.y_end = reader->get_i64();
  return bounds;
}

/**
 * @brief Encodes one complete finite declared sample interval.
 * @param domain Validated sample-domain record.
 * @param writer Nonnull bounded payload writer.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding bound failures unchanged.
 */
void encode_sample_domain(const SampleDomain& domain, ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU sample-domain writer is null");
  }
  writer->put_u32(static_cast<std::uint32_t>(domain.kind));
  writer->put_f64(domain.minimum);
  writer->put_f64(domain.maximum);
}

/**
 * @brief Decodes one declared sample interval with a closed kind.
 * @param reader Nonnull bounded payload reader.
 * @return Reconstructed interval; enclosing metadata validation checks range.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for an unknown kind or truncation.
 */
SampleDomain decode_sample_domain(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU sample-domain reader is null");
  }
  SampleDomain domain;
  switch (reader->get_u32()) {
    case static_cast<std::uint32_t>(SampleDomainKind::Normalized):
      domain.kind = SampleDomainKind::Normalized;
      break;
    case static_cast<std::uint32_t>(SampleDomainKind::Legal):
      domain.kind = SampleDomainKind::Legal;
      break;
    case static_cast<std::uint32_t>(SampleDomainKind::CodeValue):
      domain.kind = SampleDomainKind::CodeValue;
      break;
    default:
      fail("isolated CPU sample-domain kind is invalid");
  }
  domain.minimum = reader->get_f64();
  domain.maximum = reader->get_f64();
  return domain;
}

/**
 * @brief Encodes a complete rich ordinary-image facet without addresses.
 * @param facet Validated image facts.
 * @param writer Nonnull bounded payload writer.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding allocation or bound failures unchanged.
 */
void encode_image_facet(const IsolatedCpuImageFacet& facet,
                        ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU image-facet writer is null");
  }
  writer->put_u32(facet.x_axis);
  writer->put_u32(facet.y_axis);
  writer->put_bool(facet.channel_axis.has_value());
  if (facet.channel_axis.has_value()) {
    writer->put_u32(*facet.channel_axis);
  }
  encode_image_bounds(facet.data_window, writer);
  writer->put_bool(facet.display_window.has_value());
  if (facet.display_window.has_value()) {
    encode_image_bounds(*facet.display_window, writer);
  }
  writer->put_bool(facet.channel_schema.has_value());
  if (facet.channel_schema.has_value()) {
    writer->put_u32(
        static_cast<std::uint32_t>(facet.channel_schema->channels.size()));
    for (const ChannelDescription& channel : facet.channel_schema->channels) {
      writer->put_u64(channel.id.value);
      writer->put_string(channel.diagnostic_name,
                         kMaximumImageDiagnosticNameBytes);
    }
    writer->put_u32(
        static_cast<std::uint32_t>(facet.channel_schema->groups.size()));
    for (const ChannelGroupDescription& group : facet.channel_schema->groups) {
      writer->put_u64(group.id.value);
      writer->put_string(group.diagnostic_name,
                         kMaximumImageDiagnosticNameBytes);
      writer->put_u32(static_cast<std::uint32_t>(group.members.size()));
      for (const ChannelId member : group.members) {
        writer->put_u64(member.value);
      }
    }
  }
  writer->put_bool(facet.sample_domain.has_value());
  if (facet.sample_domain.has_value()) {
    const SampleDomainFacet& sample = *facet.sample_domain;
    writer->put_u32(sample.structural_version);
    writer->put_u32(sample.encoding.structural_version);
    writer->put_u32(static_cast<std::uint32_t>(sample.encoding.kind));
    encode_sample_domain(sample.default_domain, writer);
    writer->put_u32(static_cast<std::uint32_t>(sample.per_channel.size()));
    for (const ChannelSampleDomain& per_channel : sample.per_channel) {
      writer->put_u64(per_channel.channel.value);
      encode_sample_domain(per_channel.domain, writer);
    }
  }
  writer->put_bool(facet.color.has_value());
  if (facet.color.has_value()) {
    writer->put_u32(facet.color->structural_version);
    writer->put_u64(facet.color->channel_group.value);
    writer->put_u32(static_cast<std::uint32_t>(facet.color->transfer));
    writer->put_u32(static_cast<std::uint32_t>(facet.color->primaries));
  }
}

/**
 * @brief Decodes one complete bounded ordinary-image facet.
 * @param reader Nonnull bounded payload reader.
 * @return Fresh Host/runtime-owned metadata.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError or std::bad_alloc for malformed content.
 */
IsolatedCpuImageFacet decode_image_facet(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU image-facet reader is null");
  }
  IsolatedCpuImageFacet facet;
  facet.x_axis = reader->get_u32();
  facet.y_axis = reader->get_u32();
  if (reader->get_bool()) {
    facet.channel_axis = reader->get_u32();
  }
  facet.data_window = decode_image_bounds(reader);
  if (reader->get_bool()) {
    facet.display_window = decode_image_bounds(reader);
  }
  if (reader->get_bool()) {
    ChannelSchema schema;
    const std::size_t channel_count =
        decode_count(reader, kMaximumImageChannels, "image channel count");
    schema.channels.reserve(channel_count);
    for (std::size_t index = 0U; index < channel_count; ++index) {
      ChannelDescription channel;
      channel.id.value = reader->get_u64();
      channel.diagnostic_name =
          reader->get_string(kMaximumImageDiagnosticNameBytes);
      schema.channels.push_back(std::move(channel));
    }
    const std::size_t group_count = decode_count(
        reader, kMaximumImageChannelGroups, "image channel-group count");
    schema.groups.reserve(group_count);
    std::size_t memberships = 0U;
    for (std::size_t index = 0U; index < group_count; ++index) {
      ChannelGroupDescription group;
      group.id.value = reader->get_u64();
      group.diagnostic_name =
          reader->get_string(kMaximumImageDiagnosticNameBytes);
      const std::size_t member_count =
          decode_count(reader, kMaximumImageChannelGroupMembers,
                       "image channel-group member count");
      if (member_count > kMaximumImageChannelGroupMemberships - memberships) {
        fail("isolated CPU image channel memberships exceed their bound");
      }
      memberships += member_count;
      group.members.reserve(member_count);
      for (std::size_t member = 0U; member < member_count; ++member) {
        group.members.push_back(ChannelId{reader->get_u64()});
      }
      schema.groups.push_back(std::move(group));
    }
    facet.channel_schema = std::move(schema);
  }
  if (reader->get_bool()) {
    SampleDomainFacet sample;
    sample.structural_version = reader->get_u32();
    sample.encoding.structural_version = reader->get_u32();
    switch (reader->get_u32()) {
      case static_cast<std::uint32_t>(SampleEncodingKind::Value):
        sample.encoding.kind = SampleEncodingKind::Value;
        break;
      case static_cast<std::uint32_t>(SampleEncodingKind::Normalized):
        sample.encoding.kind = SampleEncodingKind::Normalized;
        break;
      case static_cast<std::uint32_t>(SampleEncodingKind::CodeValue):
        sample.encoding.kind = SampleEncodingKind::CodeValue;
        break;
      default:
        fail("isolated CPU sample encoding is invalid");
    }
    sample.default_domain = decode_sample_domain(reader);
    const std::size_t count = decode_count(reader, kMaximumImageChannels,
                                           "per-channel sample-domain count");
    sample.per_channel.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
      ChannelSampleDomain domain;
      domain.channel.value = reader->get_u64();
      domain.domain = decode_sample_domain(reader);
      sample.per_channel.push_back(std::move(domain));
    }
    facet.sample_domain = std::move(sample);
  }
  if (reader->get_bool()) {
    ColorFacet color;
    color.structural_version = reader->get_u32();
    color.channel_group.value = reader->get_u64();
    switch (reader->get_u32()) {
      case static_cast<std::uint32_t>(ColorTransferFunction::SceneLinear):
        color.transfer = ColorTransferFunction::SceneLinear;
        break;
      case static_cast<std::uint32_t>(ColorTransferFunction::Srgb):
        color.transfer = ColorTransferFunction::Srgb;
        break;
      case static_cast<std::uint32_t>(ColorTransferFunction::Rec709):
        color.transfer = ColorTransferFunction::Rec709;
        break;
      case static_cast<std::uint32_t>(ColorTransferFunction::Pq):
        color.transfer = ColorTransferFunction::Pq;
        break;
      case static_cast<std::uint32_t>(ColorTransferFunction::Hlg):
        color.transfer = ColorTransferFunction::Hlg;
        break;
      default:
        fail("isolated CPU color transfer is invalid");
    }
    switch (reader->get_u32()) {
      case static_cast<std::uint32_t>(ColorPrimaries::Rec709):
        color.primaries = ColorPrimaries::Rec709;
        break;
      case static_cast<std::uint32_t>(ColorPrimaries::DisplayP3D65):
        color.primaries = ColorPrimaries::DisplayP3D65;
        break;
      case static_cast<std::uint32_t>(ColorPrimaries::Rec2020):
        color.primaries = ColorPrimaries::Rec2020;
        break;
      case static_cast<std::uint32_t>(ColorPrimaries::AcesAp0):
        color.primaries = ColorPrimaries::AcesAp0;
        break;
      case static_cast<std::uint32_t>(ColorPrimaries::AcesAp1):
        color.primaries = ColorPrimaries::AcesAp1;
        break;
      default:
        fail("isolated CPU color primaries are invalid");
    }
    facet.color = color;
  }
  return facet;
}

/**
 * @brief Encodes one canonical RegionSet without process-local state.
 * @param region Valid canonical Region.
 * @param writer Nonnull bounded payload writer.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding bound failures unchanged.
 */
void encode_region(const RegionSet& region, ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU Region writer is null");
  }
  writer->put_u8(static_cast<std::uint8_t>(region.kind()));
  writer->put_u32(static_cast<std::uint32_t>(region.atoms().size()));
  for (const RegionAtom& atom : region.atoms()) {
    if (const auto* rect = std::get_if<ImageRect>(&atom)) {
      writer->put_u8(1U);
      writer->put_u64(rect->domain.high);
      writer->put_u64(rect->domain.low);
      writer->put_i64(rect->x_begin);
      writer->put_i64(rect->x_end);
      writer->put_i64(rect->y_begin);
      writer->put_i64(rect->y_end);
      continue;
    }
    const auto& slice = std::get<TensorSlice>(atom);
    writer->put_u8(2U);
    writer->put_u64(slice.domain.high);
    writer->put_u64(slice.domain.low);
    writer->put_u32(static_cast<std::uint32_t>(slice.axes.size()));
    for (const RegionInterval& interval : slice.axes) {
      writer->put_u64(interval.begin);
      writer->put_u64(interval.end);
    }
  }
}

/**
 * @brief Decodes and canonicalizes one bounded RegionSet.
 * @param reader Nonnull bounded payload reader.
 * @return Fresh canonical Region.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError or std::bad_alloc for malformed content.
 */
RegionSet decode_region(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU Region reader is null");
  }
  const std::uint8_t kind = reader->get_u8();
  const std::size_t atom_count =
      decode_count(reader, RegionSet::kMaximumAtoms, "Region atom count");
  if (kind == static_cast<std::uint8_t>(RegionSet::Kind::Empty)) {
    if (atom_count != 0U) {
      fail("isolated CPU Empty Region carries atoms");
    }
    return RegionSet::empty();
  }
  if (kind == static_cast<std::uint8_t>(RegionSet::Kind::Whole)) {
    if (atom_count != 0U) {
      fail("isolated CPU Whole Region carries atoms");
    }
    return RegionSet::whole();
  }
  if (kind != static_cast<std::uint8_t>(RegionSet::Kind::Clause) ||
      atom_count == 0U) {
    fail("isolated CPU Region kind or atom count is invalid");
  }
  std::vector<RegionAtom> atoms;
  atoms.reserve(atom_count);
  for (std::size_t index = 0U; index < atom_count; ++index) {
    const std::uint8_t atom_kind = reader->get_u8();
    RegionDomainKey domain{reader->get_u64(), reader->get_u64()};
    if (atom_kind == 1U) {
      atoms.push_back(ImageRect{domain, reader->get_i64(), reader->get_i64(),
                                reader->get_i64(), reader->get_i64()});
      continue;
    }
    if (atom_kind != 2U) {
      fail("isolated CPU Region atom kind is invalid");
    }
    const std::size_t rank =
        decode_count(reader, kMaximumIsolatedCpuTensorRank, "Region rank");
    TensorSlice slice;
    slice.domain = domain;
    slice.axes.reserve(rank);
    for (std::size_t axis = 0U; axis < rank; ++axis) {
      slice.axes.push_back(
          RegionInterval{reader->get_u64(), reader->get_u64()});
    }
    atoms.push_back(std::move(slice));
  }
  try {
    RegionSet normalized = RegionSet::from_atoms(std::move(atoms));
    if (normalized.kind() != RegionSet::Kind::Clause) {
      fail("isolated CPU Region clause is not canonical nonempty data");
    }
    return normalized;
  } catch (const IsolatedCpuProtocolError&) {
    throw;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("isolated CPU Region is invalid: ") + error.what());
  }
}

/**
 * @brief Encodes one complete tensor descriptor.
 * @param descriptor Validated descriptor.
 * @param writer Non-null output owner.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding allocation or bound errors unchanged.
 */
void encode_tensor(const IsolatedCpuTensorDescriptor& descriptor,
                   ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU tensor writer is null");
  }
  writer->put_u16(descriptor.descriptor_version);
  writer->put_u8(static_cast<std::uint8_t>(descriptor.kind));
  writer->put_u8(static_cast<std::uint8_t>(descriptor.layout_kind));
  writer->put_u8(static_cast<std::uint8_t>(descriptor.access));
  writer->put_u8(static_cast<std::uint8_t>(descriptor.readiness));
  writer->put_u8(static_cast<std::uint8_t>(descriptor.ownership));
  encode_opaque_id(descriptor.port_identity, writer);
  encode_opaque_id(descriptor.binding_identity, writer);
  encode_opaque_id(descriptor.schema_identity, writer);
  encode_opaque_id(descriptor.facet_identity, writer);
  encode_opaque_id(descriptor.layout_identity, writer);
  writer->put_u64(descriptor.schema_version);
  writer->put_u64(descriptor.layout_version);
  writer->put_u64(descriptor.capability_id);
  writer->put_u64(descriptor.capability_offset);
  writer->put_u64(descriptor.capability_length);
  writer->put_u8(static_cast<std::uint8_t>(descriptor.element_semantics));
  writer->put_u8(static_cast<std::uint8_t>(descriptor.storage_encoding));
  writer->put_u32(descriptor.bit_width);
  writer->put_u32(static_cast<std::uint32_t>(descriptor.extents.size()));
  for (const std::uint64_t extent : descriptor.extents) {
    writer->put_u64(extent);
  }
  writer->put_u32(static_cast<std::uint32_t>(descriptor.byte_strides.size()));
  for (const std::int64_t stride : descriptor.byte_strides) {
    writer->put_i64(stride);
  }
  writer->put_u64(descriptor.byte_offset);
  writer->put_bool(descriptor.image_facet.has_value());
  if (descriptor.image_facet.has_value()) {
    encode_image_facet(*descriptor.image_facet, writer);
  }
  encode_region(descriptor.region, writer);
  writer->put_u64(descriptor.allocation_alignment);
  writer->put_u64(descriptor.written_offset);
  writer->put_u64(descriptor.written_length);
  encode_content_binding(descriptor.content_binding, writer);
}

/**
 * @brief Reads one bounded vector count before allocation.
 * @param reader Non-null payload reader.
 * @param maximum Inclusive count maximum.
 * @param label Stable diagnostic label.
 * @return Valid local count.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for a value above maximum.
 */
std::size_t decode_count(ByteReader* reader, std::size_t maximum,
                         const char* label) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU count reader is null");
  }
  const std::size_t count = reader->get_u32();
  if (count > maximum) {
    fail(std::string("isolated CPU invocation ") + label +
         " exceeds its bound");
  }
  return count;
}

/**
 * @brief Decodes one complete bounded tensor descriptor.
 * @param reader Non-null payload reader.
 * @return Newly reconstructed descriptor.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError or std::bad_alloc from bounded decoding.
 */
IsolatedCpuTensorDescriptor decode_tensor(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU tensor reader is null");
  }
  IsolatedCpuTensorDescriptor descriptor;
  descriptor.descriptor_version = reader->get_u16();
  descriptor.kind = parse_tensor_kind(reader->get_u8());
  descriptor.layout_kind = parse_layout_kind(reader->get_u8());
  descriptor.access = parse_tensor_access(reader->get_u8());
  descriptor.readiness = parse_tensor_readiness(reader->get_u8());
  descriptor.ownership = parse_tensor_ownership(reader->get_u8());
  descriptor.port_identity = decode_opaque_id(reader);
  descriptor.binding_identity = decode_opaque_id(reader);
  descriptor.schema_identity = decode_opaque_id(reader);
  descriptor.facet_identity = decode_opaque_id(reader);
  descriptor.layout_identity = decode_opaque_id(reader);
  descriptor.schema_version = reader->get_u64();
  descriptor.layout_version = reader->get_u64();
  descriptor.capability_id = reader->get_u64();
  descriptor.capability_offset = reader->get_u64();
  descriptor.capability_length = reader->get_u64();
  descriptor.element_semantics = parse_element_semantics(reader->get_u8());
  descriptor.storage_encoding = parse_storage_encoding(reader->get_u8());
  descriptor.bit_width = reader->get_u32();
  const std::size_t rank =
      decode_count(reader, kMaximumIsolatedCpuTensorRank, "tensor rank");
  descriptor.extents.reserve(rank);
  for (std::size_t axis = 0U; axis < rank; ++axis) {
    descriptor.extents.push_back(reader->get_u64());
  }
  const std::size_t stride_count = decode_count(
      reader, kMaximumIsolatedCpuTensorRank, "tensor stride count");
  descriptor.byte_strides.reserve(stride_count);
  for (std::size_t axis = 0U; axis < stride_count; ++axis) {
    descriptor.byte_strides.push_back(reader->get_i64());
  }
  descriptor.byte_offset = reader->get_u64();
  if (reader->get_bool()) {
    descriptor.image_facet = decode_image_facet(reader);
  }
  descriptor.region = decode_region(reader);
  descriptor.allocation_alignment = reader->get_u64();
  descriptor.written_offset = reader->get_u64();
  descriptor.written_length = reader->get_u64();
  descriptor.content_binding = decode_content_binding(reader);
  return descriptor;
}

/**
 * @brief Encodes one exact resource declaration.
 * @param resources Declaration to encode.
 * @param writer Non-null output owner.
 * @throws std::invalid_argument for a null writer.
 * @throws Encoding bound errors unchanged.
 */
void encode_resources(const IsolatedCpuResourceDeclaration& resources,
                      ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("isolated CPU resource writer is null");
  }
  writer->put_u64(resources.shared_memory_bytes);
  writer->put_u32(resources.descriptor_count);
  writer->put_u32(resources.cpu_slots);
}

/**
 * @brief Decodes one exact resource declaration.
 * @param reader Non-null payload reader.
 * @return Reconstructed declaration.
 * @throws std::invalid_argument for a null reader.
 * @throws IsolatedCpuProtocolError for truncation.
 */
IsolatedCpuResourceDeclaration decode_resources(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("isolated CPU resource reader is null");
  }
  IsolatedCpuResourceDeclaration resources;
  resources.shared_memory_bytes = reader->get_u64();
  resources.descriptor_count = reader->get_u32();
  resources.cpu_slots = reader->get_u32();
  return resources;
}

/**
 * @brief Parses one closed callback outcome.
 * @param value Numeric wire value.
 * @return Valid outcome.
 * @throws IsolatedCpuProtocolError for an unknown value.
 */
IsolatedCpuInvocationOutcome parse_outcome(std::uint8_t value) {
  switch (static_cast<IsolatedCpuInvocationOutcome>(value)) {
    case IsolatedCpuInvocationOutcome::Succeeded:
    case IsolatedCpuInvocationOutcome::PluginFailed:
    case IsolatedCpuInvocationOutcome::Cancelled:
      return static_cast<IsolatedCpuInvocationOutcome>(value);
  }
  fail("isolated CPU invocation outcome is invalid");
}

/**
 * @brief Converts one supported element descriptor into bytes per scalar.
 * @param descriptor Tensor descriptor to inspect.
 * @return One, two, four, or eight bytes.
 * @throws IsolatedCpuProtocolError for an unsupported combination.
 */
std::uint64_t element_bytes(const IsolatedCpuTensorDescriptor& descriptor) {
  if (descriptor.storage_encoding != IsolatedCpuStorageEncoding::NativeScalar) {
    fail("isolated CPU invocation tensor encoding is unsupported");
  }
  switch (descriptor.element_semantics) {
    case IsolatedCpuElementSemantics::UnsignedInteger:
    case IsolatedCpuElementSemantics::SignedInteger:
      if (descriptor.bit_width == 8U || descriptor.bit_width == 16U) {
        return descriptor.bit_width / 8U;
      }
      break;
    case IsolatedCpuElementSemantics::FloatingPoint:
      if (descriptor.bit_width == 32U || descriptor.bit_width == 64U) {
        return descriptor.bit_width / 8U;
      }
      break;
  }
  fail("isolated CPU invocation tensor element type is unsupported");
}

/**
 * @brief Complete checked signed-layout address envelope.
 * @throws Nothing for ordinary value operations.
 */
struct AddressEnvelope final {
  /** @brief Maximum addressed distance below logical origin. */
  std::uint64_t lower_extent = 0U;
  /** @brief Exclusive highest addressed byte relative to range start. */
  std::uint64_t upper_end = 0U;
};

/**
 * @brief Computes the complete checked signed layout envelope.
 * @param descriptor Rank-matched positive-shape descriptor.
 * @param scalar_bytes Positive element byte width.
 * @return Lower magnitude and exclusive upper end.
 * @throws IsolatedCpuProtocolError for rank, underflow, or arithmetic errors.
 */
AddressEnvelope compute_address_envelope(
    const IsolatedCpuTensorDescriptor& descriptor, std::uint64_t scalar_bytes) {
  if (descriptor.extents.empty() ||
      descriptor.extents.size() != descriptor.byte_strides.size()) {
    fail("isolated CPU invocation tensor rank and strides disagree");
  }
  std::uint64_t lower = 0U;
  std::uint64_t upper = 0U;
  for (std::size_t axis = 0U; axis < descriptor.extents.size(); ++axis) {
    if (descriptor.extents[axis] == 0U) {
      fail("isolated CPU invocation tensor extent is zero");
    }
    const std::uint64_t axis_extent =
        checked_multiply(descriptor.extents[axis] - 1U,
                         signed_magnitude(descriptor.byte_strides[axis]));
    if (descriptor.byte_strides[axis] < 0) {
      lower = checked_add(lower, axis_extent);
    } else {
      upper = checked_add(upper, axis_extent);
    }
  }
  if (descriptor.byte_offset < lower) {
    fail("isolated CPU invocation signed layout underflows its range");
  }
  return AddressEnvelope{
      lower,
      checked_add(checked_add(descriptor.byte_offset, upper), scalar_bytes)};
}

/**
 * @brief Proves rank-general non-overlap for a positive output layout.
 * @param descriptor Valid positive-stride output descriptor.
 * @param scalar_bytes Positive element byte width.
 * @throws IsolatedCpuProtocolError when disjoint writes cannot be proven.
 * @throws std::bad_alloc when bounded rank state cannot allocate.
 */
void validate_non_overlapping_output(
    const IsolatedCpuTensorDescriptor& descriptor, std::uint64_t scalar_bytes) {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> axes;
  axes.reserve(descriptor.extents.size());
  for (std::size_t axis = 0U; axis < descriptor.extents.size(); ++axis) {
    if (descriptor.extents[axis] > 1U) {
      axes.emplace_back(
          static_cast<std::uint64_t>(descriptor.byte_strides[axis]),
          descriptor.extents[axis]);
    }
  }
  std::sort(axes.begin(), axes.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });
  std::uint64_t covered = scalar_bytes;
  for (const auto& [stride, extent] : axes) {
    if (stride < covered) {
      fail("isolated CPU invocation output writes overlap");
    }
    covered = checked_add(covered, checked_multiply(extent - 1U, stride));
  }
}

/**
 * @brief Closed validation phase for descriptor state fields.
 * @throws Nothing for ordinary enum operations.
 */
enum class TensorPhase : std::uint8_t {
  /** @brief Ready request input. */
  RequestInput,
  /** @brief Writable request output. */
  RequestOutput,
  /** @brief Ready response output candidate. */
  ResponseOutput,
};

/**
 * @brief Validates every nonzero identity component.
 * @param identity Complete tuple.
 * @throws IsolatedCpuProtocolError for any zero component or generation.
 */
void validate_identity(const IsolatedCpuInvocationIdentity& identity) {
  if (!identity.tenant_id.valid() || !identity.job_id.valid() ||
      !identity.attempt_id.valid() || !identity.worker_id.valid() ||
      identity.worker_lease_generation == 0U ||
      !identity.plugin_package_id.valid() || identity.plugin_generation == 0U ||
      !identity.invocation_id.valid()) {
    fail("isolated CPU invocation identity tuple is incomplete");
  }
}

/**
 * @brief Validates canonical active and inactive scalar fields.
 * @param parameter Parameter to inspect.
 * @throws IsolatedCpuProtocolError for invalid text, kind, finite state, or
 * noncanonical inactive content.
 */
void validate_parameter(const IsolatedCpuScalarParameter& parameter) {
  validate_text(parameter.name, kMaximumIsolatedCpuParameterNameBytes,
                "parameter name");
  const bool floating_zero = parameter.floating_value == 0.0 &&
                             !std::signbit(parameter.floating_value);
  switch (parameter.kind) {
    case IsolatedCpuScalarKind::Boolean:
      if (parameter.signed_value != 0 || parameter.unsigned_value != 0U ||
          !floating_zero || !parameter.string_value.empty()) {
        fail("isolated CPU boolean parameter has noncanonical inactive fields");
      }
      return;
    case IsolatedCpuScalarKind::SignedInteger:
      if (parameter.boolean_value || parameter.unsigned_value != 0U ||
          !floating_zero || !parameter.string_value.empty()) {
        fail("isolated CPU signed parameter has noncanonical inactive fields");
      }
      return;
    case IsolatedCpuScalarKind::UnsignedInteger:
      if (parameter.boolean_value || parameter.signed_value != 0 ||
          !floating_zero || !parameter.string_value.empty()) {
        fail(
            "isolated CPU unsigned parameter has noncanonical inactive fields");
      }
      return;
    case IsolatedCpuScalarKind::FloatingPoint:
      if (parameter.boolean_value || parameter.signed_value != 0 ||
          parameter.unsigned_value != 0U || !parameter.string_value.empty() ||
          !std::isfinite(parameter.floating_value)) {
        fail("isolated CPU floating parameter is invalid or noncanonical");
      }
      return;
    case IsolatedCpuScalarKind::String:
      if (parameter.boolean_value || parameter.signed_value != 0 ||
          parameter.unsigned_value != 0U || !floating_zero ||
          parameter.string_value.size() >
              kMaximumIsolatedCpuParameterStringBytes ||
          parameter.string_value.find('\0') != std::string::npos) {
        fail("isolated CPU string parameter is invalid or noncanonical");
      }
      return;
  }
  fail("isolated CPU invocation scalar kind is invalid");
}

/**
 * @brief Validates one node and recursively proves its canonical child tree.
 * @param nodes Complete flattened configuration inventory.
 * @param index Current node index.
 * @param depth Root-inclusive current depth.
 * @param seen Nonnull exact-size visitation state.
 * @throws std::invalid_argument for a null or mismatched visitation vector.
 * @throws IsolatedCpuProtocolError for inactive fields, ranges, keys,
 * duplicates, cycles, excessive depth, or malformed scalar content.
 */
void validate_configuration_subtree(
    const std::vector<IsolatedCpuConfigurationNode>& nodes, std::size_t index,
    std::size_t depth, std::vector<bool>* seen) {
  if (seen == nullptr || seen->size() != nodes.size()) {
    throw std::invalid_argument(
        "isolated CPU configuration visitation state is invalid");
  }
  if (index >= nodes.size() || (*seen)[index] ||
      depth >= kMaximumIsolatedCpuConfigurationDepth) {
    fail("isolated CPU configuration tree is cyclic or too deep");
  }
  (*seen)[index] = true;
  const IsolatedCpuConfigurationNode& node = nodes[index];
  if (node.key.size() > kMaximumIsolatedCpuConfigurationKeyBytes ||
      node.key.find('\0') != std::string::npos ||
      node.bytes_value.size() > kMaximumIsolatedCpuConfigurationValueBytes) {
    fail("isolated CPU configuration text exceeds its canonical bound");
  }
  const bool floating_zero =
      node.floating_value == 0.0 && !std::signbit(node.floating_value);
  const bool is_container = node.kind == IsolatedCpuConfigurationKind::Array ||
                            node.kind == IsolatedCpuConfigurationKind::Object;
  if (!is_container && (node.first_child != 0U || node.child_count != 0U)) {
    fail("isolated CPU configuration scalar carries child state");
  }
  switch (node.kind) {
    case IsolatedCpuConfigurationKind::Null:
      if (node.boolean_value || node.signed_value != 0 || !floating_zero ||
          !node.bytes_value.empty()) {
        fail("isolated CPU null configuration is noncanonical");
      }
      break;
    case IsolatedCpuConfigurationKind::Boolean:
      if (node.signed_value != 0 || !floating_zero ||
          !node.bytes_value.empty()) {
        fail("isolated CPU boolean configuration is noncanonical");
      }
      break;
    case IsolatedCpuConfigurationKind::SignedInteger:
      if (node.boolean_value || !floating_zero || !node.bytes_value.empty()) {
        fail("isolated CPU signed configuration is noncanonical");
      }
      break;
    case IsolatedCpuConfigurationKind::FloatingPoint:
      if (node.boolean_value || node.signed_value != 0 ||
          !std::isfinite(node.floating_value) || !node.bytes_value.empty()) {
        fail("isolated CPU floating configuration is noncanonical");
      }
      break;
    case IsolatedCpuConfigurationKind::String:
      if (node.boolean_value || node.signed_value != 0 || !floating_zero ||
          node.bytes_value.find('\0') != std::string::npos) {
        fail("isolated CPU string configuration is noncanonical");
      }
      break;
    case IsolatedCpuConfigurationKind::Bytes:
      if (node.boolean_value || node.signed_value != 0 || !floating_zero) {
        fail("isolated CPU bytes configuration is noncanonical");
      }
      break;
    case IsolatedCpuConfigurationKind::Array:
    case IsolatedCpuConfigurationKind::Object:
      if (node.boolean_value || node.signed_value != 0 || !floating_zero ||
          !node.bytes_value.empty()) {
        fail("isolated CPU container configuration is noncanonical");
      }
      break;
  }
  if (!is_container) {
    return;
  }
  if (node.child_count == 0U) {
    if (node.first_child != 0U) {
      fail("isolated CPU empty configuration container has an offset");
    }
    return;
  }
  if (node.first_child <= index || node.first_child >= nodes.size() ||
      node.child_count > nodes.size() - node.first_child) {
    fail("isolated CPU configuration child range is invalid");
  }
  std::string_view prior_key;
  for (std::size_t ordinal = 0U; ordinal < node.child_count; ++ordinal) {
    const std::size_t child_index = node.first_child + ordinal;
    const std::string& key = nodes[child_index].key;
    if (node.kind == IsolatedCpuConfigurationKind::Array) {
      if (!key.empty()) {
        fail("isolated CPU array configuration child has a key");
      }
    } else {
      if (key.empty() || (!prior_key.empty() && prior_key >= key)) {
        fail("isolated CPU object configuration keys are not sorted unique");
      }
      prior_key = key;
    }
    validate_configuration_subtree(nodes, child_index, depth + 1U, seen);
  }
}

/**
 * @brief Validates one complete optional recursive configuration.
 * @param configuration Flattened root-first configuration tree.
 * @throws IsolatedCpuProtocolError for a malformed or unreachable node.
 * @throws std::bad_alloc when bounded visitation storage cannot allocate.
 */
void validate_configuration(
    const std::vector<IsolatedCpuConfigurationNode>& configuration) {
  if (configuration.empty()) {
    return;
  }
  if (configuration.size() > kMaximumIsolatedCpuConfigurationNodes ||
      configuration.front().kind != IsolatedCpuConfigurationKind::Object ||
      !configuration.front().key.empty()) {
    fail("isolated CPU configuration root is invalid");
  }
  std::vector<bool> seen(configuration.size(), false);
  validate_configuration_subtree(configuration, 0U, 0U, &seen);
  if (std::any_of(seen.begin(), seen.end(),
                  [](bool value) { return !value; })) {
    fail("isolated CPU configuration contains unreachable nodes");
  }
}

/**
 * @brief Finds one capability in a strictly id-sorted table.
 * @param capabilities Valid sorted capability vector.
 * @param capability_id Nonzero referenced selector.
 * @return Borrowed matching declaration.
 * @throws IsolatedCpuProtocolError when no declaration matches.
 */
const IsolatedCpuCapability& find_capability(
    const std::vector<IsolatedCpuCapability>& capabilities,
    std::uint64_t capability_id) {
  const auto found = std::lower_bound(
      capabilities.begin(), capabilities.end(), capability_id,
      [](const IsolatedCpuCapability& capability, std::uint64_t id) {
        return capability.capability_id < id;
      });
  if (found == capabilities.end() || found->capability_id != capability_id) {
    fail("isolated CPU tensor references an absent capability");
  }
  return *found;
}

/**
 * @brief Validates complete optional image metadata against one tensor.
 * @param descriptor Tensor descriptor containing optional facet.
 * @throws IsolatedCpuProtocolError for malformed axes, windows, channel,
 * sample, or color metadata.
 * @throws std::bad_alloc when bounded public metadata validation allocates.
 */
void validate_image_facet(const IsolatedCpuTensorDescriptor& descriptor) {
  if (!descriptor.image_facet.has_value()) {
    return;
  }
  const IsolatedCpuImageFacet& facet = *descriptor.image_facet;
  const std::uint64_t rank = descriptor.extents.size();
  if (facet.x_axis >= rank || facet.y_axis >= rank ||
      facet.x_axis == facet.y_axis ||
      (facet.channel_axis.has_value() &&
       (*facet.channel_axis >= rank || *facet.channel_axis == facet.x_axis ||
        *facet.channel_axis == facet.y_axis))) {
    fail("isolated CPU invocation image facet is invalid");
  }
  DenseTensorDescriptor public_descriptor;
  public_descriptor.shape.reserve(descriptor.extents.size());
  for (const std::uint64_t extent : descriptor.extents) {
    if (extent > std::numeric_limits<std::size_t>::max()) {
      fail("isolated CPU image extent exceeds local representation");
    }
    public_descriptor.shape.push_back(static_cast<std::size_t>(extent));
  }
  switch (descriptor.element_semantics) {
    case IsolatedCpuElementSemantics::UnsignedInteger:
      public_descriptor.element_semantics = ElementSemantics::UnsignedInteger;
      break;
    case IsolatedCpuElementSemantics::SignedInteger:
      public_descriptor.element_semantics = ElementSemantics::SignedInteger;
      break;
    case IsolatedCpuElementSemantics::FloatingPoint:
      public_descriptor.element_semantics = ElementSemantics::FloatingPoint;
      break;
  }
  public_descriptor.storage_encoding =
      StorageEncoding{descriptor.bit_width, StorageEncodingKind::NativeScalar};
  ImageFacet public_facet;
  public_facet.x_axis = facet.x_axis;
  public_facet.y_axis = facet.y_axis;
  if (facet.channel_axis.has_value()) {
    public_facet.channel_axis = *facet.channel_axis;
  }
  public_facet.data_window = facet.data_window;
  public_facet.display_window = facet.display_window;
  public_facet.channel_schema = facet.channel_schema;
  public_facet.sample_domain = facet.sample_domain;
  public_facet.color = facet.color;
  try {
    validate_dense_tensor_image_metadata(public_descriptor, public_facet);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("isolated CPU image facet is invalid: ") + error.what());
  }
}

/**
 * @brief Validates one descriptor against its retained capability and phase.
 * @param descriptor Descriptor to inspect.
 * @param capabilities Valid sorted capability table.
 * @param phase Expected request/response state.
 * @throws IsolatedCpuProtocolError for any malformed structure or range.
 * @throws std::bad_alloc when bounded non-overlap proof cannot allocate.
 */
void validate_tensor(const IsolatedCpuTensorDescriptor& descriptor,
                     const std::vector<IsolatedCpuCapability>& capabilities,
                     TensorPhase phase) {
  if (descriptor.descriptor_version != kIsolatedCpuTensorDescriptorVersion ||
      descriptor.kind != IsolatedCpuTensorKind::DenseTensor ||
      descriptor.layout_kind != IsolatedCpuLayoutKind::Strided) {
    fail("isolated CPU invocation tensor version or kind is unsupported");
  }
  if (!descriptor.port_identity.valid() ||
      !descriptor.binding_identity.valid() ||
      !descriptor.schema_identity.valid() ||
      !descriptor.layout_identity.valid() || descriptor.schema_version == 0U ||
      descriptor.layout_version == 0U ||
      descriptor.facet_identity.valid() != descriptor.image_facet.has_value()) {
    fail("isolated CPU invocation tensor identity metadata is invalid");
  }
  if (descriptor.capability_id == 0U || descriptor.capability_length == 0U) {
    fail("isolated CPU invocation tensor capability range is empty");
  }
  if (descriptor.extents.empty() ||
      descriptor.extents.size() > kMaximumIsolatedCpuTensorRank ||
      descriptor.byte_strides.size() != descriptor.extents.size()) {
    fail("isolated CPU invocation tensor rank is invalid");
  }
  const IsolatedCpuCapability& capability =
      find_capability(capabilities, descriptor.capability_id);
  if (descriptor.capability_offset > capability.byte_size ||
      descriptor.capability_length >
          capability.byte_size - descriptor.capability_offset) {
    fail("isolated CPU invocation tensor range exceeds its capability");
  }
  const std::uint64_t scalar_bytes = element_bytes(descriptor);
  validate_image_facet(descriptor);
  const AddressEnvelope envelope =
      compute_address_envelope(descriptor, scalar_bytes);
  if (envelope.upper_end > descriptor.capability_length) {
    fail("isolated CPU invocation tensor layout exceeds its range");
  }
  if (descriptor.content_binding.has_value() &&
      descriptor.content_binding->algorithm !=
          CanonicalDigestAlgorithm::Sha256CanonicalV1) {
    fail("isolated CPU invocation tensor binding algorithm is unsupported");
  }

  switch (phase) {
    case TensorPhase::RequestInput:
      if (descriptor.access != IsolatedCpuTensorAccess::InputReadOnly ||
          descriptor.readiness != IsolatedCpuTensorReadiness::ReadyInput ||
          descriptor.ownership != IsolatedCpuTensorOwnership::HostInput ||
          capability.access != IsolatedCpuCapabilityAccess::ReadOnly ||
          !descriptor.content_binding.has_value() ||
          descriptor.allocation_alignment != 0U ||
          descriptor.written_offset != 0U || descriptor.written_length != 0U) {
        fail("isolated CPU invocation input state or permission is invalid");
      }
      return;
    case TensorPhase::RequestOutput:
      if (descriptor.access != IsolatedCpuTensorAccess::OutputWriteOnly ||
          descriptor.readiness != IsolatedCpuTensorReadiness::WritableOutput ||
          descriptor.ownership != IsolatedCpuTensorOwnership::RuntimeOutput ||
          capability.access != IsolatedCpuCapabilityAccess::ReadWrite ||
          descriptor.content_binding.has_value() ||
          descriptor.written_offset != 0U || descriptor.written_length != 0U) {
        fail("isolated CPU invocation output plan state is invalid");
      }
      break;
    case TensorPhase::ResponseOutput:
      if (descriptor.access != IsolatedCpuTensorAccess::OutputWriteOnly ||
          descriptor.readiness !=
              IsolatedCpuTensorReadiness::ReadyOutputCandidate ||
          descriptor.ownership !=
              IsolatedCpuTensorOwnership::HostOutputCandidate ||
          capability.access != IsolatedCpuCapabilityAccess::ReadWrite ||
          !descriptor.content_binding.has_value() ||
          descriptor.written_offset != 0U ||
          descriptor.written_length != descriptor.capability_length) {
        fail("isolated CPU invocation output candidate state is invalid");
      }
      break;
  }
  if (descriptor.allocation_alignment == 0U ||
      (descriptor.allocation_alignment &
       (descriptor.allocation_alignment - 1U)) != 0U ||
      descriptor.byte_offset != 0U ||
      std::any_of(descriptor.byte_strides.begin(),
                  descriptor.byte_strides.end(),
                  [](std::int64_t stride) { return stride <= 0; }) ||
      envelope.lower_extent != 0U ||
      envelope.upper_end != descriptor.capability_length) {
    fail("isolated CPU invocation output layout is not an exact producer");
  }
  validate_non_overlapping_output(descriptor, scalar_bytes);
}

/**
 * @brief Rejects overlapping ranges when either tensor is writable.
 * @param tensors Request or successful response descriptor inventory.
 * @throws IsolatedCpuProtocolError for a conflicting pair or range overflow.
 */
void validate_cross_tensor_overlap(
    const std::vector<IsolatedCpuTensorDescriptor>& tensors) {
  for (std::size_t index = 0U; index < tensors.size(); ++index) {
    const IsolatedCpuTensorDescriptor& current = tensors[index];
    const std::uint64_t current_end =
        checked_add(current.capability_offset, current.capability_length);
    for (std::size_t prior_index = 0U; prior_index < index; ++prior_index) {
      const IsolatedCpuTensorDescriptor& prior = tensors[prior_index];
      if (prior.capability_id != current.capability_id) {
        continue;
      }
      const std::uint64_t prior_end =
          checked_add(prior.capability_offset, prior.capability_length);
      const bool overlaps = current.capability_offset < prior_end &&
                            prior.capability_offset < current_end;
      const bool writable =
          current.access == IsolatedCpuTensorAccess::OutputWriteOnly ||
          prior.access == IsolatedCpuTensorAccess::OutputWriteOnly;
      if (overlaps && writable) {
        fail("isolated CPU invocation writable capability ranges overlap");
      }
    }
  }
}

/**
 * @brief Compares immutable output-plan fields while excluding phase state.
 * @param expected Retained request output plan.
 * @param observed Untrusted returned output candidate.
 * @return True when every immutable descriptor field is exact.
 * @throws Nothing under vector equality.
 */
bool same_output_plan(const IsolatedCpuTensorDescriptor& expected,
                      const IsolatedCpuTensorDescriptor& observed) noexcept {
  return expected.descriptor_version == observed.descriptor_version &&
         expected.kind == observed.kind &&
         expected.layout_kind == observed.layout_kind &&
         expected.access == observed.access &&
         expected.port_identity == observed.port_identity &&
         expected.binding_identity == observed.binding_identity &&
         expected.schema_identity == observed.schema_identity &&
         expected.facet_identity == observed.facet_identity &&
         expected.layout_identity == observed.layout_identity &&
         expected.schema_version == observed.schema_version &&
         expected.layout_version == observed.layout_version &&
         expected.capability_id == observed.capability_id &&
         expected.capability_offset == observed.capability_offset &&
         expected.capability_length == observed.capability_length &&
         expected.element_semantics == observed.element_semantics &&
         expected.storage_encoding == observed.storage_encoding &&
         expected.bit_width == observed.bit_width &&
         expected.extents == observed.extents &&
         expected.byte_strides == observed.byte_strides &&
         expected.byte_offset == observed.byte_offset &&
         expected.image_facet == observed.image_facet &&
         expected.region == observed.region &&
         expected.allocation_alignment == observed.allocation_alignment;
}

/**
 * @brief Encodes one complete request payload without its packet header.
 * @param request Validated request.
 * @return Exact payload bytes.
 * @throws Encoding allocation or bound errors unchanged.
 */
std::vector<std::byte> encode_request_payload(
    const IsolatedCpuInvocationRequest& request) {
  ByteWriter writer;
  encode_identity(request.identity, &writer);
  writer.put_string(request.operation, kMaximumIsolatedCpuOperationBytes);
  encode_opaque_id(request.operation_identity, &writer);
  encode_opaque_id(request.implementation_identity, &writer);
  encode_opaque_id(request.configuration_schema_identity, &writer);
  writer.put_u32(static_cast<std::uint32_t>(request.parameters.size()));
  for (const IsolatedCpuScalarParameter& parameter : request.parameters) {
    encode_parameter(parameter, &writer);
  }
  writer.put_u32(static_cast<std::uint32_t>(request.configuration.size()));
  for (const IsolatedCpuConfigurationNode& node : request.configuration) {
    encode_configuration_node(node, &writer);
  }
  writer.put_u32(static_cast<std::uint32_t>(request.capabilities.size()));
  for (const IsolatedCpuCapability& capability : request.capabilities) {
    encode_capability(capability, &writer);
  }
  writer.put_u32(request.input_count);
  writer.put_u32(request.output_count);
  writer.put_u32(static_cast<std::uint32_t>(request.tensors.size()));
  for (const IsolatedCpuTensorDescriptor& tensor : request.tensors) {
    encode_tensor(tensor, &writer);
  }
  encode_resources(request.resources, &writer);
  return writer.finish();
}

/**
 * @brief Decodes one request payload before complete semantic validation.
 * @param payload Exact bounded payload.
 * @param limits Retained endpoint limits used before each reserve.
 * @return Newly reconstructed request.
 * @throws IsolatedCpuProtocolError or std::bad_alloc from bounded decoding.
 */
IsolatedCpuInvocationRequest decode_request_payload(
    const std::vector<std::byte>& payload,
    const IsolatedCpuInvocationLimits& limits) {
  ByteReader reader(payload.data(), payload.size());
  IsolatedCpuInvocationRequest request;
  request.identity = decode_identity(&reader);
  request.operation = reader.get_string(kMaximumIsolatedCpuOperationBytes);
  request.operation_identity = decode_opaque_id(&reader);
  request.implementation_identity = decode_opaque_id(&reader);
  request.configuration_schema_identity = decode_opaque_id(&reader);
  const std::size_t parameter_count =
      decode_count(&reader, limits.maximum_parameters, "parameter count");
  request.parameters.reserve(parameter_count);
  for (std::size_t index = 0U; index < parameter_count; ++index) {
    request.parameters.push_back(decode_parameter(&reader));
  }
  const std::size_t configuration_count =
      decode_count(&reader, kMaximumIsolatedCpuConfigurationNodes,
                   "configuration node count");
  request.configuration.reserve(configuration_count);
  for (std::size_t index = 0U; index < configuration_count; ++index) {
    request.configuration.push_back(decode_configuration_node(&reader));
  }
  const std::size_t capability_count =
      decode_count(&reader, limits.maximum_capabilities, "capability count");
  request.capabilities.reserve(capability_count);
  for (std::size_t index = 0U; index < capability_count; ++index) {
    request.capabilities.push_back(decode_capability(&reader));
  }
  request.input_count = reader.get_u32();
  request.output_count = reader.get_u32();
  const std::size_t tensor_count =
      decode_count(&reader, limits.maximum_descriptors, "descriptor count");
  request.tensors.reserve(tensor_count);
  for (std::size_t index = 0U; index < tensor_count; ++index) {
    request.tensors.push_back(decode_tensor(&reader));
  }
  request.resources = decode_resources(&reader);
  reader.finish();
  return request;
}

/**
 * @brief Encodes one complete response payload without its packet header.
 * @param response Validated response.
 * @return Exact payload bytes.
 * @throws Encoding allocation or bound errors unchanged.
 */
std::vector<std::byte> encode_response_payload(
    const IsolatedCpuInvocationResponse& response) {
  ByteWriter writer;
  encode_identity(response.identity, &writer);
  writer.put_string(response.operation, kMaximumIsolatedCpuOperationBytes);
  encode_resources(response.resources, &writer);
  writer.put_u8(static_cast<std::uint8_t>(response.outcome));
  writer.put_u32(static_cast<std::uint32_t>(response.outputs.size()));
  for (const IsolatedCpuTensorDescriptor& output : response.outputs) {
    encode_tensor(output, &writer);
  }
  writer.put_string(response.diagnostic, kMaximumIsolatedCpuDiagnosticBytes);
  return writer.finish();
}

/**
 * @brief Decodes one response payload before retained-request comparison.
 * @param payload Exact bounded payload.
 * @param limits Retained Host limits used before reserve.
 * @return Newly reconstructed response.
 * @throws IsolatedCpuProtocolError or std::bad_alloc from bounded decoding.
 */
IsolatedCpuInvocationResponse decode_response_payload(
    const std::vector<std::byte>& payload,
    const IsolatedCpuInvocationLimits& limits) {
  ByteReader reader(payload.data(), payload.size());
  IsolatedCpuInvocationResponse response;
  response.identity = decode_identity(&reader);
  response.operation = reader.get_string(kMaximumIsolatedCpuOperationBytes);
  response.resources = decode_resources(&reader);
  response.outcome = parse_outcome(reader.get_u8());
  const std::size_t output_count = decode_count(
      &reader, limits.maximum_descriptors, "response output count");
  response.outputs.reserve(output_count);
  for (std::size_t index = 0U; index < output_count; ++index) {
    response.outputs.push_back(decode_tensor(&reader));
  }
  response.diagnostic = reader.get_string(kMaximumIsolatedCpuDiagnosticBytes);
  reader.finish();
  return response;
}

/** @brief Permanent canonical schema identity for protocol-v2 bindings. */
constexpr ExtensionIdentity kIsolatedCpuBindingSchemaIdentity{
    0x70686f746f737069ULL,
    0x6465722d69736f32ULL};  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Builds the canonical immutable descriptor record for byte binding.
 * @param identity Exact invocation identity.
 * @param descriptor Tensor descriptor whose phase fields are excluded.
 * @return One provider-independent schema envelope.
 * @throws Encoding allocation or bound errors unchanged.
 * @note Readiness, ownership, and the claimed digest are deliberately absent;
 * immutable identity, access, capability, logical, layout, and facet facts are
 * present.
 */
DataDescriptorEnvelope binding_descriptor_envelope(
    const IsolatedCpuInvocationIdentity& identity,
    const IsolatedCpuTensorDescriptor& descriptor) {
  ByteWriter writer;
  writer.put_u16(kIsolatedCpuInvocationProtocolVersion);
  writer.put_u16(descriptor.descriptor_version);
  encode_identity(identity, &writer);
  writer.put_u8(static_cast<std::uint8_t>(descriptor.kind));
  writer.put_u8(static_cast<std::uint8_t>(descriptor.layout_kind));
  writer.put_u8(static_cast<std::uint8_t>(descriptor.access));
  encode_opaque_id(descriptor.port_identity, &writer);
  encode_opaque_id(descriptor.binding_identity, &writer);
  encode_opaque_id(descriptor.schema_identity, &writer);
  encode_opaque_id(descriptor.facet_identity, &writer);
  encode_opaque_id(descriptor.layout_identity, &writer);
  writer.put_u64(descriptor.schema_version);
  writer.put_u64(descriptor.layout_version);
  writer.put_u64(descriptor.capability_id);
  writer.put_u64(descriptor.capability_offset);
  writer.put_u64(descriptor.capability_length);
  writer.put_u8(static_cast<std::uint8_t>(descriptor.element_semantics));
  writer.put_u8(static_cast<std::uint8_t>(descriptor.storage_encoding));
  writer.put_u32(descriptor.bit_width);
  writer.put_u32(static_cast<std::uint32_t>(descriptor.extents.size()));
  for (const std::uint64_t extent : descriptor.extents) {
    writer.put_u64(extent);
  }
  writer.put_u32(static_cast<std::uint32_t>(descriptor.byte_strides.size()));
  for (const std::int64_t stride : descriptor.byte_strides) {
    writer.put_i64(stride);
  }
  writer.put_u64(descriptor.byte_offset);
  writer.put_bool(descriptor.image_facet.has_value());
  if (descriptor.image_facet.has_value()) {
    encode_image_facet(*descriptor.image_facet, &writer);
  }
  encode_region(descriptor.region, &writer);
  writer.put_u64(descriptor.allocation_alignment);

  ExtensionRecord schema;
  schema.kind = ExtensionDefinitionKind::Schema;
  schema.identity = kIsolatedCpuBindingSchemaIdentity;
  schema.structural_version = 2U;
  schema.payload = writer.finish();
  DataDescriptorEnvelope envelope;
  envelope.schema = std::move(schema);
  return envelope;
}

}  // namespace

/** @copydoc IsolatedCpuOpaqueId::valid */
bool IsolatedCpuOpaqueId::valid() const noexcept {
  return std::any_of(bytes.begin(), bytes.end(),
                     [](std::byte value) { return value != std::byte{0}; });
}

/** @copydoc IsolatedCpuInvocationIdentity::operator== */
bool IsolatedCpuInvocationIdentity::operator==(
    const IsolatedCpuInvocationIdentity& other) const noexcept {
  return tenant_id == other.tenant_id && job_id == other.job_id &&
         attempt_id == other.attempt_id && worker_id == other.worker_id &&
         worker_lease_generation == other.worker_lease_generation &&
         plugin_package_id == other.plugin_package_id &&
         plugin_generation == other.plugin_generation &&
         invocation_id == other.invocation_id;
}

/** @copydoc IsolatedCpuScalarParameter::operator== */
bool IsolatedCpuScalarParameter::operator==(
    const IsolatedCpuScalarParameter& other) const noexcept {
  return name == other.name && kind == other.kind &&
         boolean_value == other.boolean_value &&
         signed_value == other.signed_value &&
         unsigned_value == other.unsigned_value &&
         floating_value == other.floating_value &&
         string_value == other.string_value;
}

/** @copydoc IsolatedCpuConfigurationNode::operator== */
bool IsolatedCpuConfigurationNode::operator==(
    const IsolatedCpuConfigurationNode& other) const noexcept {
  return kind == other.kind && key == other.key &&
         boolean_value == other.boolean_value &&
         signed_value == other.signed_value &&
         floating_value == other.floating_value &&
         bytes_value == other.bytes_value && first_child == other.first_child &&
         child_count == other.child_count;
}

/** @copydoc IsolatedCpuImageFacet::operator== */
bool IsolatedCpuImageFacet::operator==(
    const IsolatedCpuImageFacet& other) const noexcept {
  if (x_axis != other.x_axis || y_axis != other.y_axis ||
      channel_axis != other.channel_axis ||
      !(data_window == other.data_window) ||
      !(display_window == other.display_window) ||
      !(sample_domain == other.sample_domain) || !(color == other.color) ||
      channel_schema.has_value() != other.channel_schema.has_value()) {
    return false;
  }
  if (!channel_schema.has_value()) {
    return true;
  }
  if (channel_schema->channels.size() !=
          other.channel_schema->channels.size() ||
      channel_schema->groups.size() != other.channel_schema->groups.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < channel_schema->channels.size();
       ++index) {
    const ChannelDescription& left = channel_schema->channels[index];
    const ChannelDescription& right = other.channel_schema->channels[index];
    if (!(left.id == right.id) ||
        left.diagnostic_name != right.diagnostic_name) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < channel_schema->groups.size(); ++index) {
    const ChannelGroupDescription& left = channel_schema->groups[index];
    const ChannelGroupDescription& right = other.channel_schema->groups[index];
    if (!(left.id == right.id) ||
        left.diagnostic_name != right.diagnostic_name ||
        left.members != right.members) {
      return false;
    }
  }
  return true;
}

/** @copydoc IsolatedCpuTensorDescriptor::operator== */
bool IsolatedCpuTensorDescriptor::operator==(
    const IsolatedCpuTensorDescriptor& other) const noexcept {
  return descriptor_version == other.descriptor_version && kind == other.kind &&
         layout_kind == other.layout_kind && access == other.access &&
         readiness == other.readiness && ownership == other.ownership &&
         port_identity == other.port_identity &&
         binding_identity == other.binding_identity &&
         schema_identity == other.schema_identity &&
         facet_identity == other.facet_identity &&
         layout_identity == other.layout_identity &&
         schema_version == other.schema_version &&
         layout_version == other.layout_version &&
         capability_id == other.capability_id &&
         capability_offset == other.capability_offset &&
         capability_length == other.capability_length &&
         element_semantics == other.element_semantics &&
         storage_encoding == other.storage_encoding &&
         bit_width == other.bit_width && extents == other.extents &&
         byte_strides == other.byte_strides &&
         byte_offset == other.byte_offset && image_facet == other.image_facet &&
         region == other.region &&
         allocation_alignment == other.allocation_alignment &&
         written_offset == other.written_offset &&
         written_length == other.written_length &&
         content_binding == other.content_binding;
}

/** @copydoc IsolatedCpuInvocationRequest::operator== */
bool IsolatedCpuInvocationRequest::operator==(
    const IsolatedCpuInvocationRequest& other) const noexcept {
  return identity == other.identity && operation == other.operation &&
         operation_identity == other.operation_identity &&
         implementation_identity == other.implementation_identity &&
         configuration_schema_identity == other.configuration_schema_identity &&
         parameters == other.parameters &&
         configuration == other.configuration &&
         capabilities == other.capabilities && tensors == other.tensors &&
         input_count == other.input_count &&
         output_count == other.output_count && resources == other.resources;
}

/** @copydoc IsolatedCpuInvocationResponse::operator== */
bool IsolatedCpuInvocationResponse::operator==(
    const IsolatedCpuInvocationResponse& other) const noexcept {
  return identity == other.identity && operation == other.operation &&
         resources == other.resources && outcome == other.outcome &&
         outputs == other.outputs && diagnostic == other.diagnostic;
}

/** @copydoc validate_isolated_cpu_invocation_limits */
void validate_isolated_cpu_invocation_limits(
    const IsolatedCpuInvocationLimits& limits) {
  if (limits.maximum_shared_memory_bytes == 0U ||
      limits.maximum_shared_memory_bytes > kMaximumIsolatedCpuSharedBytes ||
      limits.maximum_capabilities == 0U ||
      limits.maximum_capabilities > kMaximumIsolatedCpuCapabilities ||
      limits.maximum_descriptors == 0U ||
      limits.maximum_descriptors > kMaximumIsolatedCpuDescriptors ||
      limits.maximum_parameters > kMaximumIsolatedCpuParameters) {
    throw std::invalid_argument(
        "isolated CPU required limit is zero or limit exceeds protocol v2");
  }
}

/** @copydoc validate_isolated_cpu_invocation_metadata */
void validate_isolated_cpu_invocation_metadata(
    const IsolatedCpuInvocationIdentity& identity, const std::string& operation,
    const std::vector<IsolatedCpuScalarParameter>& parameters,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_limits(limits);
  validate_identity(identity);
  validate_text(operation, kMaximumIsolatedCpuOperationBytes, "operation key");
  if (parameters.size() > limits.maximum_parameters) {
    fail("isolated CPU invocation parameter count exceeds its limit");
  }
  std::string_view previous_parameter;
  for (const IsolatedCpuScalarParameter& parameter : parameters) {
    validate_parameter(parameter);
    if (!previous_parameter.empty() && previous_parameter >= parameter.name) {
      fail("isolated CPU invocation parameters are not uniquely sorted");
    }
    previous_parameter = parameter.name;
  }
}

/** @copydoc validate_isolated_cpu_invocation_request */
void validate_isolated_cpu_invocation_request(
    const IsolatedCpuInvocationRequest& request,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_metadata(request.identity, request.operation,
                                            request.parameters, limits);
  if (!request.operation_identity.valid() ||
      !request.implementation_identity.valid() ||
      !request.configuration_schema_identity.valid()) {
    fail("isolated CPU operation/configuration identity is incomplete");
  }
  if (!request.parameters.empty() && !request.configuration.empty()) {
    fail("isolated CPU invocation carries two configuration representations");
  }
  validate_configuration(request.configuration);

  if (request.capabilities.empty() ||
      request.capabilities.size() > limits.maximum_capabilities) {
    fail("isolated CPU invocation capability count is invalid");
  }
  std::uint64_t previous_capability_id = 0U;
  std::uint64_t shared_bytes = 0U;
  for (const IsolatedCpuCapability& capability : request.capabilities) {
    if (capability.capability_id == 0U ||
        capability.capability_id <= previous_capability_id ||
        capability.byte_size == 0U) {
      fail("isolated CPU invocation capabilities are invalid or unsorted");
    }
    switch (capability.access) {
      case IsolatedCpuCapabilityAccess::ReadOnly:
      case IsolatedCpuCapabilityAccess::ReadWrite:
        break;
      default:
        fail("isolated CPU invocation capability access is invalid");
    }
    shared_bytes = checked_add(shared_bytes, capability.byte_size);
    previous_capability_id = capability.capability_id;
  }
  if (shared_bytes > limits.maximum_shared_memory_bytes) {
    fail("isolated CPU invocation shared bytes exceed their limit");
  }

  if (request.output_count == 0U ||
      request.tensors.size() > limits.maximum_descriptors ||
      checked_add(request.input_count, request.output_count) !=
          request.tensors.size()) {
    fail("isolated CPU invocation descriptor counts are inconsistent");
  }
  if (request.resources.shared_memory_bytes != shared_bytes ||
      request.resources.descriptor_count != request.tensors.size() ||
      request.resources.cpu_slots != 1U) {
    fail("isolated CPU invocation resource declaration is inconsistent");
  }

  std::vector<bool> referenced(request.capabilities.size(), false);
  for (std::size_t index = 0U; index < request.tensors.size(); ++index) {
    const TensorPhase phase = index < request.input_count
                                  ? TensorPhase::RequestInput
                                  : TensorPhase::RequestOutput;
    validate_tensor(request.tensors[index], request.capabilities, phase);
    const auto found = std::lower_bound(
        request.capabilities.begin(), request.capabilities.end(),
        request.tensors[index].capability_id,
        [](const IsolatedCpuCapability& capability, std::uint64_t id) {
          return capability.capability_id < id;
        });
    referenced[static_cast<std::size_t>(
        std::distance(request.capabilities.begin(), found))] = true;
  }
  if (std::any_of(referenced.begin(), referenced.end(),
                  [](bool value) { return !value; })) {
    fail("isolated CPU invocation contains an unreferenced capability");
  }
  validate_cross_tensor_overlap(request.tensors);
}

/** @copydoc validate_isolated_cpu_invocation_response */
void validate_isolated_cpu_invocation_response(
    const IsolatedCpuInvocationRequest& request,
    const IsolatedCpuInvocationResponse& response,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_request(request, limits);
  if (!(response.identity == request.identity) ||
      response.operation != request.operation ||
      !(response.resources == request.resources)) {
    fail("isolated CPU invocation response identity or resources are stale");
  }
  if (response.diagnostic.size() > kMaximumIsolatedCpuDiagnosticBytes ||
      response.diagnostic.find('\0') != std::string::npos) {
    fail("isolated CPU invocation response diagnostic is invalid");
  }
  switch (response.outcome) {
    case IsolatedCpuInvocationOutcome::Succeeded:
      if (!response.diagnostic.empty() ||
          response.outputs.size() != request.output_count) {
        fail("isolated CPU invocation success framing is invalid");
      }
      break;
    case IsolatedCpuInvocationOutcome::PluginFailed:
      if (response.diagnostic.empty() || !response.outputs.empty()) {
        fail("isolated CPU invocation plugin failure framing is invalid");
      }
      return;
    case IsolatedCpuInvocationOutcome::Cancelled:
      if (!response.outputs.empty()) {
        fail("isolated CPU invocation cancellation carries outputs");
      }
      return;
    default:
      fail("isolated CPU invocation response outcome is invalid");
  }

  for (std::size_t index = 0U; index < response.outputs.size(); ++index) {
    const std::size_t request_index = request.input_count + index;
    if (!same_output_plan(request.tensors[request_index],
                          response.outputs[index])) {
      fail("isolated CPU invocation response changed its output plan");
    }
    validate_tensor(response.outputs[index], request.capabilities,
                    TensorPhase::ResponseOutput);
  }
  validate_cross_tensor_overlap(response.outputs);
}

/** @copydoc encode_isolated_cpu_invocation_request */
std::vector<std::byte> encode_isolated_cpu_invocation_request(
    const IsolatedCpuInvocationRequest& request,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_request(request, limits);
  return encode_packet(IsolatedCpuPacketKind::Request,
                       encode_request_payload(request));
}

/** @copydoc decode_isolated_cpu_invocation_request */
IsolatedCpuInvocationRequest decode_isolated_cpu_invocation_request(
    const std::vector<std::byte>& packet,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_limits(limits);
  DecodedPacket decoded = decode_packet(packet);
  if (decoded.kind != IsolatedCpuPacketKind::Request) {
    fail("isolated CPU invocation expected a request packet");
  }
  IsolatedCpuInvocationRequest request =
      decode_request_payload(decoded.payload, limits);
  validate_isolated_cpu_invocation_request(request, limits);
  return request;
}

/** @copydoc encode_isolated_cpu_invocation_response */
std::vector<std::byte> encode_isolated_cpu_invocation_response(
    const IsolatedCpuInvocationRequest& request,
    const IsolatedCpuInvocationResponse& response,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_response(request, response, limits);
  return encode_packet(IsolatedCpuPacketKind::Response,
                       encode_response_payload(response));
}

/** @copydoc decode_isolated_cpu_invocation_response */
IsolatedCpuInvocationResponse decode_isolated_cpu_invocation_response(
    const IsolatedCpuInvocationRequest& request,
    const std::vector<std::byte>& packet,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_request(request, limits);
  DecodedPacket decoded = decode_packet(packet);
  if (decoded.kind != IsolatedCpuPacketKind::Response) {
    fail("isolated CPU invocation expected a response packet");
  }
  IsolatedCpuInvocationResponse response =
      decode_response_payload(decoded.payload, limits);
  validate_isolated_cpu_invocation_response(request, response, limits);
  return response;
}

/** @copydoc compute_isolated_cpu_content_binding */
ContentDigest compute_isolated_cpu_content_binding(
    const IsolatedCpuInvocationIdentity& identity,
    const IsolatedCpuTensorDescriptor& descriptor, const std::byte* bytes,
    std::size_t size) {
  validate_identity(identity);
  if (descriptor.descriptor_version != kIsolatedCpuTensorDescriptorVersion ||
      descriptor.kind != IsolatedCpuTensorKind::DenseTensor ||
      descriptor.layout_kind != IsolatedCpuLayoutKind::Strided ||
      descriptor.extents.empty() ||
      descriptor.extents.size() > kMaximumIsolatedCpuTensorRank ||
      descriptor.extents.size() != descriptor.byte_strides.size() ||
      descriptor.capability_id == 0U ||
      descriptor.capability_length != to_u64(size) ||
      (bytes == nullptr && size != 0U)) {
    fail("isolated CPU invocation content-binding input is invalid");
  }
  static_cast<void>(element_bytes(descriptor));
  validate_image_facet(descriptor);
  const AddressEnvelope envelope =
      compute_address_envelope(descriptor, element_bytes(descriptor));
  if (envelope.upper_end > descriptor.capability_length) {
    fail("isolated CPU invocation binding layout exceeds its range");
  }
  const DescriptorDigest descriptor_digest = compute_descriptor_digest(
      binding_descriptor_envelope(identity, descriptor));
  internal::CanonicalContentDigestWriter writer(descriptor_digest,
                                                to_u64(size));
  writer.append(bytes, size);
  return writer.finish();
}

}  // namespace ps::execution
