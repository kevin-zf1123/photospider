/**
 * @file worker_protocol.cpp
 * @brief Implements the metadata-only Issue #105 worker control protocol.
 */
#include "server/worker/worker_protocol.hpp"

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

#include "server/worker/worker_protocol_test_access.hpp"  // NOLINT(build/include_subdir)

namespace ps::server {
namespace {

/**
 * @brief Borrowed deterministic hooks active only on the current test thread.
 * @note Product threads retain the null default and therefore use the real
 * monotonic clock without observation callbacks.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
thread_local const WorkerProtocolDeadlineTestHooks*
    g_worker_protocol_deadline_test_hooks = nullptr;
// NOLINTEND

/**
 * @brief Returns the production clock or one current-thread test replacement.
 * @return Monotonic time used by protocol deadline calculations.
 * @throws Nothing.
 */
std::chrono::steady_clock::time_point worker_protocol_now() noexcept {
  const WorkerProtocolDeadlineTestHooks* hooks =
      g_worker_protocol_deadline_test_hooks;
  if (hooks != nullptr && hooks->now != nullptr) {
    return hooks->now(hooks->context);
  }
  return std::chrono::steady_clock::now();
}

/**
 * @brief Notifies one current-thread deterministic test observation point.
 * @param point Exact protocol boundary reached by the implementation.
 * @return Nothing.
 * @throws Nothing.
 */
void observe_worker_protocol_deadline_test_point(
    WorkerProtocolDeadlineTestPoint point) noexcept {
  const WorkerProtocolDeadlineTestHooks* hooks =
      g_worker_protocol_deadline_test_hooks;
  if (hooks != nullptr && hooks->observe != nullptr) {
    hooks->observe(hooks->context, point);
  }
}

/**
 * @brief Enforces one strict absolute protocol acceptance boundary.
 * @param deadline Absolute monotonic deadline whose equality is already late.
 * @return Nothing while current time is strictly before `deadline`.
 * @throws WorkerProtocolTimeout when current time is equal to or later than
 * `deadline`.
 */
void require_worker_protocol_before(
    std::chrono::steady_clock::time_point deadline) {
  if (worker_protocol_now() >= deadline) {
    throw WorkerProtocolTimeout("worker protocol I/O deadline expired");
  }
}

/** @brief Fixed big-endian frame magic spelling ASCII `PSW1`. */
constexpr std::uint32_t kWorkerProtocolMagic = 0x50535731U;
/** @brief Sole supported metadata-only private worker protocol version. */
constexpr std::uint16_t kWorkerProtocolVersion = 3U;
/** @brief Encoded width of one closed boolean, enum, or scalar byte. */
constexpr std::size_t kWorkerU8Bytes = sizeof(std::uint8_t);
/** @brief Encoded width of one length prefix or 32-bit scalar. */
constexpr std::size_t kWorkerU32Bytes = sizeof(std::uint32_t);
/** @brief Encoded width of one 64-bit scalar. */
constexpr std::size_t kWorkerU64Bytes = sizeof(std::uint64_t);
/** @brief Exact SHA-256 width shared by JobSpec and artifact digests. */
constexpr std::size_t kWorkerDigestBytes = sizeof(JobSpecDigest{}.bytes);

/**
 * @brief Computes the encoded width of one maximum-length prefixed field.
 * @param maximum Maximum raw field bytes.
 * @return Four-byte length prefix plus the supplied maximum.
 * @throws Nothing.
 */
constexpr std::size_t maximum_prefixed_field_bytes(
    std::size_t maximum) noexcept {
  return kWorkerU32Bytes + maximum;
}

// NOLINTBEGIN(whitespace/indent_namespace)
/** @brief Maximum encoded width of one opaque identity field. */
constexpr std::size_t kMaximumEncodedOpaqueIdentityBytes =
    maximum_prefixed_field_bytes(kMaximumOpaqueIdentityBytes);
/** @brief Maximum encoded width of one complete AttemptIdentity. */
constexpr std::size_t kMaximumEncodedAttemptIdentityBytes =
    4U * kMaximumEncodedOpaqueIdentityBytes + kWorkerDigestBytes +
    kWorkerU64Bytes;
/** @brief Maximum encoded width of one complete supported JobSpec. */
constexpr std::size_t kMaximumEncodedJobSpecBytes =
    3U * kMaximumEncodedOpaqueIdentityBytes + 3U * kWorkerU32Bytes +
    2U * kWorkerU8Bytes + 4U * kWorkerU64Bytes +
    kMaximumConfiguredDevicesPerJob *
        (kMaximumEncodedOpaqueIdentityBytes + kWorkerU64Bytes) +
    kWorkerU8Bytes;
/** @brief Maximum encoded width of one complete artifact receipt. */
constexpr std::size_t kMaximumEncodedArtifactReceiptBytes =
    kMaximumEncodedAttemptIdentityBytes +
    3U * kMaximumEncodedOpaqueIdentityBytes + 2U * kWorkerU32Bytes +
    kWorkerU64Bytes + kWorkerDigestBytes + kWorkerU8Bytes;
/** @brief Maximum encoded checkpoint/output data-plane metadata. */
constexpr std::size_t kMaximumEncodedDataPlaneAssignmentBytes =
    kWorkerU8Bytes +
    maximum_prefixed_field_bytes(kMaximumWorkerDataPlaneReferenceBytes) +
    kMaximumEncodedArtifactReceiptBytes +
    2U * maximum_prefixed_field_bytes(kMaximumOpaqueIdentityBytes) +
    maximum_prefixed_field_bytes(kMaximumWorkerDataPlaneReferenceBytes) +
    kWorkerU64Bytes;
/** @brief Maximum graph material and cadence metadata after data references. */
constexpr std::size_t kMaximumEncodedAssignmentTailBytes =
    kWorkerU8Bytes +
    5U * maximum_prefixed_field_bytes(kMaximumWorkerTextFieldBytes) +
    kWorkerU32Bytes;
/** @brief Complete worst-case metadata-only Assignment payload. */
constexpr std::size_t kMaximumEncodedAssignmentEnvelopeBytes =
    kMaximumEncodedAttemptIdentityBytes + kMaximumEncodedJobSpecBytes +
    kMaximumEncodedDataPlaneAssignmentBytes +
    kMaximumEncodedAssignmentTailBytes;
/** @brief Maximum complete metadata-only Report payload. */
constexpr std::size_t kMaximumEncodedReportEnvelopeBytes =
    kMaximumEncodedAttemptIdentityBytes + 4U * kWorkerU8Bytes +
    maximum_prefixed_field_bytes(kMaximumWorkerTextFieldBytes) +
    maximum_prefixed_field_bytes(kMaximumWorkerDataPlaneReferenceBytes) +
    kMaximumEncodedOpaqueIdentityBytes + 2U * kWorkerU32Bytes +
    kWorkerU64Bytes + kWorkerDigestBytes;

static_assert(kWorkerDigestBytes == sizeof(ArtifactContentDigest{}.bytes));
static_assert(kMaximumEncodedAssignmentEnvelopeBytes <
              kMaximumWorkerControlPayloadBytes);
static_assert(kMaximumEncodedReportEnvelopeBytes <
              kMaximumWorkerControlPayloadBytes);
// NOLINTEND

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
    if (additional > kMaximumWorkerControlPayloadBytes - bytes_.size()) {
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
    case WorkerMessageKind::CompletionReady:
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
  const auto now = worker_protocol_now();
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
 * @note A positive send is irrevocable. If its post-send deadline check fails,
 * callers must treat the write as failed and must not retry the frame. An
 * existing cancellation FSM may retain the channel only for bounded
 * receive-side report/EOF/exit drainage.
 */
void write_all(int fd, const std::byte* bytes, std::size_t size,
               std::chrono::steady_clock::time_point deadline) {
  std::size_t offset = 0U;
  while (offset != size) {
    wait_ready(fd, POLLOUT, deadline);
    require_worker_protocol_before(deadline);
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    const ssize_t written =
        ::send(fd, bytes + offset, size - offset, kSendFlags);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      observe_worker_protocol_deadline_test_point(
          WorkerProtocolDeadlineTestPoint::WriteProgressAfterSend);
      require_worker_protocol_before(deadline);
      continue;
    }
    if (written < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      require_worker_protocol_before(deadline);
      continue;
    }
    const int error = written == 0 ? EPIPE : errno;
    throw WorkerChannelError(std::string("worker protocol write failed: ") +
                             std::strerror(error));
  }
}

/**
 * @brief Advances an exact byte range while retaining its partial offset.
 * @param fd Connected private socket.
 * @param bytes Non-null output when `size` is nonzero.
 * @param size Number of bytes to receive.
 * @param offset Non-null retained offset at or below `size`.
 * @param poll_deadline Absolute monotonic readiness-wait budget. A due value
 * still permits one nonblocking probe and one read when readiness is present.
 * @param acceptance_deadline Absolute monotonic frame acceptance deadline.
 * @param clean_eof_allowed Whether zero bytes at initial offset means clean
 * frame-boundary EOF.
 * @throws std::invalid_argument for inconsistent buffer/offset state.
 * @throws WorkerProtocolEof for allowed initial EOF.
 * @throws WorkerProtocolError for truncated input.
 * @throws WorkerProtocolTimeout or WorkerChannelError on I/O failure.
 */
void read_incremental(int fd, std::byte* bytes, std::size_t size,
                      std::size_t* offset,
                      std::chrono::steady_clock::time_point poll_deadline,
                      std::chrono::steady_clock::time_point acceptance_deadline,
                      bool clean_eof_allowed) {
  if (offset == nullptr || *offset > size || (size != 0U && bytes == nullptr)) {
    throw std::invalid_argument("worker incremental read state is invalid");
  }
  while (*offset != size) {
    wait_ready(fd, POLLIN, poll_deadline);
    const ssize_t received = ::recv(fd, bytes + *offset, size - *offset, 0);
    if (received > 0) {
      *offset += static_cast<std::size_t>(received);
      require_worker_protocol_before(acceptance_deadline);
      continue;
    }
    if (received == 0) {
      if (clean_eof_allowed && *offset == 0U) {
        throw WorkerProtocolEof("worker protocol channel reached EOF");
      }
      throw WorkerProtocolError("worker protocol frame is truncated by EOF");
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      require_worker_protocol_before(acceptance_deadline);
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
 * @brief Encodes one named-Value archive descriptor without payload bytes.
 * @param descriptor Valid positive canonical archive descriptor.
 * @param writer Non-null metadata payload owner.
 * @return Nothing after all scalar fields are appended.
 * @throws std::invalid_argument for null writer or inconsistent descriptor.
 * @throws Length, type, or allocation failures unchanged.
 */
void encode_artifact_descriptor(const ValueArtifactSetDescriptor& descriptor,
                                ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("worker artifact descriptor writer is null");
  }
  if (descriptor.archive_version != 1U || descriptor.value_count == 0U ||
      descriptor.value_count > kMaximumNamedValueArtifacts ||
      descriptor.archive_bytes == 0U) {
    throw std::invalid_argument("worker artifact descriptor is inconsistent");
  }
  writer->put_u32(descriptor.archive_version);
  writer->put_u32(descriptor.value_count);
  writer->put_u64(size_to_u64(descriptor.archive_bytes));
}

/**
 * @brief Decodes one archive descriptor before any data-plane allocation.
 * @param reader Non-null current metadata reader.
 * @return Exact validated positive tight descriptor.
 * @throws WorkerProtocolError for invalid version, count, or size.
 */
ValueArtifactSetDescriptor read_artifact_descriptor(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("worker artifact descriptor reader is null");
  }
  ValueArtifactSetDescriptor descriptor;
  descriptor.archive_version = reader->get_u32();
  descriptor.value_count = reader->get_u32();
  descriptor.archive_bytes = u64_to_size(reader->get_u64());
  if (descriptor.archive_version != 1U || descriptor.value_count == 0U ||
      descriptor.value_count > kMaximumNamedValueArtifacts ||
      descriptor.archive_bytes == 0U) {
    throw WorkerProtocolError("worker artifact descriptor is invalid");
  }
  return descriptor;
}

/**
 * @brief Encodes one immutable artifact receipt without payload bytes.
 * @param receipt Valid checkpoint receipt metadata.
 * @param writer Non-null metadata payload owner.
 * @return Nothing after bounded identity/descriptor/digest metadata is added.
 * @throws Contract, length, type, or allocation failures unchanged.
 */
void encode_artifact_receipt(const OutputCommitReceipt& receipt,
                             ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("worker artifact receipt writer is null");
  }
  encode_identity(receipt.attempt, writer);
  writer->put_string(receipt.output_slot_id.value(),
                     kMaximumOpaqueIdentityBytes);
  writer->put_string(receipt.artifact_id.value(), kMaximumOpaqueIdentityBytes);
  writer->put_string(receipt.output_commit_id.value(),
                     kMaximumOpaqueIdentityBytes);
  encode_artifact_descriptor(receipt.descriptor, writer);
  writer->put_raw(receipt.content_digest.bytes.data(),
                  receipt.content_digest.bytes.size());
  writer->put_u8(static_cast<std::uint8_t>(receipt.achieved_durability));
  if (receipt.achieved_durability != ArtifactDurability::CrashDurable) {
    throw std::invalid_argument("worker artifact durability is invalid");
  }
}

/**
 * @brief Decodes and validates one immutable artifact receipt without bytes.
 * @param reader Non-null current payload reader.
 * @return Independently owned receipt metadata.
 * @throws WorkerProtocolError for malformed identity, descriptor, or
 * durability metadata.
 * @throws Allocation failures unchanged.
 */
OutputCommitReceipt read_artifact_receipt(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("worker artifact receipt reader is null");
  }
  OutputCommitReceipt receipt;
  receipt.attempt = read_identity(reader);
  receipt.output_slot_id =
      OutputSlotId(reader->get_string(kMaximumOpaqueIdentityBytes));
  receipt.artifact_id =
      ArtifactId(reader->get_string(kMaximumOpaqueIdentityBytes));
  receipt.output_commit_id =
      OutputCommitId(reader->get_string(kMaximumOpaqueIdentityBytes));
  receipt.descriptor = read_artifact_descriptor(reader);
  reader->get_raw(receipt.content_digest.bytes.data(),
                  receipt.content_digest.bytes.size());
  receipt.achieved_durability =
      static_cast<ArtifactDurability>(reader->get_u8());
  if (receipt.achieved_durability != ArtifactDurability::CrashDurable) {
    throw WorkerProtocolError("worker artifact durability is invalid");
  }
  return receipt;
}

/**
 * @brief Compares every immutable field in two artifact receipts.
 * @param left First complete receipt.
 * @param right Second complete receipt.
 * @return True only when provenance, ids, descriptor, digest, and durability
 * all match exactly.
 * @throws Nothing.
 * @note This is an equality join, not artifact authorization; durable state
 * remains the sole authority that produced the manager-side receipt.
 */
bool artifact_receipts_equal(const OutputCommitReceipt& left,
                             const OutputCommitReceipt& right) noexcept {
  return left.attempt == right.attempt &&
         left.output_slot_id == right.output_slot_id &&
         left.artifact_id == right.artifact_id &&
         left.output_commit_id == right.output_commit_id &&
         left.descriptor == right.descriptor &&
         left.content_digest == right.content_digest &&
         left.achieved_durability == right.achieved_durability;
}

/**
 * @brief Encodes checkpoint/output data-plane Assignment metadata.
 * @param data_plane Exact validated metadata.
 * @param writer Non-null control payload owner.
 * @return Nothing after bounded metadata is appended.
 * @throws Contract, length, or allocation failures unchanged.
 */
void encode_data_plane_assignment(const WorkerDataPlaneAssignment& data_plane,
                                  ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("worker data-plane writer is null");
  }
  writer->put_bool(data_plane.checkpoint.has_value());
  if (data_plane.checkpoint.has_value()) {
    writer->put_string(data_plane.checkpoint->reference_id,
                       kMaximumWorkerDataPlaneReferenceBytes);
    encode_artifact_receipt(data_plane.checkpoint->receipt, writer);
  }
  writer->put_string(data_plane.output.reference_id,
                     kMaximumWorkerDataPlaneReferenceBytes);
  writer->put_string(data_plane.output.output_slot_id.value(),
                     kMaximumOpaqueIdentityBytes);
  writer->put_u64(size_to_u64(data_plane.output.maximum_payload_bytes));
}

