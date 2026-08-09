/**
 * @file worker_protocol.cpp
 * @brief Implements the bounded private Issue #100 worker frame protocol.
 */
#include "server/worker_protocol.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps::server {
namespace {

/** @brief Fixed big-endian frame magic spelling ASCII `PSW1`. */
constexpr std::uint32_t kWorkerProtocolMagic = 0x50535731U;
/** @brief Sole supported private worker protocol version. */
constexpr std::uint16_t kWorkerProtocolVersion = 1U;
/** @brief Fixed magic/version/kind/length header byte count. */
constexpr std::size_t kWorkerFrameHeaderBytes = 12U;
/** @brief Maximum transported path/configuration field length. */
constexpr std::size_t kMaximumWorkerPathBytes = 16U << 10U;
/** @brief Maximum worker or resolver diagnostic length. */
constexpr std::size_t kMaximumWorkerDiagnosticBytes = 16U << 10U;

/**
 * @brief Checked append-only encoder for one already-bounded frame payload.
 * @throws std::length_error when aggregate or field bounds are exceeded.
 * @throws std::bad_alloc when payload storage exhausts memory.
 * @note Integer values use fixed-width big-endian representation.
 */
class ByteWriter final {
 public:
  /**
   * @brief Appends one unsigned byte.
   * @param value Exact byte value.
   * @throws std::length_error when the aggregate frame bound is exceeded.
   * @throws std::bad_alloc when growing storage exhausts memory.
   */
  void put_u8(std::uint8_t value) { put_byte(static_cast<std::byte>(value)); }

  /**
   * @brief Appends one big-endian 16-bit integer.
   * @param value Exact integer value.
   * @throws As `put_byte()`.
   */
  void put_u16(std::uint16_t value) {
    put_u8(static_cast<std::uint8_t>(value >> 8U));
    put_u8(static_cast<std::uint8_t>(value));
  }

  /**
   * @brief Appends one big-endian 32-bit integer.
   * @param value Exact integer value.
   * @throws As `put_byte()`.
   */
  void put_u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      put_u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  /**
   * @brief Appends one big-endian 64-bit integer.
   * @param value Exact integer value.
   * @throws As `put_byte()`.
   */
  void put_u64(std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      put_u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  /**
   * @brief Appends one closed boolean representation.
   * @param value Boolean value.
   * @throws As `put_byte()`.
   */
  void put_bool(bool value) { put_u8(value ? 1U : 0U); }

  /**
   * @brief Appends one length-prefixed bounded string.
   * @param value Exact bytes to append.
   * @param maximum Per-field maximum length.
   * @throws std::length_error for per-field, 32-bit, or aggregate overflow.
   * @throws std::bad_alloc when growing storage exhausts memory.
   */
  void put_string(std::string_view value, std::size_t maximum) {
    if (value.size() > maximum ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("worker protocol string exceeds its bound");
    }
    put_u32(static_cast<std::uint32_t>(value.size()));
    put_raw(reinterpret_cast<const std::byte*>(value.data()), value.size());
  }

  /**
   * @brief Appends one length-prefixed bounded byte vector.
   * @param value Exact bytes to append.
   * @param maximum Per-field maximum length.
   * @throws std::length_error for per-field, 32-bit, or aggregate overflow.
   * @throws std::bad_alloc when growing storage exhausts memory.
   */
  void put_blob(const std::vector<std::byte>& value, std::size_t maximum) {
    if (value.size() > maximum ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("worker protocol blob exceeds its bound");
    }
    put_u32(static_cast<std::uint32_t>(value.size()));
    put_raw(value.data(), value.size());
  }

  /**
   * @brief Appends an exact raw byte range without a length prefix.
   * @param bytes Borrowed bytes, null only when `size` is zero.
   * @param size Number of bytes to append.
   * @throws std::invalid_argument for null nonempty input.
   * @throws std::length_error when the aggregate frame bound is exceeded.
   * @throws std::bad_alloc when growing storage exhausts memory.
   */
  void put_raw(const std::byte* bytes, std::size_t size) {
    if (size != 0U && bytes == nullptr) {
      throw std::invalid_argument("worker protocol raw input is null");
    }
    require_capacity(size);
    if (size == 0U) {
      return;
    }
    bytes_.insert(bytes_.end(), bytes, bytes + size);
  }

  /**
   * @brief Transfers the completed payload out of this writer.
   * @return Exact encoded bytes.
   * @throws Nothing.
   */
  std::vector<std::byte> finish() noexcept { return std::move(bytes_); }

 private:
  /**
   * @brief Appends one byte after aggregate-bound validation.
   * @param value Exact byte.
   * @throws std::length_error when the aggregate bound is exceeded.
   * @throws std::bad_alloc when growing storage exhausts memory.
   */
  void put_byte(std::byte value) {
    require_capacity(1U);
    bytes_.push_back(value);
  }

  /**
   * @brief Verifies that a pending append fits the frame payload maximum.
   * @param additional Number of bytes about to be appended.
   * @throws std::length_error on checked aggregate overflow.
   */
  void require_capacity(std::size_t additional) const {
    if (additional > kMaximumWorkerFramePayloadBytes - bytes_.size()) {
      throw std::length_error("worker protocol payload exceeds its bound");
    }
  }

  /** @brief Exact encoded payload under construction. */
  std::vector<std::byte> bytes_;
};

/**
 * @brief Checked single-pass decoder for one bounded frame payload.
 * @throws WorkerProtocolError for truncation, invalid booleans, or leftovers.
 * @throws std::bad_alloc when decoded strings/blobs exhaust memory.
 */
class ByteReader final {
 public:
  /**
   * @brief Borrows one immutable payload for its decoding lifetime.
   * @param bytes Exact frame payload.
   * @throws Nothing.
   */
  explicit ByteReader(const std::vector<std::byte>& bytes) noexcept
      : bytes_(bytes) {}