/**
 * @brief Decodes checkpoint/output data-plane Assignment metadata.
 * @param reader Non-null current control payload reader.
 * @return Independently owned bounded metadata.
 * @throws WorkerProtocolError for malformed/truncated fields.
 */
WorkerDataPlaneAssignment read_data_plane_assignment(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("worker data-plane reader is null");
  }
  WorkerDataPlaneAssignment data_plane;
  if (reader->get_bool()) {
    WorkerCheckpointDataReference checkpoint;
    checkpoint.reference_id =
        reader->get_string(kMaximumWorkerDataPlaneReferenceBytes);
    checkpoint.receipt = read_artifact_receipt(reader);
    data_plane.checkpoint = std::move(checkpoint);
  }
  data_plane.output.reference_id =
      reader->get_string(kMaximumWorkerDataPlaneReferenceBytes);
  data_plane.output.output_slot_id =
      OutputSlotId(reader->get_string(kMaximumOpaqueIdentityBytes));
  data_plane.output.maximum_payload_bytes = u64_to_size(reader->get_u64());
  return data_plane;
}

/**
 * @brief Encodes one candidate output-stage reference without Value bytes.
 * @param output Exact worker-produced staged-output metadata.
 * @param writer Non-null control payload owner.
 * @return Nothing after bounded metadata is appended.
 * @throws Contract, length, or allocation failures unchanged.
 */
void encode_output_reference(const WorkerOutputDataReference& output,
                             ByteWriter* writer) {
  if (writer == nullptr) {
    throw std::invalid_argument("worker output reference writer is null");
  }
  writer->put_string(output.reference_id,
                     kMaximumWorkerDataPlaneReferenceBytes);
  writer->put_string(output.output_slot_id.value(),
                     kMaximumOpaqueIdentityBytes);
  encode_artifact_descriptor(output.descriptor, writer);
  writer->put_raw(output.content_digest.bytes.data(),
                  output.content_digest.bytes.size());
}

/**
 * @brief Decodes one candidate output-stage reference without Value bytes.
 * @param reader Non-null current control payload reader.
 * @return Independently owned bounded staged-output metadata.
 * @throws WorkerProtocolError for malformed/truncated fields.
 */
WorkerOutputDataReference read_output_reference(ByteReader* reader) {
  if (reader == nullptr) {
    throw std::invalid_argument("worker output reference reader is null");
  }
  WorkerOutputDataReference output;
  output.reference_id =
      reader->get_string(kMaximumWorkerDataPlaneReferenceBytes);
  output.output_slot_id =
      OutputSlotId(reader->get_string(kMaximumOpaqueIdentityBytes));
  output.descriptor = read_artifact_descriptor(reader);
  reader->get_raw(output.content_digest.bytes.data(),
                  output.content_digest.bytes.size());
  return output;
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

/**
 * @brief Enforces the shared byte bound for one Assignment graph text field.
 * @param field_name Stable source-field name used in validation diagnostics.
 * @param value Exact bytes that would be retained or encoded.
 * @return Nothing when `value` is at or below the inclusive shared bound.
 * @throws std::length_error when `value` exceeds the private worker bound.
 * @throws std::bad_alloc when constructing the validation diagnostic fails.
 * @note The comparison is byte-based because the private codec transports
 * opaque string bytes and prefixes their exact byte count.
 */
void validate_worker_assignment_graph_text_field(std::string_view field_name,
                                                 std::string_view value) {
  if (value.size() <= kMaximumWorkerTextFieldBytes) {
    return;
  }
  throw std::length_error("worker assignment graph " + std::string(field_name) +
                          " has " + std::to_string(value.size()) +
                          " bytes; maximum is " +
                          std::to_string(kMaximumWorkerTextFieldBytes));
}

}  // namespace