  /**
   * @brief Reads one unsigned byte.
   * @return Exact byte value.
   * @throws WorkerProtocolError for truncation.
   */
  std::uint8_t get_u8() { return std::to_integer<std::uint8_t>(*take(1U)); }

  /**
   * @brief Reads one big-endian 16-bit integer.
   * @return Exact integer value.
   * @throws WorkerProtocolError for truncation.
   */
  std::uint16_t get_u16() {
    std::uint16_t value = 0U;
    for (std::size_t index = 0U; index < 2U; ++index) {
      value = static_cast<std::uint16_t>((value << 8U) | get_u8());
    }
    return value;
  }

  /**
   * @brief Reads one big-endian 32-bit integer.
   * @return Exact integer value.
   * @throws WorkerProtocolError for truncation.
   */
  std::uint32_t get_u32() {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
      value = (value << 8U) | get_u8();
    }
    return value;
  }

  /**
   * @brief Reads one big-endian 64-bit integer.
   * @return Exact integer value.
   * @throws WorkerProtocolError for truncation.
   */
  std::uint64_t get_u64() {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
      value = (value << 8U) | get_u8();
    }
    return value;
  }

  /**
   * @brief Reads one closed boolean byte.
   * @return Decoded boolean.
   * @throws WorkerProtocolError for values other than zero or one.
   */
  bool get_bool() {
    const std::uint8_t value = get_u8();
    if (value > 1U) {
      throw WorkerProtocolError("worker protocol boolean is invalid");
    }
    return value != 0U;
  }

  /**
   * @brief Reads one length-prefixed bounded string.
   * @param maximum Per-field maximum accepted length.
   * @return Exact copied string bytes.
   * @throws WorkerProtocolError for length overflow or truncation.
   * @throws std::bad_alloc when copying exhausts memory.
   */
  std::string get_string(std::size_t maximum) {
    const std::size_t size = get_u32();
    if (size > maximum) {
      throw WorkerProtocolError("worker protocol string exceeds its bound");
    }
    if (size == 0U) {
      return {};
    }
    const std::byte* bytes = take(size);
    return std::string(reinterpret_cast<const char*>(bytes), size);
  }

  /**
   * @brief Reads one length-prefixed bounded byte vector.
   * @param maximum Per-field maximum accepted length.
   * @return Exact copied bytes.
   * @throws WorkerProtocolError for length overflow or truncation.
   * @throws std::bad_alloc when copying exhausts memory.
   */
  std::vector<std::byte> get_blob(std::size_t maximum) {
    const std::size_t size = get_u32();
    if (size > maximum) {
      throw WorkerProtocolError("worker protocol blob exceeds its bound");
    }
    if (size == 0U) {
      return {};
    }
    const std::byte* bytes = take(size);
    return std::vector<std::byte>(bytes, bytes + size);
  }

  /**
   * @brief Copies one exact fixed-width raw byte range.
   * @param output Non-null destination covering `size` bytes.
   * @param size Exact fixed width.
   * @throws std::invalid_argument for null nonempty output.
   * @throws WorkerProtocolError for truncation.
   */
  void get_raw(std::byte* output, std::size_t size) {
    if (size != 0U && output == nullptr) {
      throw std::invalid_argument("worker protocol raw output is null");
    }
    if (size == 0U) {
      return;
    }
    const std::byte* bytes = take(size);
    std::copy(bytes, bytes + size, output);
  }

  /**
   * @brief Rejects any unconsumed trailing payload bytes.
   * @throws WorkerProtocolError when fields remain.
   */
  void finish() const {
    if (offset_ != bytes_.size()) {
      throw WorkerProtocolError("worker protocol payload has trailing bytes");
    }
  }

 private:
  /**
   * @brief Borrows the next exact byte range and advances once.
   * @param size Required byte count.
   * @return Borrowed pointer valid for `bytes_` lifetime.
   * @throws WorkerProtocolError for checked truncation.
   */
  const std::byte* take(std::size_t size) {
    if (size > bytes_.size() - offset_) {
      throw WorkerProtocolError("worker protocol payload is truncated");
    }
    const std::byte* result = bytes_.data() + offset_;
    offset_ += size;
    return result;
  }

  /** @brief Borrowed immutable payload. */
  const std::vector<std::byte>& bytes_;
  /** @brief Next unread byte offset. */
  std::size_t offset_ = 0U;
};

/**
 * @brief Validates one closed message-kind representation.
 * @param value Numeric header value.
 * @return Typed closed kind.
 * @throws WorkerProtocolError for an unknown value.
 */
WorkerMessageKind parse_message_kind(std::uint16_t value) {
  switch (static_cast<WorkerMessageKind>(value)) {
    case WorkerMessageKind::Assignment:
    case WorkerMessageKind::AssignmentAccepted:
    case WorkerMessageKind::Heartbeat:
    case WorkerMessageKind::Cancel:
    case WorkerMessageKind::Report:
      return static_cast<WorkerMessageKind>(value);
  }
  throw WorkerProtocolError("worker protocol message kind is invalid");
}

/**
 * @brief Converts a positive remaining duration to a poll timeout.
 * @param deadline Absolute monotonic deadline.
 * @return Milliseconds rounded up and clamped to `INT_MAX`, or zero when due.
 * @throws Nothing.
 */