/** @copydoc
 * ps::server::ScopedWorkerProtocolDeadlineTestHooks::ScopedWorkerProtocolDeadlineTestHooks
 */
ScopedWorkerProtocolDeadlineTestHooks::ScopedWorkerProtocolDeadlineTestHooks(
    const WorkerProtocolDeadlineTestHooks* hooks) noexcept
    : previous_(g_worker_protocol_deadline_test_hooks) {
  g_worker_protocol_deadline_test_hooks = hooks;
}

/** @copydoc
 * ps::server::ScopedWorkerProtocolDeadlineTestHooks::~ScopedWorkerProtocolDeadlineTestHooks
 */
ScopedWorkerProtocolDeadlineTestHooks::
    ~ScopedWorkerProtocolDeadlineTestHooks() noexcept {
  g_worker_protocol_deadline_test_hooks = previous_;
}

/** @copydoc ps::server::validate_worker_assignment_graph_transport */
void validate_worker_assignment_graph_transport(
    const ResolvedGraphArtifact& graph) {
  validate_worker_assignment_graph_text_field("root_dir", graph.root_dir);
  validate_worker_assignment_graph_text_field("yaml_path", graph.yaml_path);
  validate_worker_assignment_graph_text_field("config_path", graph.config_path);
  validate_worker_assignment_graph_text_field("cache_root_dir",
                                              graph.cache_root_dir);
  validate_worker_assignment_graph_text_field("message", graph.message);
}

/** @copydoc ps::server::write_worker_frame */
void write_worker_frame(int fd, WorkerMessageKind kind,
                        const std::vector<std::byte>& payload,
                        std::chrono::steady_clock::time_point deadline) {
  if (fd < 0 || payload.size() > kMaximumWorkerControlPayloadBytes ||
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
  require_worker_protocol_before(deadline);
}

/** @copydoc ps::server::WorkerFrameDecoder::read_frame */
WorkerProtocolFrame WorkerFrameDecoder::read_frame(
    int fd, std::chrono::steady_clock::time_point deadline) {
  return read_frame(fd, deadline, deadline);
}

/** @copydoc ps::server::WorkerFrameDecoder::read_frame */
WorkerProtocolFrame WorkerFrameDecoder::read_frame(
    int fd, std::chrono::steady_clock::time_point poll_deadline,
    std::chrono::steady_clock::time_point acceptance_deadline) {
  static_cast<void>(inspect_frame(fd, poll_deadline, acceptance_deadline));
  return accept_frame(acceptance_deadline).frame;
}

/** @copydoc ps::server::WorkerFrameDecoder::inspect_frame */
const WorkerProtocolFrame& WorkerFrameDecoder::inspect_frame(
    int fd, std::chrono::steady_clock::time_point deadline) {
  return inspect_frame(fd, deadline, deadline);
}

/** @copydoc ps::server::WorkerFrameDecoder::inspect_frame */
const WorkerProtocolFrame& WorkerFrameDecoder::inspect_frame(
    int fd, std::chrono::steady_clock::time_point poll_deadline,
    std::chrono::steady_clock::time_point acceptance_deadline) {
  if (!retained_frame_.has_value()) {
    retained_frame_ = advance_frame(fd, poll_deadline, acceptance_deadline);
  } else {
    require_worker_protocol_before(acceptance_deadline);
  }
  return *retained_frame_;
}

/** @copydoc ps::server::WorkerFrameDecoder::accept_frame */
AcceptedWorkerProtocolFrame WorkerFrameDecoder::accept_frame(
    std::chrono::steady_clock::time_point acceptance_deadline) {
  if (!retained_frame_.has_value()) {
    throw std::logic_error(
        "worker protocol semantic acceptance has no complete frame");
  }
  observe_worker_protocol_deadline_test_point(
      WorkerProtocolDeadlineTestPoint::FrameSemanticReadyBeforeAcceptance);
  const std::chrono::steady_clock::time_point accepted_at =
      worker_protocol_now();
  if (accepted_at >= acceptance_deadline) {
    throw WorkerProtocolTimeout("worker protocol I/O deadline expired");
  }
  AcceptedWorkerProtocolFrame accepted{std::move(*retained_frame_),
                                       accepted_at};
  retained_frame_.reset();
  return accepted;
}

/** @copydoc ps::server::WorkerFrameDecoder::advance_frame */
WorkerProtocolFrame WorkerFrameDecoder::advance_frame(
    int fd, std::chrono::steady_clock::time_point poll_deadline,
    std::chrono::steady_clock::time_point acceptance_deadline) {
  if (fd < 0) {
    throw WorkerChannelError("worker frame input descriptor is invalid");
  }
  const auto effective_poll_deadline =
      std::min(poll_deadline, acceptance_deadline);
  if (!header_decoded_) {
    read_incremental(fd, header_.data(), header_.size(), &header_offset_,
                     effective_poll_deadline, acceptance_deadline, true);
    const std::vector<std::byte> header(header_.begin(), header_.end());
    ByteReader header_reader(header);
    if (header_reader.get_u32() != kWorkerProtocolMagic) {
      throw WorkerProtocolError("worker frame magic is invalid");
    }
    if (header_reader.get_u16() != kWorkerProtocolVersion) {
      throw WorkerProtocolError("worker frame version is unsupported");
    }
    kind_ = parse_message_kind(header_reader.get_u16());
    const std::size_t payload_size = header_reader.get_u32();
    header_reader.finish();
    if (payload_size > kMaximumWorkerControlPayloadBytes) {
      throw WorkerProtocolError(
          "worker frame payload length exceeds its bound");
    }
    payload_.resize(payload_size);
    header_decoded_ = true;
  }

  read_incremental(fd, payload_.data(), payload_.size(), &payload_offset_,
                   effective_poll_deadline, acceptance_deadline, false);
  observe_worker_protocol_deadline_test_point(
      WorkerProtocolDeadlineTestPoint::FrameReadyBeforeAcceptance);
  require_worker_protocol_before(acceptance_deadline);
  WorkerProtocolFrame frame;
  frame.kind = kind_;
  frame.payload = std::move(payload_);
  reset();
  return frame;
}

/** @copydoc ps::server::WorkerFrameDecoder::reset */
void WorkerFrameDecoder::reset() noexcept {
  header_offset_ = 0U;
  header_decoded_ = false;
  kind_ = WorkerMessageKind::Assignment;
  payload_.clear();
  payload_offset_ = 0U;
}

/** @copydoc ps::server::read_worker_frame */
WorkerProtocolFrame read_worker_frame(
    int fd, std::chrono::steady_clock::time_point deadline) {
  WorkerFrameDecoder decoder;
  return decoder.read_frame(fd, deadline);
}

/** @copydoc ps::server::encode_worker_assignment */
WorkerProtocolFrame encode_worker_assignment(
    const PreparedWorkerAssignment& assignment) {
  if (assignment.assignment.spec == nullptr) {
    throw std::invalid_argument("prepared worker assignment has no JobSpec");
  }
  const bool checkpoint_declared =
      assignment.assignment.spec->checkpoint_artifact_id().has_value();
  if (checkpoint_declared != (assignment.assignment.checkpoint != nullptr)) {
    throw std::invalid_argument(
        "prepared worker checkpoint payload binding is incomplete");
  }
  if (assignment.assignment.checkpoint != nullptr) {
    const ArtifactRecord& checkpoint = *assignment.assignment.checkpoint;
    if (!assignment.data_plane.checkpoint.has_value()) {
      throw std::invalid_argument(
          "prepared worker checkpoint metadata is absent");
    }
    const OutputCommitReceipt& metadata =
        assignment.data_plane.checkpoint->receipt;
    const std::vector<std::byte> archive =
        encode_named_value_artifact_set(checkpoint.values);
    if (!artifact_receipts_equal(checkpoint.receipt, metadata) ||
        checkpoint.receipt.descriptor.archive_bytes != archive.size() ||
        checkpoint.receipt.content_digest !=
            hash_artifact_content(archive.data(), archive.size())) {
      throw std::invalid_argument(
          "prepared worker checkpoint metadata or size is inconsistent");
    }
  }
  return encode_worker_assignment_metadata(assignment);
}

/** @copydoc ps::server::encode_worker_assignment_metadata */
WorkerProtocolFrame encode_worker_assignment_metadata(
    const PreparedWorkerAssignment& assignment) {
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
  validate_worker_data_plane_assignment(assignment.assignment.identity,
                                        *assignment.assignment.spec,
                                        assignment.data_plane);
  validate_worker_assignment_graph_transport(assignment.graph);
  ByteWriter writer;
  encode_identity(assignment.assignment.identity, &writer);
  encode_job_spec(*assignment.assignment.spec, &writer);
  encode_data_plane_assignment(assignment.data_plane, &writer);
  writer.put_bool(assignment.graph.ok);
  writer.put_string(assignment.graph.root_dir, kMaximumWorkerTextFieldBytes);
  writer.put_string(assignment.graph.yaml_path, kMaximumWorkerTextFieldBytes);
  writer.put_string(assignment.graph.config_path, kMaximumWorkerTextFieldBytes);
  writer.put_string(assignment.graph.cache_root_dir,
                    kMaximumWorkerTextFieldBytes);
  writer.put_string(assignment.graph.message, kMaximumWorkerTextFieldBytes);
  writer.put_u32(
      static_cast<std::uint32_t>(assignment.heartbeat_interval.count()));
  return WorkerProtocolFrame{WorkerMessageKind::Assignment, writer.finish()};
}

/** @copydoc ps::server::send_worker_assignment */
void send_worker_assignment(int fd, const PreparedWorkerAssignment& assignment,
                            std::chrono::steady_clock::time_point deadline) {
  WorkerProtocolFrame frame = encode_worker_assignment(assignment);
  write_worker_frame(fd, frame.kind, frame.payload, deadline);
}

/** @copydoc ps::server::decode_worker_assignment */
PreparedWorkerAssignment decode_worker_assignment(
    const WorkerProtocolFrame& frame) {
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
    prepared.data_plane = read_data_plane_assignment(&reader);
    validate_worker_data_plane_assignment(prepared.assignment.identity,
                                          *prepared.assignment.spec,
                                          prepared.data_plane);
    prepared.graph.ok = reader.get_bool();
    prepared.graph.root_dir = reader.get_string(kMaximumWorkerTextFieldBytes);
    prepared.graph.yaml_path = reader.get_string(kMaximumWorkerTextFieldBytes);
    prepared.graph.config_path =
        reader.get_string(kMaximumWorkerTextFieldBytes);
    prepared.graph.cache_root_dir =
        reader.get_string(kMaximumWorkerTextFieldBytes);
    prepared.graph.message = reader.get_string(kMaximumWorkerTextFieldBytes);
    const std::uint32_t heartbeat_ms = reader.get_u32();
    if (heartbeat_ms == 0U) {
      throw WorkerProtocolError("worker heartbeat cadence is zero");
    }
    prepared.heartbeat_interval = std::chrono::milliseconds(heartbeat_ms);
    reader.finish();
    return prepared;
  });
}