int poll_timeout_ms(std::chrono::steady_clock::time_point deadline) noexcept {
  const auto now = std::chrono::steady_clock::now();
  if (deadline <= now) {
    return 0;
  }
  const auto remaining = deadline - now;
  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (milliseconds < remaining) {
    milliseconds += std::chrono::milliseconds(1);
  }
  return milliseconds.count() > INT_MAX
             ? INT_MAX
             : static_cast<int>(milliseconds.count());
}

/**
 * @brief Waits for one socket readiness direction until an absolute deadline.
 * @param fd Valid socket descriptor.
 * @param events `POLLIN` or `POLLOUT`.
 * @param deadline Absolute monotonic deadline.
 * @return Nothing when requested readiness or hangup is observable.
 * @throws WorkerProtocolTimeout on deadline expiry.
 * @throws WorkerChannelError on invalid descriptor or poll failure.
 */
void wait_ready(int fd, std::int16_t events,
                std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    pollfd descriptor{fd, events, 0};
    const int result = ::poll(&descriptor, 1U, poll_timeout_ms(deadline));
    if (result > 0) {
      if ((descriptor.revents & POLLNVAL) != 0) {
        throw WorkerChannelError("worker protocol descriptor is invalid");
      }
      return;
    }
    if (result == 0) {
      throw WorkerProtocolTimeout("worker protocol I/O deadline expired");
    }
    if (errno != EINTR) {
      throw WorkerChannelError(std::string("worker protocol poll failed: ") +
                               std::strerror(errno));
    }
  }
}

/**
 * @brief Writes an exact byte range with partial-I/O and SIGPIPE handling.
 * @param fd Connected private socket.
 * @param bytes Non-null exact input when `size` is nonzero.
 * @param size Number of bytes to send.
 * @param deadline Absolute monotonic deadline.
 * @throws WorkerProtocolTimeout or WorkerChannelError on I/O failure.
 */
void write_all(int fd, const std::byte* bytes, std::size_t size,
               std::chrono::steady_clock::time_point deadline) {
  std::size_t offset = 0U;
  while (offset != size) {
    wait_ready(fd, POLLOUT, deadline);
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    const ssize_t written =
        ::send(fd, bytes + offset, size - offset, kSendFlags);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    const int error = written == 0 ? EPIPE : errno;
    throw WorkerChannelError(std::string("worker protocol write failed: ") +
                             std::strerror(error));
  }
}

/**
 * @brief Reads an exact byte range with explicit frame-boundary EOF meaning.
 * @param fd Connected private socket.
 * @param bytes Non-null output when `size` is nonzero.
 * @param size Number of bytes to receive.
 * @param deadline Absolute monotonic deadline.
 * @param clean_eof_allowed Whether zero bytes at initial offset means clean
 * frame-boundary EOF.
 * @throws WorkerProtocolEof for allowed initial EOF.
 * @throws WorkerProtocolError for truncated input.
 * @throws WorkerProtocolTimeout or WorkerChannelError on I/O failure.
 */
void read_all(int fd, std::byte* bytes, std::size_t size,
              std::chrono::steady_clock::time_point deadline,
              bool clean_eof_allowed) {
  std::size_t offset = 0U;
  while (offset != size) {
    wait_ready(fd, POLLIN, deadline);
    const ssize_t received = ::recv(fd, bytes + offset, size - offset, 0);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received == 0) {
      if (clean_eof_allowed && offset == 0U) {
        throw WorkerProtocolEof("worker protocol channel reached EOF");
      }
      throw WorkerProtocolError("worker protocol frame is truncated by EOF");
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    throw WorkerChannelError(std::string("worker protocol read failed: ") +
                             std::strerror(errno));
  }
}

/**
 * @brief Encodes a complete exact attempt identity.
 * @param identity Valid identity tuple.
 * @param writer Non-null payload owner.
 * @throws Contract, length, or allocation failures unchanged.
 */
void encode_identity(const AttemptIdentity& identity, ByteWriter* writer) {
  validate_attempt_identity(identity);
  if (writer == nullptr) {
    throw std::invalid_argument("worker identity writer is null");
  }
  writer->put_string(identity.tenant_id.value(), kMaximumOpaqueIdentityBytes);
  writer->put_string(identity.job_id.value(), kMaximumOpaqueIdentityBytes);
  writer->put_raw(identity.job_spec_digest.bytes.data(),
                  identity.job_spec_digest.bytes.size());
  writer->put_string(identity.attempt_id.value(), kMaximumOpaqueIdentityBytes);
  writer->put_string(identity.worker_instance_id.value(),
                     kMaximumOpaqueIdentityBytes);
  writer->put_u64(identity.worker_lease_generation.value);
}

/**
 * @brief Decodes and validates a complete exact attempt identity.
 * @param reader Non-null current payload reader.
 * @return Complete identity tuple.
 * @throws WorkerProtocolError or identity allocation/validation failures.
 */
AttemptIdentity read_identity(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("worker identity reader is null");
  }
  AttemptIdentity identity;
  identity.tenant_id =
      TenantId(reader->get_string(kMaximumOpaqueIdentityBytes));
  identity.job_id = JobId(reader->get_string(kMaximumOpaqueIdentityBytes));
  reader->get_raw(identity.job_spec_digest.bytes.data(),
                  identity.job_spec_digest.bytes.size());
  identity.attempt_id =
      JobAttemptId(reader->get_string(kMaximumOpaqueIdentityBytes));
  identity.worker_instance_id =
      WorkerInstanceId(reader->get_string(kMaximumOpaqueIdentityBytes));
  identity.worker_lease_generation.value = reader->get_u64();
  validate_attempt_identity(identity);
  return identity;
}