/** @copydoc ps::server::receive_worker_assignment */
PreparedWorkerAssignment receive_worker_assignment(
    int fd, std::chrono::steady_clock::time_point deadline) {
  WorkerFrameDecoder decoder;
  const WorkerProtocolFrame& frame = decoder.inspect_frame(fd, deadline);
  PreparedWorkerAssignment decoded = decode_worker_assignment(frame);
  static_cast<void>(decoder.accept_frame(deadline));
  return decoded;
}

/** @copydoc ps::server::send_worker_identity */
void send_worker_identity(int fd, WorkerMessageKind kind,
                          const AttemptIdentity& identity,
                          std::chrono::steady_clock::time_point deadline) {
  if (kind != WorkerMessageKind::AssignmentAccepted &&
      kind != WorkerMessageKind::Heartbeat &&
      kind != WorkerMessageKind::Cancel &&
      kind != WorkerMessageKind::CompletionReady) {
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
         expected_kind != WorkerMessageKind::Cancel &&
         expected_kind != WorkerMessageKind::CompletionReady) ||
        frame.kind != expected_kind) {
      throw WorkerProtocolError("worker identity frame kind is unexpected");
    }
    ByteReader reader(frame.payload);
    AttemptIdentity identity = read_identity(&reader);
    reader.finish();
    return identity;
  });
}

/** @copydoc ps::server::encode_worker_report */
WorkerProtocolFrame encode_worker_report(
    const PreparedWorkerReport& prepared, const JobSpec& spec,
    const WorkerOutputStageReference& output_stage) {
  const JobAttemptReport& report = prepared.report;
  validate_attempt_identity(report.identity);
  validate_job_spec(spec);
  if (report.identity.job_spec_digest != spec.digest() ||
      report.values.has_value() ||
      output_stage.output_slot_id != spec.output_slot_id() ||
      output_stage.maximum_payload_bytes !=
          std::min({u64_to_size(spec.resource_request().output_bytes),
                    u64_to_size(spec.resource_request().staging_bytes),
                    u64_to_size(spec.resource_request().retention_bytes)})) {
    throw std::invalid_argument("worker metadata report join is invalid");
  }
  const bool successful_shape =
      report.outcome == JobAttemptOutcome::Succeeded && report.settled &&
      report.failure == JobAttemptFailure::None;
  if (successful_shape != prepared.output.has_value()) {
    throw std::invalid_argument(
        "worker metadata report output presence is invalid");
  }
  if (prepared.output.has_value() &&
      (prepared.output->reference_id != output_stage.reference_id ||
       prepared.output->output_slot_id != output_stage.output_slot_id ||
       prepared.output->descriptor.archive_bytes >
           output_stage.maximum_payload_bytes)) {
    throw std::invalid_argument(
        "worker metadata report output reference is invalid");
  }
  ByteWriter writer;
  encode_identity(report.identity, &writer);
  writer.put_u8(static_cast<std::uint8_t>(report.outcome));
  writer.put_bool(report.settled);
  writer.put_u8(static_cast<std::uint8_t>(report.failure));
  writer.put_string(report.message, kMaximumWorkerTextFieldBytes);
  writer.put_bool(prepared.output.has_value());
  if (prepared.output.has_value()) {
    encode_output_reference(*prepared.output, &writer);
  }
  return WorkerProtocolFrame{WorkerMessageKind::Report, writer.finish()};
}