/**
 * @brief Validates and converts a size value to its fixed transport width.
 * @param value Host-size value.
 * @return Exact 64-bit representation.
 * @throws std::overflow_error when the host size exceeds 64 bits.
 */
std::uint64_t size_to_u64(std::size_t value) {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("worker protocol size exceeds uint64");
    }
  }
  return static_cast<std::uint64_t>(value);
}

/**
 * @brief Converts a transport size after host-width validation.
 * @param value Exact 64-bit representation.
 * @return Host size value.
 * @throws WorkerProtocolError when the value cannot fit `size_t`.
 */
std::size_t u64_to_size(std::uint64_t value) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw WorkerProtocolError("worker protocol size exceeds host width");
  }
  return static_cast<std::size_t>(value);
}

/**
 * @brief Encodes one immutable JobSpec by closed typed fields.
 * @param spec Valid digest-stable JobSpec.
 * @param writer Non-null payload owner.
 * @throws Contract, length, or allocation failures unchanged.
 */
void encode_job_spec(const JobSpec& spec, ByteWriter* writer) {
  validate_job_spec(spec);
  if (writer == nullptr) {
    throw std::invalid_argument("worker JobSpec writer is null");
  }
  writer->put_string(spec.graph_artifact_id().value(),
                     kMaximumOpaqueIdentityBytes);
  writer->put_u32(static_cast<std::uint32_t>(spec.target_node()));
  writer->put_string(spec.output_slot_id().value(),
                     kMaximumOpaqueIdentityBytes);
  writer->put_u8(static_cast<std::uint8_t>(spec.execution_profile()));
  writer->put_u8(static_cast<std::uint8_t>(spec.requested_durability()));
  const JobResourceRequest& resources = spec.resource_request();
  writer->put_u32(resources.cpu_slots);
  writer->put_u64(resources.host_memory_bytes);
  writer->put_u64(resources.output_bytes);
  writer->put_u64(resources.staging_bytes);
  writer->put_u64(resources.retention_bytes);
  writer->put_u32(static_cast<std::uint32_t>(resources.devices.size()));
  for (const DeviceResourceRequest& device : resources.devices) {
    writer->put_string(device.device_id, kMaximumOpaqueIdentityBytes);
    writer->put_u64(device.bytes);
  }
  writer->put_bool(spec.checkpoint_artifact_id().has_value());
  if (spec.checkpoint_artifact_id().has_value()) {
    writer->put_string(spec.checkpoint_artifact_id()->value(),
                       kMaximumOpaqueIdentityBytes);
  }
}

/**
 * @brief Decodes and reconstructs one digest-stable JobSpec.
 * @param reader Non-null current payload reader.
 * @return Newly owned immutable JobSpec.
 * @throws WorkerProtocolError for invalid enum/count/range content.
 * @throws JobSpec validation or allocation failures unchanged.
 */
std::shared_ptr<const JobSpec> read_job_spec(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("worker JobSpec reader is null");
  }
  GraphArtifactId graph(reader->get_string(kMaximumOpaqueIdentityBytes));
  const std::uint32_t target = reader->get_u32();
  if (target > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    throw WorkerProtocolError("worker JobSpec target exceeds int range");
  }
  OutputSlotId output(reader->get_string(kMaximumOpaqueIdentityBytes));
  const auto profile = static_cast<JobExecutionProfile>(reader->get_u8());
  if (profile != JobExecutionProfile::EmbeddedCpuV1) {
    throw WorkerProtocolError("worker JobSpec profile is invalid");
  }
  const auto durability = static_cast<ArtifactDurability>(reader->get_u8());
  if (durability != ArtifactDurability::CrashDurable) {
    throw WorkerProtocolError("worker JobSpec durability is invalid");
  }
  JobResourceRequest resources;
  resources.cpu_slots = reader->get_u32();
  resources.host_memory_bytes = reader->get_u64();
  resources.output_bytes = reader->get_u64();
  resources.staging_bytes = reader->get_u64();
  resources.retention_bytes = reader->get_u64();
  const std::size_t device_count = reader->get_u32();
  if (device_count > kMaximumConfiguredDevicesPerJob) {
    throw WorkerProtocolError("worker JobSpec device count exceeds its bound");
  }
  resources.devices.reserve(device_count);
  for (std::size_t index = 0U; index < device_count; ++index) {
    DeviceResourceRequest device;
    device.device_id = reader->get_string(kMaximumOpaqueIdentityBytes);
    device.bytes = reader->get_u64();
    resources.devices.push_back(std::move(device));
  }
  std::optional<ArtifactId> checkpoint;
  if (reader->get_bool()) {
    checkpoint = ArtifactId(reader->get_string(kMaximumOpaqueIdentityBytes));
  }
  return std::make_shared<const JobSpec>(
      std::move(graph), static_cast<int>(target), std::move(output),
      std::move(resources), std::move(checkpoint), profile, durability);
}

/**
 * @brief Validates one closed DataType transport value.
 * @param value Numeric representation.
 * @return Typed DataType.
 * @throws WorkerProtocolError for an unknown value.
 */
DataType parse_data_type(std::uint8_t value) {
  const auto type = static_cast<DataType>(value);
  switch (type) {
    case DataType::UINT8:
    case DataType::INT8:
    case DataType::UINT16:
    case DataType::INT16:
    case DataType::FLOAT32:
    case DataType::FLOAT64:
      return type;
  }
  throw WorkerProtocolError("worker image data type is invalid");
}