/** @copydoc ps::server::send_worker_report */
void send_worker_report(int fd, const PreparedWorkerReport& report,
                        const JobSpec& spec,
                        const WorkerOutputStageReference& output_stage,
                        std::chrono::steady_clock::time_point deadline) {
  WorkerProtocolFrame frame = encode_worker_report(report, spec, output_stage);
  write_worker_frame(fd, frame.kind, frame.payload, deadline);
}

/** @copydoc ps::server::decode_worker_report */
PreparedWorkerReport decode_worker_report(
    const WorkerProtocolFrame& frame, const JobSpec& spec,
    const WorkerOutputStageReference& output_stage) {
  return decode_checked([&] {
    if (frame.kind != WorkerMessageKind::Report) {
      throw WorkerProtocolError("worker expected one report frame");
    }
    validate_job_spec(spec);
    ByteReader reader(frame.payload);
    PreparedWorkerReport prepared;
    JobAttemptReport& report = prepared.report;
    report.identity = read_identity(&reader);
    if (report.identity.job_spec_digest != spec.digest() ||
        output_stage.output_slot_id != spec.output_slot_id()) {
      throw WorkerProtocolError("worker report JobSpec join is invalid");
    }
    report.outcome = parse_attempt_outcome(reader.get_u8());
    report.settled = reader.get_bool();
    report.failure = parse_attempt_failure(reader.get_u8());
    report.message = reader.get_string(kMaximumWorkerTextFieldBytes);
    if (reader.get_bool()) {
      prepared.output = read_output_reference(&reader);
    }
    reader.finish();
    const bool successful_shape =
        report.outcome == JobAttemptOutcome::Succeeded && report.settled &&
        report.failure == JobAttemptFailure::None;
    if (successful_shape != prepared.output.has_value()) {
      throw WorkerProtocolError(
          "worker report output presence does not match outcome");
    }
    if (prepared.output.has_value() &&
        (prepared.output->reference_id != output_stage.reference_id ||
         prepared.output->output_slot_id != output_stage.output_slot_id ||
         prepared.output->descriptor.archive_bytes >
             output_stage.maximum_payload_bytes)) {
      throw WorkerProtocolError(
          "worker report output metadata exceeds its assigned stage");
    }
    return prepared;
  });
}

}  // namespace ps::server