/**
 * @brief Computes one tight worker image size before any allocation.
 * @param width Positive transport width.
 * @param height Positive transport height.
 * @param channels Positive transport channel count.
 * @param type Valid closed scalar type.
 * @param row_bytes Non-null output receiving the exact tight row size.
 * @return Exact tight payload size.
 * @throws std::invalid_argument when `row_bytes` is null.
 * @throws WorkerProtocolError when shape arithmetic exceeds `std::size_t`.
 * @note This check runs before trusting worker-controlled dimensions for a
 * control-plane allocation.
 */
std::size_t checked_tight_image_payload(std::uint32_t width,
                                        std::uint32_t height,
                                        std::uint32_t channels, DataType type,
                                        std::size_t* row_bytes) {
  if (row_bytes == nullptr) {
    throw std::invalid_argument("worker image row-size output is null");
  }
  const std::size_t width_size = width;
  const std::size_t height_size = height;
  const std::size_t channel_count = channels;
  const std::size_t channel_bytes = image_buffer_bytes_per_channel(type);
  if (width_size > std::numeric_limits<std::size_t>::max() / channel_count ||
      width_size * channel_count >
          std::numeric_limits<std::size_t>::max() / channel_bytes) {
    throw WorkerProtocolError("worker report image row size overflowed");
  }
  const std::size_t checked_row = width_size * channel_count * channel_bytes;
  if (checked_row > std::numeric_limits<std::size_t>::max() / height_size) {
    throw WorkerProtocolError("worker report image size overflowed");
  }
  *row_bytes = checked_row;
  return checked_row * height_size;
}

/**
 * @brief Allocates one exact tight CPU image after complete bound validation.
 * @param width Positive width no larger than `INT_MAX`.
 * @param height Positive height no larger than `INT_MAX`.
 * @param channels Positive channel count no larger than `INT_MAX`.
 * @param type Valid closed scalar type.
 * @param row_bytes Exact prevalidated tight row size.
 * @param payload_bytes Exact positive prevalidated allocation size.
 * @return Independently owned CPU image with no row padding.
 * @throws std::bad_alloc when the bounded payload or owner cannot be allocated.
 * @note The caller validates dimensions, multiplication, frame size, and Job
 * resource limits before entering this helper.
 */
ImageBuffer make_tight_worker_image(std::uint32_t width, std::uint32_t height,
                                    std::uint32_t channels, DataType type,
                                    std::size_t row_bytes,
                                    std::size_t payload_bytes) {
  void* raw = std::malloc(payload_bytes);
  if (raw == nullptr) {
    throw std::bad_alloc();
  }
  ImageBuffer image;
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);
  image.channels = static_cast<int>(channels);
  image.type = type;
  image.device = Device::CPU;
  image.step = row_bytes;
  image.context.reset();
  image.data =
      std::shared_ptr<void>(raw, [](void* bytes) { std::free(bytes); });
  return image;
}

/**
 * @brief Encodes one immutable artifact receipt and tight payload.
 * @param artifact Valid checkpoint artifact record.
 * @param writer Non-null payload owner.
 * @throws Contract, length, digest, or allocation failures unchanged.
 */
void encode_artifact(const ArtifactRecord& artifact, ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("worker artifact writer is null");
  }
  const OutputCommitReceipt& receipt = artifact.receipt;
  encode_identity(receipt.attempt, writer);
  writer->put_string(receipt.output_slot_id.value(),
                     kMaximumOpaqueIdentityBytes);
  writer->put_string(receipt.artifact_id.value(), kMaximumOpaqueIdentityBytes);
  writer->put_string(receipt.output_commit_id.value(),
                     kMaximumOpaqueIdentityBytes);
  const ArtifactImageDescriptor& descriptor = receipt.descriptor;
  writer->put_u32(static_cast<std::uint32_t>(descriptor.width));
  writer->put_u32(static_cast<std::uint32_t>(descriptor.height));
  writer->put_u32(static_cast<std::uint32_t>(descriptor.channels));
  static_cast<void>(image_buffer_bytes_per_channel(descriptor.type));
  writer->put_u8(static_cast<std::uint8_t>(descriptor.type));
  writer->put_u64(size_to_u64(descriptor.row_bytes));
  writer->put_u64(size_to_u64(descriptor.payload_bytes));
  writer->put_raw(receipt.content_digest.bytes.data(),
                  receipt.content_digest.bytes.size());
  writer->put_u8(static_cast<std::uint8_t>(receipt.achieved_durability));
  if (descriptor.payload_bytes != artifact.payload.size() ||
      receipt.content_digest !=
          hash_artifact_content(artifact.payload.data(),
                                artifact.payload.size())) {
    throw std::invalid_argument(
        "worker checkpoint artifact payload does not match its receipt");
  }
  writer->put_blob(artifact.payload, kMaximumWorkerFramePayloadBytes);
}

/**
 * @brief Decodes and validates one immutable artifact receipt and payload.
 * @param reader Non-null current payload reader.
 * @return Independently owned artifact record.
 * @throws WorkerProtocolError for malformed descriptor/digest/durability.
 * @throws Allocation and hashing failures unchanged.
 */
ArtifactRecord read_artifact(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("worker artifact reader is null");
  }
  ArtifactRecord artifact;
  OutputCommitReceipt& receipt = artifact.receipt;
  receipt.attempt = read_identity(reader);
  receipt.output_slot_id =
      OutputSlotId(reader->get_string(kMaximumOpaqueIdentityBytes));
  receipt.artifact_id =
      ArtifactId(reader->get_string(kMaximumOpaqueIdentityBytes));
  receipt.output_commit_id =
      OutputCommitId(reader->get_string(kMaximumOpaqueIdentityBytes));
  ArtifactImageDescriptor& descriptor = receipt.descriptor;
  const std::uint32_t width = reader->get_u32();
  const std::uint32_t height = reader->get_u32();
  const std::uint32_t channels = reader->get_u32();
  if (width == 0U || height == 0U || channels == 0U ||
      width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      channels > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    throw WorkerProtocolError("worker artifact descriptor is invalid");
  }
  descriptor.width = static_cast<int>(width);
  descriptor.height = static_cast<int>(height);
  descriptor.channels = static_cast<int>(channels);
  descriptor.type = parse_data_type(reader->get_u8());
  descriptor.row_bytes = u64_to_size(reader->get_u64());
  descriptor.payload_bytes = u64_to_size(reader->get_u64());
  reader->get_raw(receipt.content_digest.bytes.data(),
                  receipt.content_digest.bytes.size());
  receipt.achieved_durability =
      static_cast<ArtifactDurability>(reader->get_u8());
  if (receipt.achieved_durability != ArtifactDurability::CrashDurable) {
    throw WorkerProtocolError("worker artifact durability is invalid");
  }
  artifact.payload = reader->get_blob(kMaximumWorkerFramePayloadBytes);
  const std::size_t width_size = static_cast<std::size_t>(descriptor.width);
  const std::size_t channel_count =
      static_cast<std::size_t>(descriptor.channels);
  const std::size_t channel_bytes =
      image_buffer_bytes_per_channel(descriptor.type);
  if (width_size > std::numeric_limits<std::size_t>::max() / channel_count ||
      width_size * channel_count >
          std::numeric_limits<std::size_t>::max() / channel_bytes) {
    throw WorkerProtocolError("worker artifact row size overflowed");
  }
  const std::size_t row_bytes = width_size * channel_count * channel_bytes;
  if (descriptor.row_bytes != row_bytes ||
      (descriptor.height > 0 &&
       row_bytes > std::numeric_limits<std::size_t>::max() /
                       static_cast<std::size_t>(descriptor.height)) ||
      descriptor.payload_bytes !=
          row_bytes * static_cast<std::size_t>(descriptor.height) ||
      descriptor.payload_bytes != artifact.payload.size() ||
      receipt.content_digest !=
          hash_artifact_content(artifact.payload.data(),
                                artifact.payload.size())) {
    throw WorkerProtocolError(
        "worker artifact descriptor or content digest is inconsistent");
  }
  return artifact;
}

/**
 * @brief Parses one closed attempt outcome.
 * @param value Numeric wire value.
 * @return Typed outcome.
 * @throws WorkerProtocolError for an unknown value.
 */
JobAttemptOutcome parse_attempt_outcome(std::uint8_t value) {
  switch (static_cast<JobAttemptOutcome>(value)) {
    case JobAttemptOutcome::None:
    case JobAttemptOutcome::Succeeded:
    case JobAttemptOutcome::Failed:
    case JobAttemptOutcome::Cancelled:
      return static_cast<JobAttemptOutcome>(value);
  }
  throw WorkerProtocolError("worker report outcome is invalid");
}

/**
 * @brief Parses one closed attempt failure value.
 * @param value Numeric wire value.
 * @return Typed failure category.
 * @throws WorkerProtocolError for an unknown value.
 */
JobAttemptFailure parse_attempt_failure(std::uint8_t value) {
  switch (static_cast<JobAttemptFailure>(value)) {
    case JobAttemptFailure::None:
    case JobAttemptFailure::InvalidAssignment:
    case JobAttemptFailure::GraphResolution:
    case JobAttemptFailure::HostSetup:
    case JobAttemptFailure::GraphLoad:
    case JobAttemptFailure::Compute:
    case JobAttemptFailure::Settlement:
    case JobAttemptFailure::CancellationObserved:
    case JobAttemptFailure::Unexpected:
    case JobAttemptFailure::ReportRejected:
    case JobAttemptFailure::ArtifactCommit:
    case JobAttemptFailure::RecoveryInterrupted:
    case JobAttemptFailure::WorkerStartup:
    case JobAttemptFailure::WorkerExit:
    case JobAttemptFailure::WorkerSignal:
    case JobAttemptFailure::WorkerChannel:
    case JobAttemptFailure::WorkerProtocol:
    case JobAttemptFailure::WorkerHeartbeatTimeout:
    case JobAttemptFailure::WorkerRuntimeTimeout:
    case JobAttemptFailure::WorkerCancellationForced:
      return static_cast<JobAttemptFailure>(value);
  }
  throw WorkerProtocolError("worker report failure category is invalid");
}

/**
 * @brief Converts trusted codec/contract exceptions into protocol rejection.
 * @tparam Function Nullary decoder callable.
 * @param function Decoder body.
 * @return Decoder result.
 * @throws WorkerProtocolError with preserved diagnostic for invalid content.
 * @throws std::bad_alloc unchanged.
 */
template <typename Function>
auto decode_checked(Function&& function) -> decltype(function()) {
  try {
    return function();
  } catch (const WorkerProtocolError&) {
    throw;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw WorkerProtocolError(std::string("invalid worker payload: ") +
                              error.what());
  }
}

}  // namespace

/** @copydoc ps::server::write_worker_frame */
void write_worker_frame(int fd, WorkerMessageKind kind,
                        const std::vector<std::byte>& payload,
                        std::chrono::steady_clock::time_point deadline) {
  if (fd < 0 || payload.size() > kMaximumWorkerFramePayloadBytes ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("worker frame output is invalid");
  }
  static_cast<void>(parse_message_kind(static_cast<std::uint16_t>(kind)));
  ByteWriter header;
  header.put_u32(kWorkerProtocolMagic);
  header.put_u16(kWorkerProtocolVersion);
  header.put_u16(static_cast<std::uint16_t>(kind));
  header.put_u32(static_cast<std::uint32_t>(payload.size()));
  const std::vector<std::byte> header_bytes = header.finish();
  if (header_bytes.size() != kWorkerFrameHeaderBytes) {
    throw std::logic_error("worker frame header size drifted");
  }
  write_all(fd, header_bytes.data(), header_bytes.size(), deadline);
  write_all(fd, payload.data(), payload.size(), deadline);
}

/** @copydoc ps::server::read_worker_frame */
WorkerProtocolFrame read_worker_frame(
    int fd, std::chrono::steady_clock::time_point deadline) {
  if (fd < 0) {
    throw WorkerChannelError("worker frame input descriptor is invalid");
  }
  std::vector<std::byte> header(kWorkerFrameHeaderBytes);
  read_all(fd, header.data(), header.size(), deadline, true);
  ByteReader header_reader(header);
  if (header_reader.get_u32() != kWorkerProtocolMagic) {
    throw WorkerProtocolError("worker frame magic is invalid");
  }
  if (header_reader.get_u16() != kWorkerProtocolVersion) {
    throw WorkerProtocolError("worker frame version is unsupported");
  }
  WorkerProtocolFrame frame;
  frame.kind = parse_message_kind(header_reader.get_u16());
  const std::size_t payload_size = header_reader.get_u32();
  header_reader.finish();
  if (payload_size > kMaximumWorkerFramePayloadBytes) {
    throw WorkerProtocolError("worker frame payload length exceeds its bound");
  }
  frame.payload.resize(payload_size);
  read_all(fd, frame.payload.data(), frame.payload.size(), deadline, false);
  return frame;
}

/** @copydoc ps::server::send_worker_assignment */
void send_worker_assignment(int fd, const PreparedWorkerAssignment& assignment,
                            std::chrono::steady_clock::time_point deadline) {
  validate_attempt_identity(assignment.assignment.identity);
  if (assignment.assignment.spec == nullptr ||
      assignment.heartbeat_interval.count() <= 0 ||
      assignment.heartbeat_interval.count() >
          std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("prepared worker assignment is invalid");
  }
  validate_job_spec(*assignment.assignment.spec);
  if (assignment.assignment.spec->digest() !=
      assignment.assignment.identity.job_spec_digest) {
    throw std::invalid_argument(
        "prepared worker assignment digest is inconsistent");
  }
  ByteWriter writer;
  encode_identity(assignment.assignment.identity, &writer);
  encode_job_spec(*assignment.assignment.spec, &writer);
  writer.put_bool(assignment.assignment.checkpoint != nullptr);
  if (assignment.assignment.checkpoint != nullptr) {
    encode_artifact(*assignment.assignment.checkpoint, &writer);
  }
  writer.put_bool(assignment.graph.ok);
  writer.put_string(assignment.graph.root_dir, kMaximumWorkerPathBytes);
  writer.put_string(assignment.graph.yaml_path, kMaximumWorkerPathBytes);
  writer.put_string(assignment.graph.config_path, kMaximumWorkerPathBytes);
  writer.put_string(assignment.graph.cache_root_dir, kMaximumWorkerPathBytes);
  writer.put_string(assignment.graph.message, kMaximumWorkerDiagnosticBytes);
  writer.put_u32(
      static_cast<std::uint32_t>(assignment.heartbeat_interval.count()));
  write_worker_frame(fd, WorkerMessageKind::Assignment, writer.finish(),
                     deadline);
}

/** @copydoc ps::server::receive_worker_assignment */
PreparedWorkerAssignment receive_worker_assignment(
    int fd, std::chrono::steady_clock::time_point deadline) {
  WorkerProtocolFrame frame = read_worker_frame(fd, deadline);
  return decode_checked([&] {
    if (frame.kind != WorkerMessageKind::Assignment) {
      throw WorkerProtocolError("worker expected one assignment frame");
    }
    ByteReader reader(frame.payload);
    PreparedWorkerAssignment prepared;
    prepared.assignment.identity = read_identity(&reader);
    prepared.assignment.spec = read_job_spec(&reader);
    if (prepared.assignment.spec->digest() !=
        prepared.assignment.identity.job_spec_digest) {
      throw WorkerProtocolError(
          "worker assignment JobSpec digest does not join identity");
    }
    if (reader.get_bool()) {
      prepared.assignment.checkpoint =
          std::make_shared<const ArtifactRecord>(read_artifact(&reader));
    }
    prepared.graph.ok = reader.get_bool();
    prepared.graph.root_dir = reader.get_string(kMaximumWorkerPathBytes);
    prepared.graph.yaml_path = reader.get_string(kMaximumWorkerPathBytes);
    prepared.graph.config_path = reader.get_string(kMaximumWorkerPathBytes);
    prepared.graph.cache_root_dir = reader.get_string(kMaximumWorkerPathBytes);
    prepared.graph.message = reader.get_string(kMaximumWorkerDiagnosticBytes);
    const std::uint32_t heartbeat_ms = reader.get_u32();
    if (heartbeat_ms == 0U) {
      throw WorkerProtocolError("worker heartbeat cadence is zero");
    }
    prepared.heartbeat_interval = std::chrono::milliseconds(heartbeat_ms);
    reader.finish();
    return prepared;
  });
}

/** @copydoc ps::server::send_worker_identity */
void send_worker_identity(int fd, WorkerMessageKind kind,
                          const AttemptIdentity& identity,
                          std::chrono::steady_clock::time_point deadline) {
  if (kind != WorkerMessageKind::AssignmentAccepted &&
      kind != WorkerMessageKind::Heartbeat &&
      kind != WorkerMessageKind::Cancel) {
    throw std::invalid_argument("worker identity frame kind is invalid");
  }
  ByteWriter writer;
  encode_identity(identity, &writer);
  write_worker_frame(fd, kind, writer.finish(), deadline);
}

/** @copydoc ps::server::decode_worker_identity */
AttemptIdentity decode_worker_identity(const WorkerProtocolFrame& frame,
                                       WorkerMessageKind expected_kind) {
  return decode_checked([&] {
    if ((expected_kind != WorkerMessageKind::AssignmentAccepted &&
         expected_kind != WorkerMessageKind::Heartbeat &&
         expected_kind != WorkerMessageKind::Cancel) ||
        frame.kind != expected_kind) {
      throw WorkerProtocolError("worker identity frame kind is unexpected");
    }
    ByteReader reader(frame.payload);
    AttemptIdentity identity = read_identity(&reader);
    reader.finish();
    return identity;
  });
}

/** @copydoc ps::server::send_worker_report */
void send_worker_report(int fd, const JobAttemptReport& report,
                        const JobSpec& spec,
                        std::chrono::steady_clock::time_point deadline) {
  validate_attempt_identity(report.identity);
  validate_job_spec(spec);
  ByteWriter writer;
  encode_identity(report.identity, &writer);
  writer.put_u8(static_cast<std::uint8_t>(report.outcome));
  writer.put_bool(report.settled);
  writer.put_u8(static_cast<std::uint8_t>(report.failure));
  writer.put_string(report.message, kMaximumWorkerDiagnosticBytes);
  writer.put_bool(report.image.has_value());
  if (report.image.has_value()) {
    const ImageBuffer& image = *report.image;
    validate_image_buffer(image);
    if (image.device != Device::CPU || image.width <= 0 || image.height <= 0 ||
        image.channels <= 0) {
      throw std::invalid_argument("worker report image is not nonempty CPU");
    }
    const std::size_t row_bytes = image_buffer_row_bytes(image);
    if (row_bytes > std::numeric_limits<std::size_t>::max() /
                        static_cast<std::size_t>(image.height)) {
      throw std::overflow_error("worker report image size overflowed");
    }
    const std::size_t payload_bytes =
        row_bytes * static_cast<std::size_t>(image.height);
    const JobResourceRequest& resources = spec.resource_request();
    const std::uint64_t payload_u64 = size_to_u64(payload_bytes);
    if (payload_bytes > kMaximumWorkerFramePayloadBytes ||
        payload_u64 > resources.output_bytes ||
        payload_u64 > resources.staging_bytes ||
        payload_u64 > resources.retention_bytes) {
      throw std::length_error("worker report image exceeds Job bounds");
    }
    writer.put_u32(static_cast<std::uint32_t>(image.width));
    writer.put_u32(static_cast<std::uint32_t>(image.height));
    writer.put_u32(static_cast<std::uint32_t>(image.channels));
    writer.put_u8(static_cast<std::uint8_t>(image.type));
    writer.put_u64(payload_u64);
    for (int row = 0; row < image.height; ++row) {
      writer.put_raw(image_buffer_row_data(image, row), row_bytes);
    }
  }
  write_worker_frame(fd, WorkerMessageKind::Report, writer.finish(), deadline);
}

/** @copydoc ps::server::decode_worker_report */
JobAttemptReport decode_worker_report(const WorkerProtocolFrame& frame,
                                      const JobSpec& spec) {
  return decode_checked([&] {
    if (frame.kind != WorkerMessageKind::Report) {
      throw WorkerProtocolError("worker expected one report frame");
    }
    validate_job_spec(spec);
    ByteReader reader(frame.payload);
    JobAttemptReport report;
    report.identity = read_identity(&reader);
    report.outcome = parse_attempt_outcome(reader.get_u8());
    report.settled = reader.get_bool();
    report.failure = parse_attempt_failure(reader.get_u8());
    report.message = reader.get_string(kMaximumWorkerDiagnosticBytes);
    if (reader.get_bool()) {
      const std::uint32_t width = reader.get_u32();
      const std::uint32_t height = reader.get_u32();
      const std::uint32_t channels = reader.get_u32();
      if (width == 0U || height == 0U || channels == 0U ||
          width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
          height >
              static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
          channels >
              static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw WorkerProtocolError("worker report image dimensions are invalid");
      }
      const DataType type = parse_data_type(reader.get_u8());
      const std::size_t declared_payload = u64_to_size(reader.get_u64());
      std::size_t row_bytes = 0U;
      const std::size_t payload_bytes = checked_tight_image_payload(
          width, height, channels, type, &row_bytes);
      const JobResourceRequest& resources = spec.resource_request();
      const std::uint64_t payload_u64 = size_to_u64(payload_bytes);
      if (declared_payload != payload_bytes ||
          payload_bytes > kMaximumWorkerFramePayloadBytes ||
          payload_u64 > resources.output_bytes ||
          payload_u64 > resources.staging_bytes ||
          payload_u64 > resources.retention_bytes) {
        throw WorkerProtocolError("worker report image exceeds exact bounds");
      }
      ImageBuffer image = make_tight_worker_image(width, height, channels, type,
                                                  row_bytes, payload_bytes);
      auto* output = static_cast<std::byte*>(image.data.get());
      for (std::uint32_t row = 0U; row < height; ++row) {
        reader.get_raw(output + static_cast<std::size_t>(row) * image.step,
                       row_bytes);
      }
      report.image = std::move(image);
    }
    reader.finish();
    return report;
  });
}

}  // namespace ps::server
