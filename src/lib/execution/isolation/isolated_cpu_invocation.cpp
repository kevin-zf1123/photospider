/**
 * @file isolated_cpu_invocation.cpp
 * @brief Implements non-supervised transport and bounded runtime supervision.
 */
#include "execution/isolation/isolated_cpu_invocation.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "execution/device/plugin_runtime_supervisor.hpp"  // NOLINT(build/include_subdir)
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#include <openssl/evp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/value_descriptor_metadata.hpp"  // NOLINT(build/include_subdir)
#include "execution/isolation/isolated_cpu_invocation_test_probe.hpp"  // NOLINT(build/include_subdir)

namespace ps::execution {
namespace {

/** @brief Fixed close-on-exec child setup-status descriptor. */
constexpr int kIsolatedCpuRuntimeSetupDescriptor = 4;
/** @brief Fixed retained executable descriptor used by `fexecve`. */
constexpr int kPluginRuntimeExecutableDescriptor = 6;
/** @brief First descriptor closed after every fixed runtime capability. */
constexpr int kFirstPluginRuntimeClosedDescriptor = 7;
/** @brief Fixed capability header width before one tensor payload range. */
constexpr std::size_t kCapabilityHeaderBytes = 40U;
/** @brief Capability header magic spelling ASCII `PSC1`. */
constexpr std::uint32_t kCapabilityHeaderMagic = 0x50534331U;
/** @brief Exact capability header structural version. */
constexpr std::uint16_t kCapabilityHeaderVersion = 1U;

/**
 * @brief Decodes one canonical operation identity into its two opaque words.
 * @param identity Pointer-free big-endian identity bytes.
 * @return Equivalent process-independent extension identity.
 * @throws Nothing.
 */
ExtensionIdentity dense_tensor_extension_identity(
    const IsolatedCpuOpaqueId& identity) noexcept {
  std::array<std::uint64_t, 2U> words{};
  for (std::size_t word = 0U; word < words.size(); ++word) {
    for (std::size_t byte = 0U; byte < 8U; ++byte) {
      words[word] = (words[word] << 8U) | std::to_integer<std::uint8_t>(
                                              identity.bytes[word * 8U + byte]);
    }
  }
  return ExtensionIdentity{words[0], words[1]};
}

/**
 * @brief Builds immutable DenseTensor metadata from one validated output plan.
 * @param plan Exact high-level tensor plan retained before process execution.
 * @return Equivalent identities, versions, and opaque digest words.
 * @throws Nothing.
 * @note Facet identity is zero exactly for a facet-free plan; Schema/Layout
 * identity, versions, and all three opaque digests are always retained.
 */
DenseTensorValueDescriptorMetadata dense_tensor_value_metadata(
    const IsolatedCpuDenseTensorOutputPlan& plan) noexcept {
  DenseTensorValueDescriptorMetadata metadata;
  metadata.schema_identity =
      dense_tensor_extension_identity(plan.schema_identity);
  metadata.facet_identity =
      dense_tensor_extension_identity(plan.facet_identity);
  metadata.layout_identity =
      dense_tensor_extension_identity(plan.layout_identity);
  metadata.descriptor_version = plan.schema_version;
  metadata.layout_version = plan.layout_version;
  metadata.descriptor_digest = plan.descriptor_digest.words;
  metadata.content_digest = plan.logical_content_digest.words;
  metadata.layout_digest = plan.layout_digest.words;
  return metadata;
}

/**
 * @brief Invokes POSIX close after descriptor ownership has been cleared.
 * @param descriptor Nonnegative descriptor.
 * @return Nothing.
 * @throws Nothing; close result including EINTR is deliberately ignored.
 * @note Retrying could close an unrelated descriptor after numeric reuse.
 */
void close_once(int descriptor) noexcept {
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
  }
}

/**
 * @brief Unique owner for one local POSIX descriptor.
 * @throws Nothing for construction, moves, reset, and destruction.
 */
class UniqueFd final {
 public:
  /** @brief Creates an empty owner. */
  UniqueFd() noexcept = default;

  /**
   * @brief Takes ownership of one descriptor or the invalid sentinel.
   * @param descriptor Descriptor to own, or -1.
   * @throws Nothing.
   */
  explicit UniqueFd(int descriptor) noexcept : descriptor_(descriptor) {}

  /** @brief Clears ownership before one close attempt. */
  ~UniqueFd() noexcept { reset(); }

  /** @brief Prevents duplicate descriptor ownership. */
  UniqueFd(const UniqueFd&) = delete;
  /** @brief Prevents duplicate descriptor assignment. */
  UniqueFd& operator=(const UniqueFd&) = delete;

  /**
   * @brief Transfers complete descriptor ownership.
   * @param other Source owner cleared by the move.
   * @throws Nothing.
   */
  UniqueFd(UniqueFd&& other) noexcept : descriptor_(other.release()) {}

  /**
   * @brief Replaces this descriptor with transferred ownership.
   * @param other Source owner cleared by the move.
   * @return This owner.
   * @throws Nothing.
   */
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  /**
   * @brief Returns the retained descriptor without transfer.
   * @return Descriptor or -1.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Reports whether a descriptor is retained.
   * @return True for a nonnegative descriptor.
   * @throws Nothing.
   */
  bool valid() const noexcept { return descriptor_ >= 0; }

  /**
   * @brief Transfers the retained descriptor to the caller.
   * @return Prior descriptor or -1.
   * @throws Nothing.
   */
  int release() noexcept { return std::exchange(descriptor_, -1); }

  /**
   * @brief Replaces ownership and closes the prior descriptor once.
   * @param replacement New descriptor or -1.
   * @throws Nothing.
   */
  void reset(int replacement = -1) noexcept {
    const int prior = std::exchange(descriptor_, replacement);
    close_once(prior);
  }

 private:
  /** @brief Sole owned descriptor or invalid sentinel. */
  int descriptor_ = -1;
};

/**
 * @brief Unique owner for one complete shared-memory mapping.
 * @throws Nothing for construction, moves, reset, and destruction.
 */
class UniqueMapping final {
 public:
  /** @brief Creates an empty mapping owner. */
  UniqueMapping() noexcept = default;

  /**
   * @brief Takes ownership of one successful mmap result.
   * @param address Non-null mapping start.
   * @param size Positive mapping byte length.
   * @throws Nothing.
   */
  UniqueMapping(void* address, std::size_t size) noexcept
      : address_(address), size_(size) {}

  /** @brief Clears ownership before one munmap attempt. */
  ~UniqueMapping() noexcept { reset(); }

  /** @brief Prevents duplicate mapping ownership. */
  UniqueMapping(const UniqueMapping&) = delete;
  /** @brief Prevents duplicate mapping assignment. */
  UniqueMapping& operator=(const UniqueMapping&) = delete;

  /**
   * @brief Transfers complete mapping ownership.
   * @param other Source owner cleared by the move.
   * @throws Nothing.
   */
  UniqueMapping(UniqueMapping&& other) noexcept
      : address_(std::exchange(other.address_, nullptr)),
        size_(std::exchange(other.size_, 0U)) {}

  /**
   * @brief Replaces this mapping with transferred ownership.
   * @param other Source owner cleared by the move.
   * @return This owner.
   * @throws Nothing.
   */
  UniqueMapping& operator=(UniqueMapping&& other) noexcept {
    if (this != &other) {
      reset();
      address_ = std::exchange(other.address_, nullptr);
      size_ = std::exchange(other.size_, 0U);
    }
    return *this;
  }

  /**
   * @brief Returns the mapping start as mutable bytes.
   * @return Mapping start, or null for an empty owner.
   * @throws Nothing.
   */
  std::byte* data() const noexcept { return static_cast<std::byte*>(address_); }

  /**
   * @brief Returns the complete mapping byte length.
   * @return Positive length or zero for an empty owner.
   * @throws Nothing.
   */
  std::size_t size() const noexcept { return size_; }

  /**
   * @brief Unmaps the retained region once after clearing ownership.
   * @throws Nothing; munmap failure cannot justify a second overlapping owner.
   */
  void reset() noexcept {
    void* address = std::exchange(address_, nullptr);
    const std::size_t size = std::exchange(size_, 0U);
    if (address != nullptr) {
      static_cast<void>(::munmap(address, size));
    }
  }

 private:
  /** @brief Sole mapping start, or null. */
  void* address_ = nullptr;
  /** @brief Exact mapping bytes, or zero. */
  std::size_t size_ = 0U;
};

/**
 * @brief Best-effort owner for a newly created shared-memory name.
 * @throws Nothing for construction, moves, release, and destruction.
 * @note The name exists only while this guard is armed and is never sent.
 */
class SharedMemoryName final {
 public:
  /**
   * @brief Retains one created POSIX shared-memory name.
   * @param name Exact leading-slash name.
   * @throws std::bad_alloc when string ownership cannot allocate.
   */
  explicit SharedMemoryName(std::string name) : name_(std::move(name)) {}

  /** @brief Unlinks an armed name once. */
  ~SharedMemoryName() noexcept { unlink(); }

  /** @brief Prevents duplicate namespace cleanup ownership. */
  SharedMemoryName(const SharedMemoryName&) = delete;
  /** @brief Prevents duplicate namespace cleanup assignment. */
  SharedMemoryName& operator=(const SharedMemoryName&) = delete;

  /**
   * @brief Transfers sole namespace cleanup ownership.
   * @param other Source guard disarmed by the move.
   * @throws Nothing under standard string move construction.
   */
  SharedMemoryName(SharedMemoryName&& other) noexcept
      : name_(std::move(other.name_)) {
    other.name_.clear();
  }

  /**
   * @brief Replaces this name after unlinking its prior owned object.
   * @param other Source guard disarmed by the move.
   * @return This guard.
   * @throws Nothing under standard string move assignment.
   */
  SharedMemoryName& operator=(SharedMemoryName&& other) noexcept {
    if (this != &other) {
      unlink();
      name_ = std::move(other.name_);
      other.name_.clear();
    }
    return *this;
  }

  /**
   * @brief Returns the retained name for immediate reopen.
   * @return Borrowed null-terminated name.
   * @throws Nothing.
   */
  const char* c_str() const noexcept { return name_.c_str(); }

  /**
   * @brief Unlinks the retained name once and clears ownership.
   * @throws Nothing; the caller separately fails when unlink is authoritative.
   */
  void unlink() noexcept {
    if (!name_.empty()) {
      static_cast<void>(::shm_unlink(name_.c_str()));
      name_.clear();
    }
  }

  /**
   * @brief Clears namespace ownership after a successful explicit unlink.
   * @throws Nothing.
   */
  void release() noexcept { name_.clear(); }

 private:
  /** @brief Exact still-owned name, or empty after unlink/release. */
  std::string name_;
};

/**
 * @brief One retained wire capability plus its local FD and mapping.
 * @throws Nothing for moves and destruction.
 */
struct MappedCapability final {
  /** @brief Exact validated wire declaration. */
  IsolatedCpuCapability capability;
  /** @brief Sole local descriptor owner. */
  UniqueFd descriptor;
  /** @brief Sole complete mapping owner. */
  UniqueMapping mapping;
};

/**
 * @brief One received packet plus every installed ancillary descriptor.
 * @throws Nothing for moves and destruction.
 */
struct ReceivedPacket final {
  /** @brief Exact packet bytes. */
  std::vector<std::byte> packet;
  /** @brief Sole owners for all received FDs, including malformed extras. */
  std::vector<UniqueFd> descriptors;
};

/**
 * @brief Platform input for closing every inherited post-fork descriptor.
 * @throws Nothing for ordinary value operations.
 */
struct ChildDescriptorClosurePlan final {
  /** @brief First inherited descriptor that must be closed. */
  int first_closed_descriptor = kFirstPluginRuntimeClosedDescriptor;
  /** @brief Darwin kernel-exclusive descriptor ceiling; unused on Linux. */
  int darwin_exclusive_maximum = 0;
};

/** @brief Monotonic process-local suffix for collision-resistant `O_EXCL`. */
std::atomic<std::uint64_t> g_shared_memory_sequence{1U};
/** @brief Monotonic Host attempts to enter capability materialization. */
std::atomic<std::uint64_t> g_host_capability_materialization_attempts{0U};
/** @brief Monotonic successful parent-side fresh child creations. */
std::atomic<std::uint64_t> g_spawned_children{0U};
/** @brief Monotonic exact child PIDs returned by blocking `waitpid`. */
std::atomic<std::uint64_t> g_reaped_children{0U};
/** @brief Most recent exactly reaped child PID, or -1 before any reap. */
std::atomic<std::int64_t> g_last_reaped_child{-1};
/** @brief Monotonic frames reaching their exact declared byte length. */
std::atomic<std::uint64_t> g_exact_frames_received{0U};
/** @brief One-shot supervised request-send delay used only by tests. */
std::atomic<std::int64_t> g_next_supervised_request_send_delay_ms{0};
/**
 * @brief Owns the one-shot successful request-shutdown acceptance test delay.
 * @note Zero disables the seam. A positive value is atomically consumed only
 * after successful `SHUT_WR` and grants no channel, deadline, or PID authority.
 */
std::atomic<std::int64_t> g_next_request_shutdown_acceptance_delay_ms{0};
/**
 * @brief Owns the one-shot request-transfer post-acceptance test delay.
 * @note Zero disables the seam. A positive value is atomically consumed only
 * after the same-deadline transfer acceptance observation and grants no
 * channel, deadline, lifecycle, or PID authority.
 */
std::atomic<std::int64_t> g_next_request_transfer_post_acceptance_delay_ms{0};
/**
 * @brief One-shot owned response-channel observation-overflow test seam.
 * @note False is production behavior. The invocation entry consumes this
 * state before fallible preparation, and the retained local value grants no
 * authority outside that one synchronous call.
 */
std::atomic<bool> g_next_response_channel_observation_overflow{false};
/** @brief One-shot post-receive RuntimeStarted acceptance test delay. */
std::atomic<std::int64_t> g_next_runtime_started_acceptance_delay_ms{0};
/** @brief One-shot post-receive Heartbeat acceptance test delay. */
std::atomic<std::int64_t> g_next_runtime_heartbeat_acceptance_delay_ms{0};
/** @brief One-shot post-receive InvocationCompleted acceptance test delay. */
std::atomic<std::int64_t> g_next_invocation_completed_acceptance_delay_ms{0};
/** @brief One-shot post-request exact-child-exit monitor hold for tests. */
std::atomic<bool> g_next_invocation_monitor_exit_hold{false};

/**
 * @brief Records one exact successful blocking child reap.
 * @param pid Positive PID returned by `waitpid`.
 * @return Nothing.
 * @throws Nothing; relaxed atomic observation cannot affect ownership.
 */
void record_reaped_child(pid_t pid) noexcept {
  g_last_reaped_child.store(static_cast<std::int64_t>(pid),
                            std::memory_order_relaxed);
  g_reaped_children.fetch_add(1U, std::memory_order_relaxed);
}

/**
 * @brief Sets close-on-exec on one newly owned descriptor.
 * @param descriptor Valid local descriptor.
 * @throws IsolatedCpuInvocationError when fcntl fails.
 */
void set_close_on_exec(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFD);
  if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU descriptor CLOEXEC failed: ") +
        std::strerror(errno));
  }
}

/**
 * @brief Configures SIGPIPE suppression on one private Unix socket.
 * @param descriptor Valid socket descriptor.
 * @throws IsolatedCpuInvocationError when the platform option fails.
 */
void configure_socket(int descriptor) {
  set_close_on_exec(descriptor);
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  if (::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU socket SIGPIPE setup failed: ") +
        std::strerror(errno));
  }
#endif
}

/**
 * @brief Writes one big-endian uint16 into fixed capability-header storage.
 * @param bytes Non-null header storage.
 * @param offset First byte offset with two bytes available.
 * @param value Exact scalar.
 * @throws Nothing under fixed-header preconditions.
 */
void put_header_u16(std::byte* bytes, std::size_t offset,
                    std::uint16_t value) noexcept {
  bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
  bytes[offset + 1U] = static_cast<std::byte>(value & 0xffU);
}

/**
 * @brief Writes one big-endian uint32 into fixed capability-header storage.
 * @param bytes Non-null header storage.
 * @param offset First byte offset with four bytes available.
 * @param value Exact scalar.
 * @throws Nothing under fixed-header preconditions.
 */
void put_header_u32(std::byte* bytes, std::size_t offset,
                    std::uint32_t value) noexcept {
  for (std::size_t index = 0U; index < 4U; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> ((3U - index) * 8U)) & 0xffU);
  }
}

/**
 * @brief Writes one big-endian uint64 into fixed capability-header storage.
 * @param bytes Non-null header storage.
 * @param offset First byte offset with eight bytes available.
 * @param value Exact scalar.
 * @throws Nothing under fixed-header preconditions.
 */
void put_header_u64(std::byte* bytes, std::size_t offset,
                    std::uint64_t value) noexcept {
  for (std::size_t index = 0U; index < 8U; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU);
  }
}

/**
 * @brief Reads one big-endian uint16 from fixed capability-header storage.
 * @param bytes Non-null header storage.
 * @param offset First byte offset with two bytes available.
 * @return Exact scalar.
 * @throws Nothing under fixed-header preconditions.
 */
std::uint16_t get_header_u16(const std::byte* bytes,
                             std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint16_t>(bytes[offset]) << 8U) |
      std::to_integer<std::uint16_t>(bytes[offset + 1U]));
}

/**
 * @brief Reads one big-endian uint32 from fixed capability-header storage.
 * @param bytes Non-null header storage.
 * @param offset First byte offset with four bytes available.
 * @return Exact scalar.
 * @throws Nothing under fixed-header preconditions.
 */
std::uint32_t get_header_u32(const std::byte* bytes,
                             std::size_t offset) noexcept {
  std::uint32_t value = 0U;
  for (std::size_t index = 0U; index < 4U; ++index) {
    value =
        (value << 8U) | std::to_integer<std::uint32_t>(bytes[offset + index]);
  }
  return value;
}

/**
 * @brief Reads one big-endian uint64 from fixed capability-header storage.
 * @param bytes Non-null header storage.
 * @param offset First byte offset with eight bytes available.
 * @return Exact scalar.
 * @throws Nothing under fixed-header preconditions.
 */
std::uint64_t get_header_u64(const std::byte* bytes,
                             std::size_t offset) noexcept {
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value =
        (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + index]);
  }
  return value;
}

/**
 * @brief Writes the exact FD/content identity header for one capability.
 * @param destination Non-null complete mapping start.
 * @param identity Exact invocation identity supplying the audience tag.
 * @param capability Valid capability declaration including header bytes.
 * @throws IsolatedCpuInvocationError for an impossible declared size.
 */
void write_capability_header(std::byte* destination,
                             const IsolatedCpuInvocationIdentity& identity,
                             const IsolatedCpuCapability& capability) {
  if (destination == nullptr ||
      capability.byte_size <= kCapabilityHeaderBytes) {
    throw IsolatedCpuInvocationError(
        "isolated CPU capability cannot contain its header and payload");
  }
  std::fill(destination, destination + kCapabilityHeaderBytes, std::byte{0});
  put_header_u32(destination, 0U, kCapabilityHeaderMagic);
  put_header_u16(destination, 4U, kCapabilityHeaderVersion);
  destination[6U] = static_cast<std::byte>(capability.access);
  put_header_u64(destination, 8U, capability.capability_id);
  put_header_u64(destination, 16U,
                 capability.byte_size - kCapabilityHeaderBytes);
  std::copy(identity.invocation_id.bytes.begin(),
            identity.invocation_id.bytes.end(), destination + 24U);
}

/**
 * @brief Revalidates one mapped capability header against retained wire state.
 * @param mapping Complete mapped capability bytes.
 * @param identity Exact retained invocation identity.
 * @param capability Exact retained capability declaration.
 * @throws IsolatedCpuProtocolError when any field or audience tag differs.
 */
void validate_capability_header(const UniqueMapping& mapping,
                                const IsolatedCpuInvocationIdentity& identity,
                                const IsolatedCpuCapability& capability) {
  if (mapping.data() == nullptr || mapping.size() != capability.byte_size ||
      mapping.size() <= kCapabilityHeaderBytes) {
    throw IsolatedCpuProtocolError(
        "isolated CPU capability mapping size is inconsistent");
  }
  const std::byte* bytes = mapping.data();
  if (get_header_u32(bytes, 0U) != kCapabilityHeaderMagic ||
      get_header_u16(bytes, 4U) != kCapabilityHeaderVersion ||
      std::to_integer<std::uint8_t>(bytes[6U]) !=
          static_cast<std::uint8_t>(capability.access) ||
      bytes[7U] != std::byte{0} ||
      get_header_u64(bytes, 8U) != capability.capability_id ||
      get_header_u64(bytes, 16U) !=
          capability.byte_size - kCapabilityHeaderBytes ||
      !std::equal(identity.invocation_id.bytes.begin(),
                  identity.invocation_id.bytes.end(), bytes + 24U)) {
    throw IsolatedCpuProtocolError(
        "isolated CPU capability header identity is invalid");
  }
}

/**
 * @brief Validates exact descriptor mode and file size for one capability FD.
 * @param descriptor Valid owned descriptor.
 * @param capability Retained declaration.
 * @throws IsolatedCpuProtocolError for mode/type/size mismatch.
 * @throws IsolatedCpuInvocationError for fcntl/fstat system failure.
 */
void validate_capability_fd(int descriptor,
                            const IsolatedCpuCapability& capability) {
  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU capability fstat failed: ") +
        std::strerror(errno));
  }
  bool shared_memory_type = false;
#if defined(__APPLE__)
  struct pshm_fdinfo shared_memory_info{};
  shared_memory_type =
      ::proc_pidfdinfo(::getpid(), descriptor, PROC_PIDFDPSHMINFO,
                       &shared_memory_info,
                       static_cast<int>(sizeof(shared_memory_info))) ==
      static_cast<int>(sizeof(shared_memory_info));
#elif defined(__linux__)
  shared_memory_type = S_ISREG(status.st_mode);
#endif
  if (!shared_memory_type || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) != capability.byte_size) {
    throw IsolatedCpuProtocolError(
        "isolated CPU capability type or byte size is invalid (shared=" +
        std::to_string(shared_memory_type ? 1 : 0) +
        ", mode=" + std::to_string(status.st_mode) +
        ", actual=" + std::to_string(status.st_size) +
        ", expected=" + std::to_string(capability.byte_size) + ")");
  }
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU capability access query failed: ") +
        std::strerror(errno));
  }
  const int actual = flags & O_ACCMODE;
  const int expected =
      capability.access == IsolatedCpuCapabilityAccess::ReadOnly ? O_RDONLY
                                                                 : O_RDWR;
  if (actual != expected) {
    throw IsolatedCpuProtocolError(
        "isolated CPU capability FD access mode is invalid");
  }
}

/**
 * @brief Creates one unique POSIX shared-memory name candidate.
 * @param sequence Monotonic nonzero process-local suffix.
 * @return Leading-slash name below ordinary POSIX limits.
 * @throws std::bad_alloc when formatting cannot allocate.
 */
std::string shared_memory_name(std::uint64_t sequence) {
  std::array<char, 32U> name{};
  constexpr std::array<char, 5U> prefix{'/', 'p', 's', 'i', '-'};
  std::copy(prefix.begin(), prefix.end(), name.begin());
  char* cursor = name.data() + prefix.size();
  char* const end = name.data() + name.size();
  const auto pid_result =
      std::to_chars(cursor, end, static_cast<std::uint32_t>(::getpid()), 16);
  if (pid_result.ec != std::errc() || pid_result.ptr == end) {
    throw IsolatedCpuInvocationError(
        "isolated CPU shared-memory name formatting failed");
  }
  cursor = pid_result.ptr;
  *cursor = '-';
  const auto sequence_result = std::to_chars(cursor + 1, end, sequence, 16);
  if (sequence_result.ec != std::errc()) {
    throw IsolatedCpuInvocationError(
        "isolated CPU shared-memory name formatting failed");
  }
  return std::string(name.data(), sequence_result.ptr);
}

/**
 * @brief Opens one unique mode-0600 read/write POSIX shared-memory object.
 * @return Descriptor owner and armed name guard.
 * @throws IsolatedCpuInvocationError after system failure or bounded collision
 * exhaustion.
 * @throws std::bad_alloc when candidate-name formatting cannot allocate.
 */
std::pair<UniqueFd, SharedMemoryName> create_shared_memory_object() {
  constexpr std::size_t kMaximumNameAttempts = 64U;
  for (std::size_t attempt = 0U; attempt < kMaximumNameAttempts; ++attempt) {
    const std::uint64_t sequence =
        g_shared_memory_sequence.fetch_add(1U, std::memory_order_relaxed);
    if (sequence == 0U) {
      throw IsolatedCpuInvocationError(
          "isolated CPU shared-memory name sequence exhausted");
    }
    std::string name = shared_memory_name(sequence);
    const int flags = O_CREAT | O_EXCL | O_RDWR;
    const int descriptor = ::shm_open(name.c_str(), flags, 0600);
    if (descriptor >= 0) {
      UniqueFd owner(descriptor);
      set_close_on_exec(owner.get());
      return {std::move(owner), SharedMemoryName(std::move(name))};
    }
    if (errno != EEXIST) {
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU shared-memory creation failed: ") +
          std::strerror(errno));
    }
  }
  throw IsolatedCpuInvocationError(
      "isolated CPU shared-memory name collisions exceeded their bound");
}

/**
 * @brief Creates, fills, unlinks, and maps one directional capability.
 * @param identity Exact invocation audience identity.
 * @param capability Complete capability declaration including header bytes.
 * @param source Input payload bytes for ReadOnly, null for zero-filled output.
 * @param source_size Exact payload bytes excluding the capability header.
 * @return Sole local FD/mapping owner after the name is unlinked. Platform
 * physical padding, when present, remains outside every tensor descriptor.
 * @throws IsolatedCpuInvocationError for size, shm, map, reopen, protection, or
 * unlink failure.
 * @throws std::bad_alloc when local owner state cannot allocate.
 */
MappedCapability prepare_capability(
    const IsolatedCpuInvocationIdentity& identity,
    const IsolatedCpuCapability& capability, const std::byte* source,
    std::size_t source_size) {
  if (capability.byte_size < kCapabilityHeaderBytes + source_size ||
      source_size == 0U ||
      (capability.access == IsolatedCpuCapabilityAccess::ReadOnly &&
       source == nullptr)) {
    throw IsolatedCpuInvocationError(
        "isolated CPU capability preparation size is invalid");
  }
  g_host_capability_materialization_attempts.fetch_add(
      1U, std::memory_order_relaxed);
  auto [read_write, name] = create_shared_memory_object();
  if (capability.byte_size >
          static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
      ::ftruncate(read_write.get(), static_cast<off_t>(capability.byte_size)) !=
          0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU shared-memory sizing failed: ") +
        std::strerror(errno));
  }
  void* mapped =
      ::mmap(nullptr, static_cast<std::size_t>(capability.byte_size),
             PROT_READ | PROT_WRITE, MAP_SHARED, read_write.get(), 0);
  if (mapped == MAP_FAILED) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU shared-memory mapping failed: ") +
        std::strerror(errno));
  }
  UniqueMapping mapping(mapped, static_cast<std::size_t>(capability.byte_size));
  write_capability_header(mapping.data(), identity, capability);
  std::byte* payload = mapping.data() + kCapabilityHeaderBytes;
  std::fill(payload, mapping.data() + mapping.size(), std::byte{0});
  if (source != nullptr) {
    std::memcpy(payload, source, source_size);
  }

  UniqueFd exported;
  if (capability.access == IsolatedCpuCapabilityAccess::ReadOnly) {
    const int flags = O_RDONLY;
    const int read_only = ::shm_open(name.c_str(), flags, 0);
    if (read_only < 0) {
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU read-only shared-memory reopen failed: ") +
          std::strerror(errno));
    }
    exported = UniqueFd(read_only);
    set_close_on_exec(exported.get());
  } else {
    exported = std::move(read_write);
  }
  if (::shm_unlink(name.c_str()) != 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU shared-memory unlink failed: ") +
        std::strerror(errno));
  }
  name.release();
  if (capability.access == IsolatedCpuCapabilityAccess::ReadOnly) {
    read_write.reset();
    if (::mprotect(mapping.data(), mapping.size(), PROT_READ) != 0) {
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU input mapping protection failed: ") +
          std::strerror(errno));
    }
  }
  validate_capability_fd(exported.get(), capability);
  validate_capability_header(mapping, identity, capability);
  return MappedCapability{capability, std::move(exported), std::move(mapping)};
}

/** @brief Monotonic clock used for every supervised lifecycle bound. */
using SupervisorClock = std::chrono::steady_clock;
/** @brief Absolute monotonic deadline used by bounded channel helpers. */
using SupervisorDeadline = SupervisorClock::time_point;
// NOLINTBEGIN(whitespace/indent_namespace)
/** @brief Inclusive configured-duration cap for supervisor lifecycle policy. */
constexpr std::chrono::milliseconds kMaximumSupervisorDuration =
    std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::hours{24});
/** @brief Exact monotonic-clock ticks in one millisecond. */
constexpr auto kSupervisorClockTicksPerMillisecond =
    std::chrono::duration_cast<SupervisorClock::duration>(
        std::chrono::milliseconds{1})
        .count();
static_assert(
    kSupervisorClockTicksPerMillisecond > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            SupervisorClock::duration{kSupervisorClockTicksPerMillisecond}) ==
            std::chrono::milliseconds{1},
    "supported plugin supervision clocks must exactly represent milliseconds");
static_assert(std::numeric_limits<SupervisorClock::duration::rep>::is_integer,
              "supported plugin supervision clocks must use integer ticks");
static_assert(
    kMaximumSupervisorDuration.count() <=
        std::numeric_limits<SupervisorClock::duration::rep>::max() /
            kSupervisorClockTicksPerMillisecond,
    "the plugin supervision duration bound must fit the monotonic clock");
// NOLINTEND

/**
 * @brief Identifies only a checked supervisor time-point range failure.
 * @throws std::bad_alloc when fixed runtime-error storage cannot allocate.
 * @note The private subtype lets the lifecycle owner map arithmetic failures
 * without misclassifying an unrelated `std::overflow_error` from callback,
 * publication, or protocol code.
 */
class SupervisorDeadlineOverflow final : public std::overflow_error {
 public:
  /**
   * @brief Creates the fixed checked-deadline overflow diagnostic.
   * @throws std::bad_alloc when runtime-error storage cannot allocate.
   */
  SupervisorDeadlineOverflow()
      : std::overflow_error(
            "plugin runtime supervisor deadline exceeds monotonic clock "
            "range") {}
};

/**
 * @brief Validates and exactly converts one supervisor lifecycle duration.
 * @param duration Candidate positive millisecond duration.
 * @return Exact monotonic-clock duration for safe deadline arithmetic.
 * @throws std::invalid_argument when `duration` is outside the closed
 * construction domain.
 * @throws std::bad_alloc when constructing a rejection diagnostic exhausts
 * memory.
 * @note Validation precedes `duration_cast`; compile-time scale checks prove
 * every admitted value converts without integer overflow or truncation on
 * supported platforms. This validates duration shape, not a future base sum.
 */
SupervisorClock::duration validate_and_convert_supervisor_duration(
    std::chrono::milliseconds duration) {
  if (duration.count() <= 0 || duration > kMaximumSupervisorDuration) {
    throw std::invalid_argument(
        "plugin runtime supervisor duration must be between 1 and 86400000 "
        "milliseconds");
  }
  const SupervisorClock::duration converted =
      std::chrono::duration_cast<SupervisorClock::duration>(duration);
  if (std::chrono::duration_cast<std::chrono::milliseconds>(converted) !=
      duration) {
    throw std::invalid_argument(
        "plugin runtime supervisor duration is not exactly representable");
  }
  return converted;
}

/**
 * @brief Adds one validated supervisor duration to one captured monotonic base.
 * @param base Exact base captured once by the caller.
 * @param duration Candidate positive supervisor duration within the shared cap.
 * @return Exact absolute monotonic deadline, including an exact-fit maximum.
 * @throws std::invalid_argument when `duration` is outside the construction
 * domain.
 * @throws std::overflow_error when the exact sum exceeds the clock range.
 * @throws std::bad_alloc when constructing a rejection diagnostic exhausts
 * memory.
 * @note The range proof and addition use the same caller-provided base. Every
 * production supervisor deadline derivation delegates here; the helper never
 * wraps, saturates, clamps, or samples a replacement base.
 */
SupervisorDeadline checked_supervisor_deadline(
    SupervisorDeadline base, std::chrono::milliseconds duration) {
  const SupervisorClock::duration increment =
      validate_and_convert_supervisor_duration(duration);
  const SupervisorDeadline latest_base = SupervisorDeadline::max() - increment;
  if (base > latest_base) {
    throw SupervisorDeadlineOverflow();
  }
  return base + increment;
}

/**
 * @brief Internal signal that an absolute channel deadline was reached.
 * @throws std::bad_alloc when fixed runtime-error storage cannot allocate.
 */
class SupervisorDeadlineExpired final : public std::runtime_error {
 public:
  /**
   * @brief Creates the fixed internal timeout diagnostic.
   * @throws std::bad_alloc when runtime-error storage cannot allocate.
   */
  SupervisorDeadlineExpired()
      : std::runtime_error("isolated CPU supervisor deadline expired") {}
};

/**
 * @brief Distinguishes orderly premature framing EOF from a socket syscall
 * failure.
 * @throws std::bad_alloc when fixed diagnostic storage cannot allocate.
 * @note Supervised response handling uses this definitive framing fact for
 * deterministic bad-output classification while true `recvmsg` errors retain
 * the generic channel-failure path.
 */
class PrematureFramedPacketEof final : public IsolatedCpuInvocationError {
 public:
  /**
   * @brief Creates the fixed premature-framing diagnostic.
   * @throws std::bad_alloc when runtime-error storage cannot allocate.
   */
  PrematureFramedPacketEof()
      : IsolatedCpuInvocationError(
            "isolated CPU channel closed before its framed packet completed") {}
};

/**
 * @brief Enables nonblocking I/O on one retained descriptor.
 * @param descriptor Valid descriptor.
 * @return Nothing after preserving existing status flags.
 * @throws IsolatedCpuInvocationError when `fcntl` fails.
 */
void set_nonblocking(int descriptor) {
  const int flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU descriptor nonblocking setup failed: ") +
        std::strerror(errno));
  }
}

/**
 * @brief Converts one future absolute deadline into a ceil-rounded poll wait.
 * @param deadline Absolute monotonic deadline.
 * @return Zero when reached, otherwise a positive value capped at `INT_MAX`.
 * @throws Nothing.
 */
int poll_timeout_until(SupervisorDeadline deadline) noexcept {
  const SupervisorDeadline now = SupervisorClock::now();
  if (now >= deadline) {
    return 0;
  }
  const auto remaining = deadline - now;
  auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (milliseconds < remaining) {
    milliseconds += std::chrono::milliseconds{1};
  }
  const auto capped = std::min<std::int64_t>(milliseconds.count(),
                                             std::numeric_limits<int>::max());
  return static_cast<int>(std::max<std::int64_t>(capped, 1));
}

/**
 * @brief Waits for one descriptor event through an absolute deadline.
 * @param descriptor Valid descriptor.
 * @param events Requested poll event mask.
 * @param deadline Absolute monotonic deadline.
 * @return Observed `revents` mask.
 * @throws SupervisorDeadlineExpired when the bound is reached.
 * @throws IsolatedCpuInvocationError when `poll` fails.
 */
int poll_descriptor_until(int descriptor, int events,
                          SupervisorDeadline deadline) {
  for (;;) {
    if (SupervisorClock::now() >= deadline) {
      throw SupervisorDeadlineExpired();
    }
    struct pollfd descriptor_poll{descriptor, static_cast<std::int16_t>(events),
                                  0};
    const int result =
        ::poll(&descriptor_poll, 1U, poll_timeout_until(deadline));
    if (result > 0) {
      return static_cast<int>(descriptor_poll.revents);
    }
    if (result == 0) {
      throw SupervisorDeadlineExpired();
    }
    if (errno != EINTR) {
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU supervisor poll failed: ") +
          std::strerror(errno));
    }
  }
}

/**
 * @brief Streams one bounded frame and its ordered descriptor capabilities.
 * @param socket Connected blocking Unix stream socket. A would-block result
 * is reported as a channel failure because this helper has no deadline-driven
 * retry path.
 * @param packet Nonempty canonical request or response packet.
 * @param descriptors Ordered descriptors installed with `SCM_RIGHTS`.
 * @return Nothing after the frame and rights are completely sent and the
 * write half is closed as the authoritative frame terminator.
 * @throws IsolatedCpuInvocationError for a channel, zero-progress send, or
 * write-half shutdown failure.
 * @throws IsolatedCpuProtocolError when local packet/descriptor bounds fail.
 * @note Descriptor ownership remains with the caller; `SCM_RIGHTS` accompanies
 * only the first nonempty `sendmsg`, and later sends carry bytes only. This
 * blocking helper serves the non-supervised Host path and the blocking runtime
 * endpoints; the supervised Host request path uses `send_packet_until` on its
 * nonblocking descriptor. Unix `SOCK_STREAM` receive calls do not expose
 * sender-call boundaries. Each endpoint sends exactly one packet, so
 * write-half closure preserves the opposite response direction while making
 * delayed tail detection complete.
 */
void send_packet(int socket, const std::vector<std::byte>& packet,
                 const std::vector<int>& descriptors) {
  if (packet.empty() || packet.size() > kMaximumIsolatedCpuPacketBytes ||
      descriptors.size() > kMaximumIsolatedCpuCapabilities) {
    throw IsolatedCpuProtocolError(
        "isolated CPU outbound packet or FD count exceeds its bound");
  }

  union AncillaryBuffer {
    struct cmsghdr alignment;
    std::array<unsigned char,
               CMSG_SPACE(sizeof(int) * kMaximumIsolatedCpuCapabilities)>
        bytes;
  } control{};
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  std::size_t offset = 0U;
  while (offset != packet.size()) {
    struct iovec data{const_cast<std::byte*>(packet.data() + offset),
                      packet.size() - offset};
    struct msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1U;
    if (offset == 0U && !descriptors.empty()) {
      message.msg_control = control.bytes.data();
      message.msg_controllen = CMSG_SPACE(sizeof(int) * descriptors.size());
      struct cmsghdr* header = CMSG_FIRSTHDR(&message);
      if (header == nullptr) {
        throw IsolatedCpuInvocationError(
            "isolated CPU ancillary header construction failed");
      }
      header->cmsg_level = SOL_SOCKET;
      header->cmsg_type = SCM_RIGHTS;
      header->cmsg_len = CMSG_LEN(sizeof(int) * descriptors.size());
      std::memcpy(CMSG_DATA(header), descriptors.data(),
                  sizeof(int) * descriptors.size());
    }
    ssize_t sent = -1;
    do {
      sent = ::sendmsg(socket, &message, flags);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU framed stream send failed: ") +
          std::strerror(errno));
    }
    if (sent == 0) {
      throw IsolatedCpuInvocationError(
          "isolated CPU framed stream send made no progress");
    }
    offset += static_cast<std::size_t>(sent);
  }
  int shutdown_result = -1;
  do {
    shutdown_result = ::shutdown(socket, SHUT_WR);
  } while (shutdown_result < 0 && errno == EINTR);
  if (shutdown_result != 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU framed stream shutdown failed: ") +
        std::strerror(errno));
  }
}

/**
 * @brief Sends one canonical frame and rights through an absolute deadline.
 * @param socket Connected nonblocking Unix stream socket.
 * @param packet Nonempty bounded canonical request packet.
 * @param descriptors Ordered borrowed capability descriptors.
 * @param deadline Absolute monotonic send/shutdown bound.
 * @return Exact monotonic transfer-acceptance instant sampled for the final
 * same-deadline observation after successful write-half shutdown.
 * @throws SupervisorDeadlineExpired when the bound is reached before complete
 * request-transfer acceptance.
 * @throws IsolatedCpuProtocolError for local packet or descriptor overflow.
 * @throws IsolatedCpuInvocationError for channel or shutdown failure.
 * @note Rights accompany only the first successfully sent byte segment;
 * ownership remains with the caller on every path.
 * A source-private one-shot delay can simulate bounded sender backpressure, a
 * second can simulate descheduling after successful shutdown but before
 * acceptance, and a third can simulate descheduling after acceptance but
 * before caller continuation; none grants channel or lifecycle authority. A
 * failed shutdown remains a channel fact because transfer never reached its
 * success acceptance point. After successful shutdown, the same absolute
 * deadline is observed once and that exact successful observation is returned
 * as the callback/heartbeat budget anchor. The post-acceptance test delay runs
 * only after this timestamp is captured, so caller descheduling cannot arm a
 * fresh window. A late transfer cannot arm invocation or heartbeat budgets,
 * and later cleanup facts cannot replace its deadline cause.
 */
SupervisorDeadline send_packet_until(int socket,
                                     const std::vector<std::byte>& packet,
                                     const std::vector<int>& descriptors,
                                     SupervisorDeadline deadline) {
  if (packet.empty() || packet.size() > kMaximumIsolatedCpuPacketBytes ||
      descriptors.size() > kMaximumIsolatedCpuCapabilities) {
    throw IsolatedCpuProtocolError(
        "isolated CPU outbound packet or FD count exceeds its bound");
  }
  const auto test_delay = std::chrono::milliseconds{
      g_next_supervised_request_send_delay_ms.exchange(
          0, std::memory_order_acq_rel)};
  if (test_delay.count() > 0) {
    std::this_thread::sleep_for(test_delay);
  }

  union AncillaryBuffer {
    struct cmsghdr alignment;
    std::array<unsigned char,
               CMSG_SPACE(sizeof(int) * kMaximumIsolatedCpuCapabilities)>
        bytes;
  } control{};
  int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  std::size_t offset = 0U;
  while (offset != packet.size()) {
    static_cast<void>(poll_descriptor_until(socket, POLLOUT, deadline));
    struct iovec data{const_cast<std::byte*>(packet.data() + offset),
                      packet.size() - offset};
    struct msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1U;
    if (offset == 0U && !descriptors.empty()) {
      message.msg_control = control.bytes.data();
      message.msg_controllen = CMSG_SPACE(sizeof(int) * descriptors.size());
      struct cmsghdr* header = CMSG_FIRSTHDR(&message);
      if (header == nullptr) {
        throw IsolatedCpuInvocationError(
            "isolated CPU ancillary header construction failed");
      }
      header->cmsg_level = SOL_SOCKET;
      header->cmsg_type = SCM_RIGHTS;
      header->cmsg_len = CMSG_LEN(sizeof(int) * descriptors.size());
      std::memcpy(CMSG_DATA(header), descriptors.data(),
                  sizeof(int) * descriptors.size());
    }
    const ssize_t sent = ::sendmsg(socket, &message, flags);
    if (sent < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU supervised frame send failed: ") +
          std::strerror(errno));
    }
    if (sent == 0) {
      throw IsolatedCpuInvocationError(
          "isolated CPU supervised frame send made no progress");
    }
    offset += static_cast<std::size_t>(sent);
  }
  if (SupervisorClock::now() >= deadline) {
    throw SupervisorDeadlineExpired();
  }
  int shutdown_result = -1;
  do {
    shutdown_result = ::shutdown(socket, SHUT_WR);
  } while (shutdown_result < 0 && errno == EINTR);
  if (shutdown_result != 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU supervised frame shutdown failed: ") +
        std::strerror(errno));
  }
  const auto shutdown_acceptance_test_delay = std::chrono::milliseconds{
      g_next_request_shutdown_acceptance_delay_ms.exchange(
          0, std::memory_order_acq_rel)};
  if (shutdown_acceptance_test_delay.count() > 0) {
    std::this_thread::sleep_for(shutdown_acceptance_test_delay);
  }
  const SupervisorDeadline accepted_at = SupervisorClock::now();
  if (accepted_at >= deadline) {
    throw SupervisorDeadlineExpired();
  }
  const auto post_acceptance_test_delay = std::chrono::milliseconds{
      g_next_request_transfer_post_acceptance_delay_ms.exchange(
          0, std::memory_order_acq_rel)};
  if (post_acceptance_test_delay.count() > 0) {
    std::this_thread::sleep_for(post_acceptance_test_delay);
  }
  return accepted_at;
}

/**
 * @brief Assembles one bounded stream frame and owns all installed FDs.
 * @param socket Connected Unix stream socket. It must be blocking when no
 * deadline is supplied and may be nonblocking for the deadline-driven path.
 * @param deadline Optional absolute monotonic receive bound. When present,
 * polling and would-block retries continue only through this deadline.
 * @return Exact framed bytes and RAII-owned ancillary descriptors.
 * @throws PrematureFramedPacketEof for orderly EOF before the declared frame
 * is complete.
 * @throws IsolatedCpuInvocationError for a channel-system failure.
 * @throws SupervisorDeadlineExpired when `deadline` is reached.
 * @throws IsolatedCpuProtocolError for truncation, malformed control data, or
 * excessive packet/descriptor counts.
 * @throws std::bad_alloc only before `recvmsg` installs descriptor rights.
 * @note Storage is fully reserved before receiving. `SCM_RIGHTS` is accepted
 * only while no earlier `recvmsg` payload has been observed; later observed
 * segments carry bytes only. Unix `SOCK_STREAM` can coalesce bytes from an
 * earlier plain send with a later rights-bearing send into the first receive,
 * so endpoint descriptor inventories remain the authoritative rejection gate
 * for rights forbidden in that direction. A declared frame is accepted only
 * when followed by peer write-half EOF; bytes or rights arriving after the
 * exact length are rejected. Every `recvmsg` result is checked for truncation
 * and its complete control records are adopted before a zero byte count is
 * interpreted as EOF, because Darwin can install `SCM_RIGHTS` while returning
 * zero payload bytes. The supervised Host response path supplies a deadline
 * for its nonblocking descriptor. Runtime endpoints and the non-supervised
 * Host omit it and may block until the exact frame is followed by peer
 * write-half EOF.
 */
ReceivedPacket receive_packet(
    int socket, std::optional<SupervisorDeadline> deadline = std::nullopt) {
  ReceivedPacket received;
  received.packet.resize(kMaximumIsolatedCpuPacketBytes + 1U);
  received.descriptors.reserve(kMaximumIsolatedCpuCapabilities);

  int flags = 0;
#ifdef MSG_CMSG_CLOEXEC
  flags |= MSG_CMSG_CLOEXEC;
#endif
  bool malformed_control = false;
  bool excessive_descriptors = false;
  std::size_t received_bytes = 0U;
  std::size_t expected_bytes = 0U;
  for (;;) {
    if (deadline.has_value()) {
      static_cast<void>(poll_descriptor_until(socket, POLLIN, *deadline));
    }
    union AncillaryBuffer {
      struct cmsghdr alignment;
      std::array<unsigned char,
                 CMSG_SPACE(sizeof(int) * kMaximumIsolatedCpuCapabilities)>
          bytes;
    } control{};
    struct iovec data{received.packet.data() + received_bytes,
                      received.packet.size() - received_bytes};
    struct msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1U;
    message.msg_control = control.bytes.data();
    message.msg_controllen = control.bytes.size();
    ssize_t count = -1;
    do {
      count = ::recvmsg(socket, &message, flags);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
      if ((errno == EAGAIN || errno == EWOULDBLOCK) && deadline.has_value()) {
        continue;
      }
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU framed stream receive failed: ") +
          std::strerror(errno));
    }
    const bool truncated = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;

    for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_len < CMSG_LEN(0U)) {
        malformed_control = true;
        continue;
      }
      if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
        malformed_control = true;
        continue;
      }
      if (received_bytes != 0U) {
        malformed_control = true;
      }
      const std::size_t payload_size = header->cmsg_len - CMSG_LEN(0U);
      if (payload_size % sizeof(int) != 0U) {
        malformed_control = true;
        continue;
      }
      const std::size_t descriptor_count = payload_size / sizeof(int);
      const auto* descriptor_bytes =
          reinterpret_cast<const unsigned char*>(CMSG_DATA(header));
      for (std::size_t index = 0U; index < descriptor_count; ++index) {
        int descriptor = -1;
        std::memcpy(&descriptor, descriptor_bytes + index * sizeof(int),
                    sizeof(descriptor));
        UniqueFd owner(descriptor);
        if (received.descriptors.size() >= kMaximumIsolatedCpuCapabilities) {
          excessive_descriptors = true;
          continue;
        }
#ifndef MSG_CMSG_CLOEXEC
        set_close_on_exec(owner.get());
#else
        if ((flags & MSG_CMSG_CLOEXEC) == 0) {
          set_close_on_exec(owner.get());
        }
#endif
        received.descriptors.push_back(std::move(owner));
      }
    }

    if (truncated) {
      throw IsolatedCpuProtocolError(
          "isolated CPU packet or ancillary data was truncated");
    }
    if (count == 0 && expected_bytes != 0U &&
        received_bytes == expected_bytes) {
      break;
    }
    if (count == 0) {
      throw PrematureFramedPacketEof();
    }
    received_bytes += static_cast<std::size_t>(count);
    if (received_bytes >= kIsolatedCpuPacketHeaderBytes &&
        expected_bytes == 0U) {
      std::uint32_t payload_size = 0U;
      for (std::size_t index = 8U; index < 12U; ++index) {
        payload_size = (payload_size << 8U) |
                       std::to_integer<std::uint32_t>(received.packet[index]);
      }
      if (payload_size >
          kMaximumIsolatedCpuPacketBytes - kIsolatedCpuPacketHeaderBytes) {
        throw IsolatedCpuProtocolError(
            "isolated CPU framed packet length exceeds its bound");
      }
      expected_bytes = kIsolatedCpuPacketHeaderBytes + payload_size;
    }
    if (expected_bytes != 0U && received_bytes > expected_bytes) {
      throw IsolatedCpuProtocolError(
          "isolated CPU stream carried bytes beyond one framed packet");
    }
    if (expected_bytes != 0U && received_bytes == expected_bytes) {
      g_exact_frames_received.fetch_add(1U, std::memory_order_relaxed);
    }
  }
  if (malformed_control || excessive_descriptors) {
    throw IsolatedCpuProtocolError(
        "isolated CPU ancillary descriptor record is malformed or arrived "
        "after the first stream segment");
  }
  received.packet.resize(received_bytes);
  return received;
}

/**
 * @brief Maps and header-validates received FDs in declared capability order.
 * @param request Fully decoded request defining exact rights and sizes.
 * @param descriptors Sole owners for all descriptors installed by `recvmsg`.
 * @return Ordered retained capability mappings.
 * @throws IsolatedCpuProtocolError for count, mode, size, or identity mismatch.
 * @throws IsolatedCpuInvocationError for `fstat`, `fcntl`, or `mmap` failure.
 * @throws std::bad_alloc when bounded mapping-owner storage cannot allocate.
 * @note Every input mapping is read-only and every output mapping writable.
 */
std::vector<MappedCapability> map_received_capabilities(
    const IsolatedCpuInvocationRequest& request,
    std::vector<UniqueFd>* descriptors) {
  if (descriptors == nullptr) {
    throw std::invalid_argument(
        "isolated CPU received descriptor owner is null");
  }
  if (descriptors->size() != request.capabilities.size()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU ancillary FD count does not match capabilities");
  }
  std::vector<MappedCapability> mappings;
  mappings.reserve(request.capabilities.size());
  for (std::size_t index = 0U; index < request.capabilities.size(); ++index) {
    const IsolatedCpuCapability& capability = request.capabilities[index];
    UniqueFd descriptor = std::move((*descriptors)[index]);
    validate_capability_fd(descriptor.get(), capability);
    const int protection =
        capability.access == IsolatedCpuCapabilityAccess::ReadOnly
            ? PROT_READ
            : PROT_READ | PROT_WRITE;
    void* address =
        ::mmap(nullptr, static_cast<std::size_t>(capability.byte_size),
               protection, MAP_SHARED, descriptor.get(), 0);
    if (address == MAP_FAILED) {
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU received FD mapping failed: ") +
          std::strerror(errno));
    }
    UniqueMapping mapping(address,
                          static_cast<std::size_t>(capability.byte_size));
    validate_capability_header(mapping, request.identity, capability);
    mappings.push_back(MappedCapability{capability, std::move(descriptor),
                                        std::move(mapping)});
  }
  descriptors->clear();
  return mappings;
}

/**
 * @brief Finds one retained mapped capability by invocation-local selector.
 * @param mappings Capability-id-sorted retained mappings.
 * @param capability_id Nonzero selector from a validated tensor descriptor.
 * @return Borrowed matching capability owner.
 * @throws IsolatedCpuProtocolError when the retained mapping is absent.
 */
const MappedCapability& find_mapped_capability(
    const std::vector<MappedCapability>& mappings,
    std::uint64_t capability_id) {
  const auto found = std::lower_bound(
      mappings.begin(), mappings.end(), capability_id,
      [](const MappedCapability& capability, std::uint64_t id) {
        return capability.capability.capability_id < id;
      });
  if (found == mappings.end() ||
      found->capability.capability_id != capability_id) {
    throw IsolatedCpuProtocolError("isolated CPU mapped capability is absent");
  }
  return *found;
}

/**
 * @brief Reports whether the process-wide SIGCHLD action auto-reaps children.
 * @param action Action returned by `sigaction`.
 * @return True for `SIG_IGN` or `SA_NOCLDWAIT`.
 * @throws Nothing.
 */
bool sigchld_action_auto_reaps(const struct sigaction& action) noexcept {
  if (action.sa_handler == SIG_IGN) {
    return true;
  }
#ifdef SA_NOCLDWAIT
  return (action.sa_flags & SA_NOCLDWAIT) != 0;
#else
  return false;
#endif
}

/**
 * @brief Validates that exact synchronous child reaping remains available.
 * @return Nothing for a waitable SIGCHLD disposition.
 * @throws std::system_error when the action cannot be queried.
 * @throws std::invalid_argument for an auto-reaping disposition.
 */
void validate_sigchld_reaping_configuration() {
  struct sigaction action{};
  if (::sigaction(SIGCHLD, nullptr, &action) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "query isolated CPU SIGCHLD disposition");
  }
  if (sigchld_action_auto_reaps(action)) {
    throw std::invalid_argument(
        "isolated CPU invocation requires waitable SIGCHLD state");
  }
}

/**
 * @brief Revalidates process-global reaping state immediately before fork.
 * @return Nothing while exact wait status remains available.
 * @throws IsolatedCpuInvocationError when the action changed or query fails.
 * @note Issue #102 has no long-lived supervisor; fail-closed synchronous
 * rejection is sufficient before any child authority is minted.
 */
void require_sigchld_reaping_configuration_before_fork() {
  struct sigaction action{};
  if (::sigaction(SIGCHLD, nullptr, &action) != 0 ||
      sigchld_action_auto_reaps(action)) {
    throw IsolatedCpuInvocationError(
        "isolated CPU SIGCHLD reaping configuration changed");
  }
}

/**
 * @brief Writes one setup errno through the close-on-exec pipe and exits.
 * @param descriptor Best available setup-status descriptor.
 * @param error Captured errno, normalized to `EIO` when zero.
 * @return Never returns.
 * @throws Nothing; this post-fork path uses async-signal-safe calls only.
 */
[[noreturn]] void child_setup_failed(int descriptor, int error) noexcept {
  const int normalized = error == 0 ? EIO : error;
  const auto* bytes = reinterpret_cast<const unsigned char*>(&normalized);
  std::size_t offset = 0U;
  while (offset != sizeof(normalized)) {
    const ssize_t written =
        ::write(descriptor, bytes + offset, sizeof(normalized) - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  _exit(127);
}

/**
 * @brief Closes every inherited child descriptor at or above the plan bound.
 * @param plan Platform closure input prepared before fork.
 * @return Zero after complete closure, otherwise a positive setup errno.
 * @throws Nothing; only async-signal-safe operations run after fork.
 * @note The non-supervised plan retains fds 0 through 4; the supervised plan
 * additionally retains fixed supervision fd 5. Darwin scans its kernel
 * descriptor ceiling; Linux uses raw `close_range` and fails closed when
 * unavailable.
 */
int close_child_descriptors(const ChildDescriptorClosurePlan& plan) noexcept {
#if defined(__APPLE__)
  for (int descriptor = plan.first_closed_descriptor;
       descriptor < plan.darwin_exclusive_maximum; ++descriptor) {
    if (::close(descriptor) == 0 || errno == EBADF) {
      continue;
    }
    return errno == 0 ? EIO : errno;
  }
  return 0;
#elif defined(__linux__)
#if defined(SYS_close_range)
  const auto result = ::syscall(
      SYS_close_range, static_cast<unsigned int>(plan.first_closed_descriptor),
      std::numeric_limits<unsigned int>::max(), 0U);
  return result == 0 ? 0 : (errno == 0 ? EIO : errno);
#else
  return ENOSYS;
#endif
#else
  static_cast<void>(plan);
  return ENOSYS;
#endif
}

/**
 * @brief Prepares authoritative platform state for post-fork FD closure.
 * @param first_closed_descriptor First descriptor that the child must close;
 * must be above every fixed endpoint descriptor retained across exec.
 * @return Darwin kernel ceiling or empty Linux plan.
 * @throws std::invalid_argument for a descriptor below the supported bounds.
 * @throws std::system_error when the platform query fails or is unsupported.
 */
ChildDescriptorClosurePlan prepare_child_descriptor_closure(
    int first_closed_descriptor = kFirstPluginRuntimeClosedDescriptor) {
  if (first_closed_descriptor < kPluginRuntimeExecutableDescriptor + 1) {
    throw std::invalid_argument(
        "isolated CPU first closed descriptor is below fixed endpoints");
  }
#if defined(__APPLE__)
  int maximum_descriptor = 0;
  std::size_t result_size = sizeof(maximum_descriptor);
  if (::sysctlbyname("kern.maxfilesperproc", &maximum_descriptor, &result_size,
                     nullptr, 0U) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "query isolated CPU descriptor ceiling");
  }
  if (result_size != sizeof(maximum_descriptor) ||
      maximum_descriptor <= first_closed_descriptor) {
    throw std::system_error(EIO, std::generic_category(),
                            "invalid isolated CPU descriptor ceiling");
  }
  return ChildDescriptorClosurePlan{first_closed_descriptor,
                                    maximum_descriptor};
#elif defined(__linux__)
  return ChildDescriptorClosurePlan{first_closed_descriptor, 0};
#else
  throw std::system_error(ENOSYS, std::generic_category(),
                          "isolated CPU descriptor closure is unsupported");
#endif
}

/**
 * @brief Installs one exact soft and hard child resource limit.
 * @param resource POSIX rlimit selector.
 * @param value Already range-checked finite limit.
 * @return Zero after success, otherwise the captured positive errno.
 * @throws Nothing; this helper is used only in the post-fork child.
 */
int set_child_resource_limit(int resource, std::uint64_t value) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<rlim_t>::max())) {
    return EOVERFLOW;
  }
  const rlim_t native_value = static_cast<rlim_t>(value);
  const struct rlimit limit{native_value, native_value};
  return ::setrlimit(resource, &limit) == 0 ? 0 : (errno == 0 ? EIO : errno);
}

/**
 * @brief Applies all Host-authoritative limits before descriptor-based exec.
 * @param resources Exact resource vector bound into the consumed token.
 * @param policy Validated per-process CPU and address-space policy.
 * @return Zero after every limit is active, otherwise first positive errno.
 * @throws Nothing; this helper is used only in the post-fork child.
 * @note Core dumps are disabled independently of the admitted vector.
 */
int apply_child_resource_limits(
    const PluginResourceVector& resources,
    const PluginInvocationResourcePolicy& policy) noexcept {
  int error = set_child_resource_limit(RLIMIT_AS, policy.address_space_bytes);
  if (error != 0) {
    return error;
  }
  error = set_child_resource_limit(RLIMIT_CPU, policy.cpu_time_seconds);
  if (error != 0) {
    return error;
  }
  error = set_child_resource_limit(RLIMIT_NOFILE, resources.descriptor_count);
  if (error != 0) {
    return error;
  }
  return set_child_resource_limit(RLIMIT_CORE, 0U);
}

/**
 * @brief Execs the sealed Linux runtime through fixed descriptor 6.
 * @param arguments Null-terminated argv whose first value is diagnostic only.
 * @param environment Null-terminated empty environment.
 * @return Only returns -1 after a native exec failure with errno preserved.
 * @throws Nothing; only async-signal-safe native exec is used after fork.
 * @note Linux uses `fexecve` directly and never reopens candidate spelling.
 * Every other platform reports `ENOSYS`; Darwin runtime authorization is
 * rejected before construction can reach any fork or materialization effect.
 */
int exec_authorized_plugin_runtime(char* const arguments[],
                                   char* const environment[]) noexcept {
#if defined(__linux__)
  return ::fexecve(kPluginRuntimeExecutableDescriptor, arguments, environment);
#else
  static_cast<void>(arguments);
  static_cast<void>(environment);
  errno = ENOSYS;
  return -1;
#endif
}

/**
 * @brief Reads the exact close-on-exec child setup-status record.
 * @param descriptor Blocking parent read end of the private pipe.
 * @return Empty for successful exec EOF, otherwise the child setup errno.
 * @throws IsolatedCpuInvocationError for truncation or a pipe-system failure.
 * @note This non-supervised vertical intentionally has no startup deadline.
 */
std::optional<int> read_exec_status(int descriptor) {
  int child_error = 0;
  auto* bytes = reinterpret_cast<unsigned char*>(&child_error);
  std::size_t offset = 0U;
  for (;;) {
    const ssize_t received =
        ::read(descriptor, bytes + offset, sizeof(child_error) - offset);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      if (offset == sizeof(child_error)) {
        return child_error;
      }
      continue;
    }
    if (received == 0) {
      if (offset == 0U) {
        return std::nullopt;
      }
      throw IsolatedCpuInvocationError(
          "isolated CPU exec-status record was truncated");
    }
    if (errno == EINTR) {
      continue;
    }
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU exec-status read failed: ") +
        std::strerror(errno));
  }
}

/**
 * @brief Sole exact PID owner for one non-supervised child.
 * @throws Nothing for moves and destruction.
 * @note Emergency destruction sends SIGKILL and synchronously waits with no
 * bounded deadline; `PluginRuntimeSupervisor` owns bounded escalation and hang
 * classification instead.
 */
class ChildOwner final {
 public:
  /** @brief Creates an empty PID owner. */
  ChildOwner() noexcept = default;

  /**
   * @brief Takes exact wait/signal authority for one positive child PID.
   * @param pid Fresh child PID.
   * @throws Nothing.
   */
  explicit ChildOwner(pid_t pid) noexcept : pid_(pid) {}

  /** @brief Emergency-retires an unreaped exact child. */
  ~ChildOwner() noexcept { terminate_and_reap(); }

  /** @brief Prevents duplicate PID authority. */
  ChildOwner(const ChildOwner&) = delete;
  /** @brief Prevents duplicate PID assignment. */
  ChildOwner& operator=(const ChildOwner&) = delete;

  /**
   * @brief Transfers complete PID authority.
   * @param other Source owner cleared by the move.
   * @throws Nothing.
   */
  ChildOwner(ChildOwner&& other) noexcept
      : pid_(std::exchange(other.pid_, -1)) {}

  /**
   * @brief Replaces authority with one transferred exact PID.
   * @param other Source owner cleared by the move.
   * @return This owner.
   * @throws Nothing; an existing child is emergency-retired first.
   */
  ChildOwner& operator=(ChildOwner&& other) noexcept {
    if (this != &other) {
      terminate_and_reap();
      pid_ = std::exchange(other.pid_, -1);
    }
    return *this;
  }

  /**
   * @brief Waits for exact normal zero child termination and clears authority.
   * @return Nothing after exact reaping.
   * @throws IsolatedCpuInvocationError for wait failure or abnormal exit.
   * @note PID ownership is cleared before the blocking reap attempt. `EINTR`
   * retries the same local PID value without restoring signal/reap authority.
   */
  void wait_for_normal_exit() {
    if (pid_ <= 0) {
      throw IsolatedCpuInvocationError(
          "isolated CPU child has no retained PID authority");
    }
    const pid_t owned = std::exchange(pid_, -1);
    int status = 0;
    pid_t waited = -1;
    do {
      waited = ::waitpid(owned, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != owned) {
      const int error = errno;
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU child wait failed: ") +
          std::strerror(error));
    }
    record_reaped_child(owned);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      throw IsolatedCpuInvocationError(
          "isolated CPU child did not exit normally with status zero");
    }
  }

  /**
   * @brief Sends SIGKILL to and synchronously reaps an unreaped exact child.
   * @return Nothing after best-effort exact cleanup.
   * @throws Nothing; cleanup cannot replace an active exception.
   */
  void terminate_and_reap() noexcept {
    const pid_t owned = std::exchange(pid_, -1);
    if (owned <= 0) {
      return;
    }
    static_cast<void>(::kill(owned, SIGKILL));
    int status = 0;
    pid_t waited = -1;
    do {
      waited = ::waitpid(owned, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == owned) {
      record_reaped_child(owned);
    }
  }

 private:
  /** @brief Exact positive unreaped PID, or invalid sentinel. */
  pid_t pid_ = -1;
};

/**
 * @brief Fresh child plus parent control endpoint.
 * @throws Nothing for moves and destruction.
 */
struct SpawnedChild final {
  /** @brief Sole exact child signal/reap authority. */
  ChildOwner child;
  /** @brief Sole parent endpoint for one request/response exchange. */
  UniqueFd control;
};

/**
 * @brief Forks and descriptor-execs one retained exact runtime object.
 * @param authorized_runtime Signed runtime capability kept live by the caller.
 * @param resources Exact vector already consumed into an active lease.
 * @param policy Validated per-process rlimit policy.
 * @return Sole child and parent socket owners after successful exec.
 * @throws IsolatedCpuInvocationError for socket, pipe, `/dev/null`, fork, or
 * child setup/exec failures.
 * @throws std::system_error from authoritative descriptor-ceiling inspection.
 * @note The child inherits only fds 0 through 4 and executable fd 6; the
 * supervised-only fd 5 gap and every descriptor above fd 6 are closed.
 */
SpawnedChild spawn_runtime(const AuthorizedPluginFile& authorized_runtime,
                           const PluginResourceVector& resources,
                           const PluginInvocationResourcePolicy& policy) {
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU socketpair failed: ") + std::strerror(errno));
  }
  UniqueFd parent_socket(sockets[0]);
  UniqueFd child_socket(sockets[1]);
  configure_socket(parent_socket.get());
  configure_socket(child_socket.get());

  int status_pipe[2] = {-1, -1};
  if (::pipe(status_pipe) != 0) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU exec-status pipe failed: ") +
        std::strerror(errno));
  }
  UniqueFd status_read(status_pipe[0]);
  UniqueFd status_write(status_pipe[1]);
  set_close_on_exec(status_read.get());
  set_close_on_exec(status_write.get());

  int null_flags = O_RDWR;
#ifdef O_CLOEXEC
  null_flags |= O_CLOEXEC;
#endif
  UniqueFd null_device(::open("/dev/null", null_flags));
  if (!null_device.valid()) {
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU /dev/null open failed: ") +
        std::strerror(errno));
  }
  set_close_on_exec(null_device.get());

  const ChildDescriptorClosurePlan closure =
      prepare_child_descriptor_closure(kFirstPluginRuntimeClosedDescriptor);
  const int executable_descriptor = authorized_runtime.native_descriptor();
  if (executable_descriptor < 0) {
    throw PluginTrustError(
        PluginTrustErrorCode::ExactObjectUnsupported,
        "Authorized plugin runtime has no descriptor-based exec capability.");
  }
  const std::string executable = authorized_runtime.original_path().string();
  const char* const executable_pointer = executable.c_str();
  char* const empty_environment[] = {nullptr};

  require_sigchld_reaping_configuration_before_fork();
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw IsolatedCpuInvocationError(std::string("isolated CPU fork failed: ") +
                                     std::strerror(errno));
  }
  if (pid == 0) {
    const int control_copy = ::fcntl(child_socket.get(), F_DUPFD_CLOEXEC,
                                     kFirstPluginRuntimeClosedDescriptor);
    if (control_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int status_copy = ::fcntl(status_write.get(), F_DUPFD_CLOEXEC,
                                    kFirstPluginRuntimeClosedDescriptor);
    if (status_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int null_copy = ::fcntl(null_device.get(), F_DUPFD_CLOEXEC,
                                  kFirstPluginRuntimeClosedDescriptor);
    if (null_copy < 0) {
      child_setup_failed(status_copy, errno);
    }
    const int executable_copy = ::fcntl(executable_descriptor, F_DUPFD_CLOEXEC,
                                        kFirstPluginRuntimeClosedDescriptor);
    if (executable_copy < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::dup2(null_copy, STDIN_FILENO) < 0 ||
        ::dup2(null_copy, STDOUT_FILENO) < 0 ||
        ::dup2(null_copy, STDERR_FILENO) < 0 ||
        ::dup2(control_copy, kIsolatedCpuRuntimeControlDescriptor) < 0 ||
        ::dup2(status_copy, kIsolatedCpuRuntimeSetupDescriptor) < 0 ||
        ::dup2(executable_copy, kPluginRuntimeExecutableDescriptor) < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::fcntl(kIsolatedCpuRuntimeControlDescriptor, F_SETFD, 0) < 0 ||
        ::fcntl(kIsolatedCpuRuntimeSetupDescriptor, F_SETFD, FD_CLOEXEC) < 0 ||
        ::fcntl(kPluginRuntimeExecutableDescriptor, F_SETFD, 0) < 0) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, errno);
    }
    if (::close(kPluginRuntimeSupervisionDescriptor) != 0 && errno != EBADF) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, errno);
    }
    const int close_error = close_child_descriptors(closure);
    if (close_error != 0) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, close_error);
    }
    const int limit_error = apply_child_resource_limits(resources, policy);
    if (limit_error != 0) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, limit_error);
    }
    char* const arguments[] = {const_cast<char*>(executable_pointer), nullptr};
    static_cast<void>(
        exec_authorized_plugin_runtime(arguments, empty_environment));
    child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, errno);
  }

  g_spawned_children.fetch_add(1U, std::memory_order_relaxed);

  child_socket.reset();
  status_write.reset();
  null_device.reset();
  ChildOwner child(pid);
  const std::optional<int> child_error = read_exec_status(status_read.get());
  status_read.reset();
  if (child_error.has_value()) {
    child.terminate_and_reap();
    throw IsolatedCpuInvocationError(
        std::string("isolated CPU setup/exec failed: ") +
        std::strerror(*child_error));
  }
  return SpawnedChild{std::move(child), std::move(parent_socket)};
}

/** @brief Fixed lifecycle frame magic spelling ASCII `PSS1`. */
constexpr std::uint32_t kPluginRuntimeLifecycleMagic = 0x50535331U;
/** @brief Exact private lifecycle protocol version. */
constexpr std::uint16_t kPluginRuntimeLifecycleVersion = 1U;
/** @brief Exact fixed-size lifecycle datagram width. */
constexpr std::size_t kPluginRuntimeLifecycleFrameBytes = 152U;

/**
 * @brief Closed fixed lifecycle frame kinds on the private supervision socket.
 */
enum class PluginRuntimeLifecycleKind : std::uint16_t {
  /** @brief Parent-to-child launch nonce and invocation binding. */
  Hello = 1U,
  /** @brief Child-to-parent authenticated endpoint readiness. */
  RuntimeStarted = 2U,
  /** @brief Child-to-parent callback-liveness observation. */
  Heartbeat = 3U,
  /** @brief Child-to-parent callback/response-materialization completion. */
  InvocationCompleted = 4U,
};

/**
 * @brief Applies one source-private post-receive lifecycle acceptance delay.
 * @param kind Decoded and session-validated child lifecycle event kind.
 * @return Nothing after consuming at most one matching test delay.
 * @throws Nothing.
 * @note Hello is parent-to-child and therefore has no acceptance perturbation.
 * Production behavior is unchanged unless a maintained test explicitly arms a
 * process-local one-shot delay.
 */
void apply_supervised_lifecycle_acceptance_test_delay(
    PluginRuntimeLifecycleKind kind) noexcept {
  std::atomic<std::int64_t>* delay_slot = nullptr;
  switch (kind) {
    case PluginRuntimeLifecycleKind::Hello:
      return;
    case PluginRuntimeLifecycleKind::RuntimeStarted:
      delay_slot = &g_next_runtime_started_acceptance_delay_ms;
      break;
    case PluginRuntimeLifecycleKind::Heartbeat:
      delay_slot = &g_next_runtime_heartbeat_acceptance_delay_ms;
      break;
    case PluginRuntimeLifecycleKind::InvocationCompleted:
      delay_slot = &g_next_invocation_completed_acceptance_delay_ms;
      break;
  }
  const auto delay = std::chrono::milliseconds{
      delay_slot->exchange(0, std::memory_order_acq_rel)};
  if (delay.count() > 0) {
    std::this_thread::sleep_for(delay);
  }
}

/** @brief Unpredictable per-launch session nonce. */
using PluginRuntimeSessionNonce = std::array<std::byte, 16U>;

/**
 * @brief Decoded fixed lifecycle frame without native-layout dependence.
 * @throws Nothing for ordinary value operations.
 */
struct PluginRuntimeLifecycleFrame final {
  /** @brief Closed lifecycle event kind. */
  PluginRuntimeLifecycleKind kind = PluginRuntimeLifecycleKind::Hello;
  /** @brief Strictly increasing child event sequence; hello uses zero. */
  std::uint64_t sequence = 0U;
  /** @brief Exact launch nonce. */
  PluginRuntimeSessionNonce nonce{};
  /** @brief Exact complete retained invocation identity tuple. */
  IsolatedCpuInvocationIdentity identity;
  /** @brief Host-selected positive child heartbeat interval in milliseconds. */
  std::uint64_t heartbeat_interval_milliseconds = 0U;
};

/**
 * @brief Reads an unpredictable 128-bit nonce from the OS random device.
 * @return Complete nonce generated before child ownership begins.
 * @throws IsolatedCpuInvocationError for open, read, or premature EOF failure.
 */
PluginRuntimeSessionNonce generate_plugin_runtime_nonce() {
  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  UniqueFd random_device(::open("/dev/urandom", flags));
  if (!random_device.valid()) {
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime nonce source open failed: ") +
        std::strerror(errno));
  }
  set_close_on_exec(random_device.get());
  PluginRuntimeSessionNonce nonce{};
  std::size_t offset = 0U;
  while (offset != nonce.size()) {
    const ssize_t count = ::read(random_device.get(), nonce.data() + offset,
                                 nonce.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    const int error = count == 0 ? EIO : errno;
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime nonce source read failed: ") +
        std::strerror(error));
  }
  return nonce;
}

/**
 * @brief Encodes one lifecycle frame into exact big-endian canonical bytes.
 * @param frame Complete typed frame.
 * @return Exact fixed-width wire bytes.
 * @throws Nothing.
 */
std::array<std::byte, kPluginRuntimeLifecycleFrameBytes>
encode_plugin_runtime_lifecycle_frame(
    const PluginRuntimeLifecycleFrame& frame) noexcept {
  std::array<std::byte, kPluginRuntimeLifecycleFrameBytes> bytes{};
  put_header_u32(bytes.data(), 0U, kPluginRuntimeLifecycleMagic);
  put_header_u16(bytes.data(), 4U, kPluginRuntimeLifecycleVersion);
  put_header_u16(bytes.data(), 6U, static_cast<std::uint16_t>(frame.kind));
  put_header_u64(bytes.data(), 8U, frame.sequence);
  std::copy(frame.nonce.begin(), frame.nonce.end(), bytes.begin() + 16U);
  std::copy(frame.identity.tenant_id.bytes.begin(),
            frame.identity.tenant_id.bytes.end(), bytes.begin() + 32U);
  std::copy(frame.identity.job_id.bytes.begin(),
            frame.identity.job_id.bytes.end(), bytes.begin() + 48U);
  std::copy(frame.identity.attempt_id.bytes.begin(),
            frame.identity.attempt_id.bytes.end(), bytes.begin() + 64U);
  std::copy(frame.identity.worker_id.bytes.begin(),
            frame.identity.worker_id.bytes.end(), bytes.begin() + 80U);
  put_header_u64(bytes.data(), 96U, frame.identity.worker_lease_generation);
  std::copy(frame.identity.plugin_package_id.bytes.begin(),
            frame.identity.plugin_package_id.bytes.end(), bytes.begin() + 104U);
  put_header_u64(bytes.data(), 120U, frame.identity.plugin_generation);
  std::copy(frame.identity.invocation_id.bytes.begin(),
            frame.identity.invocation_id.bytes.end(), bytes.begin() + 128U);
  put_header_u64(bytes.data(), 144U, frame.heartbeat_interval_milliseconds);
  return bytes;
}

/**
 * @brief Decodes and structurally validates one exact lifecycle datagram.
 * @param bytes Exact fixed-width datagram bytes.
 * @return Typed frame with no authority beyond later session comparison.
 * @throws IsolatedCpuProtocolError for magic, version, or kind mismatch.
 */
PluginRuntimeLifecycleFrame decode_plugin_runtime_lifecycle_frame(
    const std::array<std::byte, kPluginRuntimeLifecycleFrameBytes>& bytes) {
  if (get_header_u32(bytes.data(), 0U) != kPluginRuntimeLifecycleMagic ||
      get_header_u16(bytes.data(), 4U) != kPluginRuntimeLifecycleVersion) {
    throw IsolatedCpuProtocolError(
        "plugin runtime lifecycle magic or version is invalid");
  }
  const auto raw_kind = get_header_u16(bytes.data(), 6U);
  PluginRuntimeLifecycleKind kind;
  switch (raw_kind) {
    case static_cast<std::uint16_t>(PluginRuntimeLifecycleKind::Hello):
      kind = PluginRuntimeLifecycleKind::Hello;
      break;
    case static_cast<std::uint16_t>(PluginRuntimeLifecycleKind::RuntimeStarted):
      kind = PluginRuntimeLifecycleKind::RuntimeStarted;
      break;
    case static_cast<std::uint16_t>(PluginRuntimeLifecycleKind::Heartbeat):
      kind = PluginRuntimeLifecycleKind::Heartbeat;
      break;
    case static_cast<std::uint16_t>(
        PluginRuntimeLifecycleKind::InvocationCompleted):
      kind = PluginRuntimeLifecycleKind::InvocationCompleted;
      break;
    default:
      throw IsolatedCpuProtocolError(
          "plugin runtime lifecycle kind is invalid");
  }
  PluginRuntimeLifecycleFrame frame;
  frame.kind = kind;
  frame.sequence = get_header_u64(bytes.data(), 8U);
  std::copy(bytes.begin() + 16U, bytes.begin() + 32U, frame.nonce.begin());
  std::copy(bytes.begin() + 32U, bytes.begin() + 48U,
            frame.identity.tenant_id.bytes.begin());
  std::copy(bytes.begin() + 48U, bytes.begin() + 64U,
            frame.identity.job_id.bytes.begin());
  std::copy(bytes.begin() + 64U, bytes.begin() + 80U,
            frame.identity.attempt_id.bytes.begin());
  std::copy(bytes.begin() + 80U, bytes.begin() + 96U,
            frame.identity.worker_id.bytes.begin());
  frame.identity.worker_lease_generation = get_header_u64(bytes.data(), 96U);
  std::copy(bytes.begin() + 104U, bytes.begin() + 120U,
            frame.identity.plugin_package_id.bytes.begin());
  frame.identity.plugin_generation = get_header_u64(bytes.data(), 120U);
  std::copy(bytes.begin() + 128U, bytes.begin() + 144U,
            frame.identity.invocation_id.bytes.begin());
  frame.heartbeat_interval_milliseconds = get_header_u64(bytes.data(), 144U);
  return frame;
}

/**
 * @brief Sends one fixed lifecycle datagram through an absolute deadline.
 * @param descriptor Connected nonblocking Unix `SOCK_DGRAM` endpoint.
 * @param frame Complete lifecycle frame.
 * @param deadline Absolute monotonic bound.
 * @return Nothing after one exact datagram send.
 * @throws SupervisorDeadlineExpired when the bound is reached.
 * @throws IsolatedCpuInvocationError for channel failure or short send.
 */
void send_plugin_runtime_lifecycle_frame_until(
    int descriptor, const PluginRuntimeLifecycleFrame& frame,
    SupervisorDeadline deadline) {
  const auto bytes = encode_plugin_runtime_lifecycle_frame(frame);
  int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  for (;;) {
    static_cast<void>(poll_descriptor_until(descriptor, POLLOUT, deadline));
    const ssize_t count = ::send(descriptor, bytes.data(), bytes.size(), flags);
    if (count == static_cast<ssize_t>(bytes.size())) {
      return;
    }
    if (count < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    if (count < 0) {
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime lifecycle send failed: ") +
          std::strerror(errno));
    }
    throw IsolatedCpuInvocationError(
        "plugin runtime lifecycle datagram was sent partially");
  }
}

/**
 * @brief Sends one fixed lifecycle datagram from the runtime endpoint.
 * @param descriptor Connected blocking Unix `SOCK_DGRAM` endpoint.
 * @param frame Complete lifecycle frame.
 * @return Nothing after one exact datagram send.
 * @throws IsolatedCpuInvocationError for channel failure or short send.
 */
void send_plugin_runtime_lifecycle_frame_blocking(
    int descriptor, const PluginRuntimeLifecycleFrame& frame) {
  const auto bytes = encode_plugin_runtime_lifecycle_frame(frame);
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  ssize_t count = -1;
  do {
    count = ::send(descriptor, bytes.data(), bytes.size(), flags);
  } while (count < 0 && errno == EINTR);
  if (count != static_cast<ssize_t>(bytes.size())) {
    const int error = count < 0 ? errno : EIO;
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime lifecycle send failed: ") +
        std::strerror(error));
  }
}

/**
 * @brief Receives one exact lifecycle datagram without blocking.
 * @param descriptor Connected nonblocking Unix `SOCK_DGRAM` endpoint.
 * @return Empty when no datagram is ready, otherwise one decoded frame.
 * @throws IsolatedCpuInvocationError for channel failure or EOF.
 * @throws IsolatedCpuProtocolError for truncation, trailing bytes, or content.
 */
std::optional<PluginRuntimeLifecycleFrame>
receive_plugin_runtime_lifecycle_frame_nonblocking(int descriptor) {
  std::array<std::byte, kPluginRuntimeLifecycleFrameBytes + 1U> storage{};
  const ssize_t count =
      ::recv(descriptor, storage.data(), storage.size(), MSG_DONTWAIT);
  if (count < 0 &&
      (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
    return std::nullopt;
  }
  if (count < 0) {
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime lifecycle receive failed: ") +
        std::strerror(errno));
  }
  if (count == 0) {
    throw IsolatedCpuInvocationError(
        "plugin runtime lifecycle channel closed unexpectedly");
  }
  if (count != static_cast<ssize_t>(kPluginRuntimeLifecycleFrameBytes)) {
    throw IsolatedCpuProtocolError(
        "plugin runtime lifecycle datagram is truncated or trailing");
  }
  std::array<std::byte, kPluginRuntimeLifecycleFrameBytes> frame_bytes{};
  std::copy_n(storage.begin(), frame_bytes.size(), frame_bytes.begin());
  return decode_plugin_runtime_lifecycle_frame(frame_bytes);
}

/**
 * @brief Receives one exact blocking hello inside the fresh runtime.
 * @param descriptor Connected blocking Unix `SOCK_DGRAM` endpoint.
 * @return Decoded lifecycle hello frame.
 * @throws IsolatedCpuInvocationError for channel failure or EOF.
 * @throws IsolatedCpuProtocolError for framing/content or non-hello state.
 */
PluginRuntimeLifecycleFrame receive_plugin_runtime_hello_blocking(
    int descriptor) {
  std::array<std::byte, kPluginRuntimeLifecycleFrameBytes + 1U> storage{};
  ssize_t count = -1;
  do {
    count = ::recv(descriptor, storage.data(), storage.size(), 0);
  } while (count < 0 && errno == EINTR);
  if (count <= 0) {
    const int error = count == 0 ? ECONNRESET : errno;
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime hello receive failed: ") +
        std::strerror(error));
  }
  if (count != static_cast<ssize_t>(kPluginRuntimeLifecycleFrameBytes)) {
    throw IsolatedCpuProtocolError(
        "plugin runtime hello datagram is truncated or trailing");
  }
  std::array<std::byte, kPluginRuntimeLifecycleFrameBytes> frame_bytes{};
  std::copy_n(storage.begin(), frame_bytes.size(), frame_bytes.begin());
  PluginRuntimeLifecycleFrame frame =
      decode_plugin_runtime_lifecycle_frame(frame_bytes);
  if (frame.kind != PluginRuntimeLifecycleKind::Hello || frame.sequence != 0U) {
    throw IsolatedCpuProtocolError(
        "plugin runtime first lifecycle frame is not hello");
  }
  return frame;
}

/**
 * @brief Result of bounded supervisor termination and exact-PID reconciliation.
 * @throws Nothing for ordinary value operations.
 */
struct SupervisedTerminationResult final {
  /** @brief Strongest supervisor signal sent. */
  PluginRuntimeTerminationStage stage = PluginRuntimeTerminationStage::None;
  /** @brief Exact reaped wait status when synchronously available. */
  std::optional<int> wait_status;
  /** @brief True when PID ownership moved to a deferred reaper. */
  bool reap_pending = false;
  /** @brief Deferred exact-reap completion flag when ownership moved. */
  std::shared_ptr<std::atomic<bool>> reap_completion;
};

/**
 * @brief Sole move-only exact PID owner for one supervised fresh runtime.
 * @throws Nothing for construction and moves; explicit waits may throw.
 * @note Destruction never intentionally performs an unbounded caller wait.
 */
class SupervisedChildOwner final {
 public:
  /**
   * @brief Creates an empty PID owner.
   * @throws Nothing.
   */
  SupervisedChildOwner() noexcept = default;

  /**
   * @brief Takes exact wait/signal authority for one positive child PID.
   * @param pid Fresh child PID.
   * @throws Nothing.
   */
  explicit SupervisedChildOwner(pid_t pid) noexcept : pid_(pid) {}

  /**
   * @brief Emergency-signals and transfers any still-owned exact PID.
   * @throws Nothing; thread-creation failure falls back to exact reap.
   * @note Normal callers use bounded explicit retirement before destruction.
   */
  ~SupervisedChildOwner() noexcept { emergency_retire(); }

  /**
   * @brief Prevents duplicate exact-PID authority.
   * @param other Source owner, never consumed because copying is deleted.
   * @throws Nothing because the operation is deleted.
   */
  SupervisedChildOwner(const SupervisedChildOwner&) = delete;
  /**
   * @brief Prevents duplicate exact-PID assignment.
   * @param other Source owner, never consumed because copying is deleted.
   * @return No value because assignment is deleted.
   * @throws Nothing because the operation is deleted.
   */
  SupervisedChildOwner& operator=(const SupervisedChildOwner&) = delete;

  /**
   * @brief Transfers exact-PID authority and any observed status.
   * @param other Source owner cleared by the move.
   * @throws Nothing.
   */
  SupervisedChildOwner(SupervisedChildOwner&& other) noexcept
      : pid_(std::exchange(other.pid_, -1)),
        wait_status_(std::exchange(other.wait_status_, std::nullopt)),
        termination_stage_(std::exchange(
            other.termination_stage_, PluginRuntimeTerminationStage::None)) {}

  /**
   * @brief Replaces authority after emergency-retiring any prior child.
   * @param other Source owner cleared by the move.
   * @return This owner.
   * @throws Nothing.
   */
  SupervisedChildOwner& operator=(SupervisedChildOwner&& other) noexcept {
    if (this != &other) {
      emergency_retire();
      pid_ = std::exchange(other.pid_, -1);
      wait_status_ = std::exchange(other.wait_status_, std::nullopt);
      termination_stage_ = std::exchange(other.termination_stage_,
                                         PluginRuntimeTerminationStage::None);
    }
    return *this;
  }

  /**
   * @brief Reports whether an unreaped exact PID remains owned.
   * @return True only while this object retains positive-PID authority.
   * @throws Nothing.
   */
  bool active() const noexcept { return pid_ > 0; }
  /**
   * @brief Returns the exact owned PID or invalid sentinel.
   * @return Positive owned PID or -1 after transfer/reap.
   * @throws Nothing.
   */
  pid_t pid() const noexcept { return pid_; }
  /**
   * @brief Returns the exact reaped wait status when observed.
   * @return Exact POSIX status, or no value before successful reap.
   * @throws Nothing.
   */
  std::optional<int> wait_status() const noexcept { return wait_status_; }

  /**
   * @brief Returns the strongest supervisor signal successfully sent.
   * @return None, SIGTERM, or SIGKILL for this exact child lifecycle.
   * @throws Nothing.
   */
  PluginRuntimeTerminationStage termination_stage() const noexcept {
    return termination_stage_;
  }

  /**
   * @brief Performs one nonblocking exact-PID reap observation.
   * @return True once exact wait status has been observed.
   * @throws IsolatedCpuInvocationError for an exact `waitpid` failure.
   */
  bool poll_reap() {
    if (!active()) {
      return wait_status_.has_value();
    }
    int status = 0;
    pid_t result = -1;
    do {
      result = ::waitpid(pid_, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
      return false;
    }
    if (result != pid_) {
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime exact waitpid failed: ") +
          std::strerror(errno));
    }
    const pid_t reaped = std::exchange(pid_, -1);
    wait_status_ = status;
    record_reaped_child(reaped);
    return true;
  }

  /**
   * @brief Polls exact status only until one absolute deadline.
   * @param deadline Absolute monotonic wait bound.
   * @return True when reaped, false when the bound expires.
   * @throws IsolatedCpuInvocationError for exact `waitpid` failure.
   */
  bool wait_until(SupervisorDeadline deadline) {
    while (SupervisorClock::now() < deadline) {
      if (poll_reap()) {
        return true;
      }
      const int pause_ms = std::min(poll_timeout_until(deadline), 5);
      int result = -1;
      do {
        result = ::poll(nullptr, 0U, pause_ms);
      } while (result < 0 && errno == EINTR);
      if (result < 0) {
        throw IsolatedCpuInvocationError(
            std::string("plugin runtime reap poll failed: ") +
            std::strerror(errno));
      }
    }
    return poll_reap();
  }

  /**
   * @brief Applies bounded TERM-to-KILL escalation and exact reconciliation.
   * @param options Validated termination and reap timing policy.
   * @return Exact wait/escalation result or deferred-reap ownership record.
   * @throws IsolatedCpuInvocationError for signal or exact wait failure.
   * @throws std::overflow_error when an exact cleanup deadline cannot be
   * represented by the monotonic clock.
   * @throws std::bad_alloc if completion-state allocation fails before PID
   * transfer.
   * @throws std::system_error if the deferred reaper thread cannot be created.
   * @note The final deadline transfers sole PID ownership rather than blocking
   * the caller or falsely reporting successful reap. All potentially throwing
   * completion-state allocation finishes while this object still owns the PID;
   * thread-construction failure restores that same sole authority. A deferred
   * exact-wait failure deliberately leaves completion false so the supervisor
   * remains quarantined instead of claiming an unproved reap.
   */
  SupervisedTerminationResult terminate_and_reap(
      const PluginRuntimeSupervisorOptions& options) {
    SupervisedTerminationResult result;
    result.stage = termination_stage_;
    if (!active()) {
      result.wait_status = wait_status_;
      return result;
    }

    if (::kill(pid_, SIGTERM) == 0) {
      termination_stage_ = PluginRuntimeTerminationStage::Sigterm;
      result.stage = termination_stage_;
    } else if (errno != ESRCH) {
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime SIGTERM failed: ") +
          std::strerror(errno));
    }
    if (wait_until(checked_supervisor_deadline(SupervisorClock::now(),
                                               options.termination_grace))) {
      result.wait_status = wait_status_;
      return result;
    }

    if (::kill(pid_, SIGKILL) == 0) {
      termination_stage_ = PluginRuntimeTerminationStage::Sigkill;
      result.stage = termination_stage_;
    } else if (errno != ESRCH) {
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime SIGKILL failed: ") +
          std::strerror(errno));
    }
    if (wait_until(checked_supervisor_deadline(SupervisorClock::now(),
                                               options.kill_reap_timeout))) {
      result.wait_status = wait_status_;
      return result;
    }

    const auto completion = std::make_shared<std::atomic<bool>>(false);
    const pid_t transferred = std::exchange(pid_, -1);
    result.reap_pending = true;
    result.reap_completion = completion;
    try {
      std::thread([transferred, completion]() noexcept {
        int status = 0;
        pid_t waited = -1;
        do {
          waited = ::waitpid(transferred, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited == transferred) {
          record_reaped_child(transferred);
          completion->store(true, std::memory_order_release);
        }
      }).detach();
    } catch (...) {
      pid_ = transferred;
      throw;
    }
    return result;
  }

 private:
  /**
   * @brief Best-effort emergency transfer used only by destructor fallback.
   * @return Nothing after clearing this owner.
   * @throws Nothing; a thread-construction failure falls back to exact reap.
   */
  void emergency_retire() noexcept {
    const pid_t transferred = std::exchange(pid_, -1);
    if (transferred <= 0) {
      return;
    }
    static_cast<void>(::kill(transferred, SIGKILL));
    try {
      std::thread([transferred]() noexcept {
        int status = 0;
        pid_t waited = -1;
        do {
          waited = ::waitpid(transferred, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited == transferred) {
          record_reaped_child(transferred);
        }
      }).detach();
    } catch (...) {
      int status = 0;
      pid_t waited = -1;
      do {
        waited = ::waitpid(transferred, &status, 0);
      } while (waited < 0 && errno == EINTR);
      if (waited == transferred) {
        record_reaped_child(transferred);
      }
    }
  }

  /** @brief Exact positive unreaped PID or invalid sentinel. */
  pid_t pid_ = -1;
  /** @brief Exact status after one successful reap. */
  std::optional<int> wait_status_;
  /** @brief Strongest supervisor signal successfully sent to the exact PID. */
  PluginRuntimeTerminationStage termination_stage_ =
      PluginRuntimeTerminationStage::None;
};

/**
 * @brief Fresh supervised child plus all parent lifecycle endpoints.
 * @throws Nothing for moves and destruction.
 */
struct SupervisedSpawnedChild final {
  /** @brief Sole exact child signal/reap authority. */
  SupervisedChildOwner child;
  /** @brief Sole parent request/response stream endpoint. */
  UniqueFd control;
  /** @brief Sole parent lifecycle Unix datagram endpoint. */
  UniqueFd supervision;
  /** @brief Sole parent exec-status pipe read endpoint. */
  UniqueFd setup_status;
};

/**
 * @brief Forks one supervised runtime with fixed fds 3 through 6.
 * @param authorized_runtime Signed runtime capability kept live by the caller.
 * @param resources Exact vector already consumed into an active lease.
 * @param policy Validated per-process rlimit policy.
 * @return Exact child and nonblocking parent endpoints before exec completes.
 * @throws IsolatedCpuInvocationError for socket, pipe, device, fork, or setup
 * preparation failure.
 * @throws std::system_error for descriptor-ceiling inspection failure.
 * @note The child receives an empty environment and no descriptor above fd 6.
 */
SupervisedSpawnedChild spawn_supervised_runtime(
    const AuthorizedPluginFile& authorized_runtime,
    const PluginResourceVector& resources,
    const PluginInvocationResourcePolicy& policy) {
  int control_sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, control_sockets) != 0) {
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime control socketpair failed: ") +
        std::strerror(errno));
  }
  UniqueFd parent_control(control_sockets[0]);
  UniqueFd child_control(control_sockets[1]);
  configure_socket(parent_control.get());
  configure_socket(child_control.get());

  int supervision_sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, supervision_sockets) != 0) {
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime supervision socketpair failed: ") +
        std::strerror(errno));
  }
  UniqueFd parent_supervision(supervision_sockets[0]);
  UniqueFd child_supervision(supervision_sockets[1]);
  configure_socket(parent_supervision.get());
  configure_socket(child_supervision.get());

  int status_pipe[2] = {-1, -1};
  if (::pipe(status_pipe) != 0) {
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime exec-status pipe failed: ") +
        std::strerror(errno));
  }
  UniqueFd status_read(status_pipe[0]);
  UniqueFd status_write(status_pipe[1]);
  set_close_on_exec(status_read.get());
  set_close_on_exec(status_write.get());

  int null_flags = O_RDWR;
#ifdef O_CLOEXEC
  null_flags |= O_CLOEXEC;
#endif
  UniqueFd null_device(::open("/dev/null", null_flags));
  if (!null_device.valid()) {
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime /dev/null open failed: ") +
        std::strerror(errno));
  }
  set_close_on_exec(null_device.get());

  const ChildDescriptorClosurePlan closure =
      prepare_child_descriptor_closure(kFirstPluginRuntimeClosedDescriptor);
  const int executable_descriptor = authorized_runtime.native_descriptor();
  if (executable_descriptor < 0) {
    throw PluginTrustError(
        PluginTrustErrorCode::ExactObjectUnsupported,
        "Authorized plugin runtime has no descriptor-based exec capability.");
  }
  const std::string executable = authorized_runtime.original_path().string();
  const char* const executable_pointer = executable.c_str();
  char* const empty_environment[] = {nullptr};

  require_sigchld_reaping_configuration_before_fork();
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw IsolatedCpuInvocationError(
        std::string("plugin runtime fork failed: ") + std::strerror(errno));
  }
  if (pid == 0) {
    const int control_copy = ::fcntl(child_control.get(), F_DUPFD_CLOEXEC,
                                     kFirstPluginRuntimeClosedDescriptor);
    if (control_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int status_copy = ::fcntl(status_write.get(), F_DUPFD_CLOEXEC,
                                    kFirstPluginRuntimeClosedDescriptor);
    if (status_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int supervision_copy =
        ::fcntl(child_supervision.get(), F_DUPFD_CLOEXEC,
                kFirstPluginRuntimeClosedDescriptor);
    if (supervision_copy < 0) {
      child_setup_failed(status_copy, errno);
    }
    const int null_copy = ::fcntl(null_device.get(), F_DUPFD_CLOEXEC,
                                  kFirstPluginRuntimeClosedDescriptor);
    if (null_copy < 0) {
      child_setup_failed(status_copy, errno);
    }
    const int executable_copy = ::fcntl(executable_descriptor, F_DUPFD_CLOEXEC,
                                        kFirstPluginRuntimeClosedDescriptor);
    if (executable_copy < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::dup2(null_copy, STDIN_FILENO) < 0 ||
        ::dup2(null_copy, STDOUT_FILENO) < 0 ||
        ::dup2(null_copy, STDERR_FILENO) < 0 ||
        ::dup2(control_copy, kIsolatedCpuRuntimeControlDescriptor) < 0 ||
        ::dup2(status_copy, kIsolatedCpuRuntimeSetupDescriptor) < 0 ||
        ::dup2(supervision_copy, kPluginRuntimeSupervisionDescriptor) < 0 ||
        ::dup2(executable_copy, kPluginRuntimeExecutableDescriptor) < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::fcntl(kIsolatedCpuRuntimeControlDescriptor, F_SETFD, 0) < 0 ||
        ::fcntl(kIsolatedCpuRuntimeSetupDescriptor, F_SETFD, FD_CLOEXEC) < 0 ||
        ::fcntl(kPluginRuntimeSupervisionDescriptor, F_SETFD, 0) < 0 ||
        ::fcntl(kPluginRuntimeExecutableDescriptor, F_SETFD, 0) < 0) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, errno);
    }
    const int close_error = close_child_descriptors(closure);
    if (close_error != 0) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, close_error);
    }
    const int limit_error = apply_child_resource_limits(resources, policy);
    if (limit_error != 0) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, limit_error);
    }
    char* const arguments[] = {const_cast<char*>(executable_pointer), nullptr};
    static_cast<void>(
        exec_authorized_plugin_runtime(arguments, empty_environment));
    child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, errno);
  }

  g_spawned_children.fetch_add(1U, std::memory_order_relaxed);
  SupervisedChildOwner child(pid);
  child_control.reset();
  child_supervision.reset();
  status_write.reset();
  null_device.reset();
  set_nonblocking(parent_control.get());
  set_nonblocking(parent_supervision.get());
  set_nonblocking(status_read.get());
  return SupervisedSpawnedChild{std::move(child), std::move(parent_control),
                                std::move(parent_supervision),
                                std::move(status_read)};
}

/**
 * @brief Serializes runtime lifecycle events and emits periodic heartbeats.
 * @throws std::invalid_argument for an invalid hello heartbeat interval.
 * @throws std::system_error when the heartbeat thread cannot be created.
 * @note The callback thread and heartbeat thread share only this private
 * fixed-frame sender; neither can extend the Host invocation deadline.
 */
class RuntimeHeartbeatEmitter final {
 public:
  /**
   * @brief Retains one endpoint, authenticated session, and interval.
   * @param descriptor Borrowed connected supervision descriptor.
   * @param hello Exact Host hello containing nonce and invocation id.
   * The heartbeat interval is decoded from the authenticated Host hello.
   * @throws std::invalid_argument for invalid input.
   */
  RuntimeHeartbeatEmitter(int descriptor,
                          const PluginRuntimeLifecycleFrame& hello)
      : descriptor_(descriptor), session_(hello) {
    const auto maximum_interval = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::hours{24})
            .count());
    if (descriptor_ < 0 || hello.heartbeat_interval_milliseconds == 0U ||
        hello.heartbeat_interval_milliseconds > maximum_interval) {
      throw std::invalid_argument(
          "plugin runtime heartbeat endpoint or interval is invalid");
    }
    interval_ = std::chrono::milliseconds{
        static_cast<std::int64_t>(hello.heartbeat_interval_milliseconds)};
  }

  /**
   * @brief Stops and joins a running heartbeat thread.
   * @throws Nothing.
   * @note Destruction returns only after periodic emission has ceased.
   */
  ~RuntimeHeartbeatEmitter() noexcept { stop(); }

  /**
   * @brief Prevents duplicate descriptor/session use.
   * @param other Source emitter, never consumed because copying is deleted.
   * @throws Nothing because the operation is deleted.
   */
  RuntimeHeartbeatEmitter(const RuntimeHeartbeatEmitter&) = delete;
  /**
   * @brief Prevents duplicate descriptor/session assignment.
   * @param other Source emitter, never consumed because copying is deleted.
   * @return No value because assignment is deleted.
   * @throws Nothing because the operation is deleted.
   */
  RuntimeHeartbeatEmitter& operator=(const RuntimeHeartbeatEmitter&) = delete;

  /**
   * @brief Sends the first authenticated started event.
   * @param corrupt_nonce True only for the deterministic fail-closed fixture.
   * @return Nothing after exact send.
   * @throws IsolatedCpuInvocationError for channel failure.
   */
  void send_started(bool corrupt_nonce) {
    send_event(PluginRuntimeLifecycleKind::RuntimeStarted, corrupt_nonce);
  }

  /**
   * @brief Starts periodic authenticated heartbeat emission.
   * @return Nothing after the background thread owns its loop.
   * @throws std::system_error when thread creation fails.
   */
  void start() {
    heartbeat_thread_ = std::thread([this]() noexcept { heartbeat_loop(); });
  }

  /**
   * @brief Stops periodic emission and joins the thread once.
   * @return Nothing after no heartbeat thread remains.
   * @throws Nothing.
   */
  void stop() noexcept {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stopping_ = true;
    }
    state_changed_.notify_all();
    if (heartbeat_thread_.joinable()) {
      heartbeat_thread_.join();
    }
  }

  /**
   * @brief Sends callback-completion after heartbeat emission has stopped.
   * @return Nothing after the exact next-sequence event is sent.
   * @throws IsolatedCpuInvocationError when heartbeat or completion send
   * failed.
   */
  void send_completed() {
    if (heartbeat_failed_.load(std::memory_order_acquire)) {
      throw IsolatedCpuInvocationError(
          "plugin runtime heartbeat channel failed before completion");
    }
    send_event(PluginRuntimeLifecycleKind::InvocationCompleted, false);
  }

 private:
  /**
   * @brief Sends one next-sequence event under the sole sender mutex.
   * @param kind Closed child-to-parent event kind.
   * @param corrupt_nonce True only for deterministic startup rejection.
   * @return Nothing after exact send.
   * @throws IsolatedCpuInvocationError for channel failure.
   */
  void send_event(PluginRuntimeLifecycleKind kind, bool corrupt_nonce) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      throw IsolatedCpuProtocolError(
          "plugin runtime lifecycle sequence is exhausted");
    }
    PluginRuntimeLifecycleFrame event = session_;
    event.kind = kind;
    event.sequence = next_sequence_++;
    if (corrupt_nonce) {
      event.nonce[0] ^= std::byte{1U};
    }
    send_plugin_runtime_lifecycle_frame_blocking(descriptor_, event);
  }

  /**
   * @brief Waits one interval at a time and contains heartbeat send failures.
   * @return Nothing after stop request or channel failure.
   * @throws Nothing.
   */
  void heartbeat_loop() noexcept {
    std::unique_lock<std::mutex> lock(state_mutex_);
    while (!stopping_) {
      if (state_changed_.wait_for(lock, interval_,
                                  [this]() { return stopping_; })) {
        return;
      }
      lock.unlock();
      try {
        send_event(PluginRuntimeLifecycleKind::Heartbeat, false);
      } catch (...) {
        heartbeat_failed_.store(true, std::memory_order_release);
        return;
      }
      lock.lock();
    }
  }

  /** @brief Borrowed connected endpoint valid for this process lifetime. */
  int descriptor_ = -1;
  /** @brief Exact hello-derived nonce and invocation binding. */
  PluginRuntimeLifecycleFrame session_;
  /** @brief Positive periodic emission interval. */
  std::chrono::milliseconds interval_{0};
  /** @brief Serializes sequence allocation and fixed datagram sends. */
  std::mutex send_mutex_;
  /** @brief Protects stop state and heartbeat wait. */
  std::mutex state_mutex_;
  /** @brief Wakes the background heartbeat wait during endpoint completion. */
  std::condition_variable state_changed_;
  /** @brief Next strictly increasing child event sequence. */
  std::uint64_t next_sequence_ = 1U;
  /** @brief True after endpoint completion requests thread retirement. */
  bool stopping_ = false;
  /** @brief True after the background sender observes a channel failure. */
  std::atomic<bool> heartbeat_failed_{false};
  /** @brief Sole joinable periodic sender thread. */
  std::thread heartbeat_thread_;
};

/**
 * @brief Converts one public element semantic into the closed wire value.
 * @param semantics Valid public DenseTensor semantic.
 * @return Exact wire semantic.
 * @throws IsolatedCpuProtocolError for an unknown future public value.
 */
IsolatedCpuElementSemantics to_wire_element_semantics(
    ElementSemantics semantics) {
  switch (semantics) {
    case ElementSemantics::UnsignedInteger:
      return IsolatedCpuElementSemantics::UnsignedInteger;
    case ElementSemantics::SignedInteger:
      return IsolatedCpuElementSemantics::SignedInteger;
    case ElementSemantics::FloatingPoint:
      return IsolatedCpuElementSemantics::FloatingPoint;
  }
  throw IsolatedCpuProtocolError(
      "isolated CPU DenseTensor element semantics is unsupported");
}

/**
 * @brief Projects one optional public image facet into complete wire metadata.
 * @param facet Optional facet from a public Value or output plan.
 * @return Exact optional wire facet.
 * @throws IsolatedCpuProtocolError when an axis exceeds uint32 representation.
 * @throws std::bad_alloc when bounded nested metadata copying fails.
 * @note Every signed window, stable channel/group identity, diagnostic name,
 * sample-domain fact, and color fact is retained without a process pointer.
 */
std::optional<IsolatedCpuImageFacet> to_wire_image_facet(
    const std::optional<ImageFacet>& facet) {
  if (!facet.has_value()) {
    return std::nullopt;
  }
  const auto to_axis = [](std::size_t axis) {
    if (axis > std::numeric_limits<std::uint32_t>::max()) {
      throw IsolatedCpuProtocolError(
          "isolated CPU image facet axis exceeds wire representation");
    }
    return static_cast<std::uint32_t>(axis);
  };
  IsolatedCpuImageFacet wire;
  wire.x_axis = to_axis(facet->x_axis);
  wire.y_axis = to_axis(facet->y_axis);
  if (facet->channel_axis.has_value()) {
    wire.channel_axis = to_axis(*facet->channel_axis);
  }
  wire.data_window = facet->data_window;
  wire.display_window = facet->display_window;
  wire.channel_schema = facet->channel_schema;
  wire.sample_domain = facet->sample_domain;
  wire.color = facet->color;
  return wire;
}

/**
 * @brief Converts one public whole-byte DenseTensor descriptor into wire facts.
 * @param descriptor Public logical descriptor.
 * @param image_facet Optional public complete ordinary-image interpretation.
 * @param layout Public signed whole-byte layout.
 * @return Structural wire descriptor without capability/phase/binding fields.
 * @throws IsolatedCpuProtocolError for malformed image metadata, unsupported
 * quantization/encoding, or local integer representation.
 * @throws std::invalid_argument from public scalar-width validation.
 * @throws std::bad_alloc when bounded metadata-validation state or wire vectors
 *         cannot allocate.
 */
IsolatedCpuTensorDescriptor to_wire_tensor_descriptor(
    const DenseTensorDescriptor& descriptor,
    const std::optional<ImageFacet>& image_facet, const StridedLayout& layout) {
  try {
    validate_dense_tensor_image_metadata(descriptor, image_facet);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw IsolatedCpuProtocolError(error.what());
  }
  if (descriptor.quantization.has_value() ||
      descriptor.storage_encoding.kind != StorageEncodingKind::NativeScalar) {
    throw IsolatedCpuProtocolError(
        "isolated CPU invocation accepts only unquantized native scalars");
  }
  static_cast<void>(dense_tensor_element_bytes(descriptor));
  if (descriptor.shape.size() > kMaximumIsolatedCpuTensorRank ||
      layout.byte_strides.size() != descriptor.shape.size()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU DenseTensor rank or stride count is invalid");
  }

  IsolatedCpuTensorDescriptor wire;
  wire.element_semantics =
      to_wire_element_semantics(descriptor.element_semantics);
  wire.storage_encoding = IsolatedCpuStorageEncoding::NativeScalar;
  wire.bit_width = descriptor.storage_encoding.bit_width;
  wire.extents.reserve(descriptor.shape.size());
  wire.byte_strides.reserve(layout.byte_strides.size());
  for (const std::size_t extent : descriptor.shape) {
    wire.extents.push_back(static_cast<std::uint64_t>(extent));
  }
  for (const std::ptrdiff_t stride : layout.byte_strides) {
    if constexpr (sizeof(std::ptrdiff_t) > sizeof(std::int64_t)) {
      if (stride < std::numeric_limits<std::int64_t>::min() ||
          stride > std::numeric_limits<std::int64_t>::max()) {
        throw IsolatedCpuProtocolError(
            "isolated CPU stride exceeds wire representation");
      }
    }
    wire.byte_strides.push_back(static_cast<std::int64_t>(stride));
  }
  wire.byte_offset = static_cast<std::uint64_t>(layout.byte_offset);
  wire.image_facet = to_wire_image_facet(image_facet);
  return wire;
}

/**
 * @brief Computes a complete platform-valid capability length with checks.
 * @param payload_bytes Positive tensor storage bytes.
 * @return Header plus payload bytes, rounded to the Darwin VM page size when
 * POSIX shared memory reports page-granular physical length.
 * @throws IsolatedCpuProtocolError for zero or uint64 overflow.
 */
std::uint64_t complete_capability_size(std::size_t payload_bytes) {
  if (payload_bytes == 0U ||
      payload_bytes >
          std::numeric_limits<std::uint64_t>::max() - kCapabilityHeaderBytes) {
    throw IsolatedCpuProtocolError(
        "isolated CPU capability payload size is invalid");
  }
  const std::uint64_t exact =
      static_cast<std::uint64_t>(payload_bytes) + kCapabilityHeaderBytes;
#if defined(__APPLE__)
  const std::uint64_t page_size = static_cast<std::uint64_t>(::getpagesize());
  if (page_size == 0U ||
      exact > std::numeric_limits<std::uint64_t>::max() - (page_size - 1U)) {
    throw IsolatedCpuProtocolError(
        "isolated CPU Darwin shared-memory size cannot be rounded");
  }
  return ((exact + page_size - 1U) / page_size) * page_size;
#else
  return exact;
#endif
}

/**
 * @brief Side-effect-free Host plan retained before capability materialization.
 * @throws Nothing for moves and destruction.
 * @note Every vector is bounded by protocol-v2 endpoint limits.
 */
struct HostInvocationPreflight final {
  /** @brief Fully validated canonical request before shared-memory creation. */
  IsolatedCpuInvocationRequest request;
  /** @brief Canonical packet proven to fit the complete wire byte bound. */
  std::vector<std::byte> request_packet;
  /** @brief Ready Host-visible input ranges retained through byte copying. */
  std::vector<ReadLease> input_leases;
};

/**
 * @brief Converts the wire comparison package key into the signed trust type.
 * @param identity Completely validated invocation identity.
 * @return Exact package bytes and generation for trust comparison.
 * @throws Nothing.
 */
PluginPackageIdentity invocation_package_identity(
    const IsolatedCpuInvocationIdentity& identity) noexcept {
  PluginPackageIdentity package;
  for (std::size_t index = 0U; index < package.package_id.size(); ++index) {
    package.package_id[index] =
        std::to_integer<std::uint8_t>(identity.plugin_package_id.bytes[index]);
  }
  package.generation = identity.plugin_generation;
  return package;
}

/**
 * @brief Adds one uint64 in canonical big-endian form to an EVP digest.
 * @param context Initialized SHA-256 digest context.
 * @param value Exact scalar.
 * @return Nothing after all eight bytes are incorporated.
 * @throws std::runtime_error if OpenSSL rejects the update.
 */
void digest_identity_u64(EVP_MD_CTX* context, std::uint64_t value) {
  std::array<unsigned char, 8U> bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<unsigned char>(
        (value >> ((bytes.size() - 1U - index) * 8U)) & 0xffU);
  }
  if (EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1) {
    throw std::runtime_error(
        "Cannot hash plugin invocation identity generation.");
  }
}

/**
 * @brief Derives the replay key from every complete invocation identity field.
 * @param identity Validated tenant, Job, attempt, worker lease, package, and
 * invocation tuple.
 * @return Domain-separated SHA-256 digest used only by the Host ledger.
 * @throws std::bad_alloc if the OpenSSL context cannot be allocated.
 * @throws std::runtime_error if OpenSSL rejects digest operations.
 * @note The domain includes a terminating zero byte so later textual suffixes
 * cannot collide with this versioned identity namespace.
 */
PluginInvocationIdentityDigest plugin_invocation_identity_digest(
    const IsolatedCpuInvocationIdentity& identity) {
  constexpr std::array<unsigned char, 42U> kDomain{
      'p', 'h', 'o', 't', 'o', 's', 'p', 'i', 'd', 'e', 'r', '-', 'p', 'l',
      'u', 'g', 'i', 'n', '-', 'i', 'n', 'v', 'o', 'c', 'a', 't', 'i', 'o',
      'n', '-', 'r', 'e', 's', 'o', 'u', 'r', 'c', 'e', '-', 'v', '1', '\0'};
  EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
  if (raw_context == nullptr) {
    throw std::bad_alloc();
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      raw_context, &EVP_MD_CTX_free);
  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), kDomain.data(), kDomain.size()) != 1) {
    throw std::runtime_error(
        "Cannot initialize plugin invocation identity hash.");
  }
  const std::array<const IsolatedCpuOpaqueId*, 6U> identifiers{
      &identity.tenant_id,         &identity.job_id,
      &identity.attempt_id,        &identity.worker_id,
      &identity.plugin_package_id, &identity.invocation_id,
  };
  for (const IsolatedCpuOpaqueId* identifier : identifiers) {
    if (EVP_DigestUpdate(context.get(), identifier->bytes.data(),
                         identifier->bytes.size()) != 1) {
      throw std::runtime_error("Cannot hash plugin invocation identity bytes.");
    }
  }
  digest_identity_u64(context.get(), identity.worker_lease_generation);
  digest_identity_u64(context.get(), identity.plugin_generation);
  PluginInvocationIdentityDigest digest{};
  unsigned int size = 0U;
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 ||
      size != digest.size()) {
    throw std::runtime_error(
        "Cannot finalize plugin invocation identity hash.");
  }
  return digest;
}

/**
 * @brief Derives the complete Host-authoritative resource vector from
 * preflight.
 * @param preflight Fully validated request without invocation OS effects.
 * @param policy Validated composition resource policy.
 * @return Exact aggregate and per-process resource demand.
 * @throws PluginResourceAdmissionError if descriptor addition overflows.
 */
PluginResourceVector plugin_resource_demand(
    const HostInvocationPreflight& preflight,
    const PluginInvocationResourcePolicy& policy) {
  const std::uint64_t capabilities =
      preflight.request.resources.descriptor_count;
  if (capabilities >
      std::numeric_limits<std::uint64_t>::max() - policy.descriptor_overhead) {
    throw PluginResourceAdmissionError(
        PluginResourceAdmissionErrorCode::QuotaExceeded,
        "Plugin invocation descriptor demand overflows uint64.");
  }
  return PluginResourceVector{
      1U,
      preflight.request.resources.cpu_slots,
      policy.address_space_bytes,
      preflight.request.resources.shared_memory_bytes,
      capabilities + policy.descriptor_overhead,
  };
}

/**
 * @brief Complete Host-side request and materialized shared-memory owner set.
 * @throws Nothing for moves and destruction.
 */
struct PreparedHostInvocation final {
  /** @brief Validated canonical request retained for response comparison. */
  IsolatedCpuInvocationRequest request;
  /** @brief Canonical request bytes encoded during side-effect-free preflight.
   */
  std::vector<std::byte> request_packet;
  /** @brief Ordered invocation-local FDs and mappings. */
  std::vector<MappedCapability> capabilities;
};

/**
 * @brief Adds one capability size to a checked Host preflight aggregate.
 * @param byte_size Positive complete capability bytes.
 * @param limits Retained endpoint hard limits.
 * @param aggregate Non-null running checked aggregate.
 * @return Nothing after exact bounded addition.
 * @throws std::invalid_argument for a null aggregate.
 * @throws IsolatedCpuProtocolError for overflow or a retained/hard-limit
 * excess.
 */
void add_preflight_shared_bytes(std::uint64_t byte_size,
                                const IsolatedCpuInvocationLimits& limits,
                                std::uint64_t* aggregate) {
  if (aggregate == nullptr) {
    throw std::invalid_argument(
        "isolated CPU preflight shared-byte aggregate is null");
  }
  if (*aggregate > std::numeric_limits<std::uint64_t>::max() - byte_size) {
    throw IsolatedCpuProtocolError(
        "isolated CPU aggregate shared-memory bytes overflow");
  }
  *aggregate += byte_size;
  if (*aggregate > limits.maximum_shared_memory_bytes ||
      *aggregate > kMaximumIsolatedCpuSharedBytes) {
    throw IsolatedCpuProtocolError(
        "isolated CPU aggregate shared-memory bytes exceed their bound");
  }
}

/**
 * @brief Validates and canonicalizes all Host-derived plan facts without OS
 * effects.
 * @param invocation Host input Values and exact output plans.
 * @param limits Retained local hard limits.
 * @return Fully validated request and packet plus retained input read leases.
 * @throws IsolatedCpuProtocolError for unsupported or inconsistent state.
 * @throws Value/readiness/access/allocation errors from public Value APIs.
 * @throws std::bad_alloc when strictly bounded request/lease storage cannot
 * allocate.
 * @note Identity, operation/parameter text and canonical state, counts,
 * readiness, Host visibility, descriptor geometry/layout/storage, checked
 * aggregate resources, and complete encoded packet size are rejected before
 * any shm/FD/mmap/fork effect.
 */
HostInvocationPreflight preflight_host_invocation(
    const IsolatedCpuHostInvocation& invocation,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_limits(limits);
  validate_isolated_cpu_invocation_metadata(
      invocation.identity, invocation.operation, invocation.parameters, limits);
  if (invocation.outputs.empty()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU invocation requires at least one output");
  }
  if (!invocation.operation_identity.valid() ||
      !invocation.implementation_identity.valid() ||
      !invocation.configuration_schema_identity.valid() ||
      invocation.input_bindings.size() != invocation.inputs.size()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU operation identity or input binding is incomplete");
  }
  if (invocation.inputs.size() >
          std::numeric_limits<std::size_t>::max() - invocation.outputs.size() ||
      invocation.inputs.size() + invocation.outputs.size() >
          kMaximumIsolatedCpuDescriptors ||
      invocation.inputs.size() + invocation.outputs.size() >
          limits.maximum_descriptors ||
      invocation.inputs.size() + invocation.outputs.size() >
          limits.maximum_capabilities) {
    throw IsolatedCpuProtocolError(
        "isolated CPU invocation tensor count exceeds its bound");
  }

  HostInvocationPreflight preflight;
  IsolatedCpuInvocationRequest& request = preflight.request;
  request.identity = invocation.identity;
  request.operation = invocation.operation;
  request.operation_identity = invocation.operation_identity;
  request.implementation_identity = invocation.implementation_identity;
  request.configuration_schema_identity =
      invocation.configuration_schema_identity;
  request.parameters = invocation.parameters;
  request.configuration = invocation.configuration;
  request.input_count = static_cast<std::uint32_t>(invocation.inputs.size());
  request.output_count = static_cast<std::uint32_t>(invocation.outputs.size());
  const std::size_t total_count =
      invocation.inputs.size() + invocation.outputs.size();
  request.capabilities.reserve(total_count);
  request.tensors.reserve(total_count);
  preflight.input_leases.reserve(invocation.inputs.size());

  std::uint64_t aggregate_shared_bytes = 0U;
  std::uint64_t next_capability_id = 1U;
  for (std::size_t input_index = 0U; input_index < invocation.inputs.size();
       ++input_index) {
    const Value& value = invocation.inputs[input_index];
    const IsolatedCpuInputBinding& binding =
        invocation.input_bindings[input_index];
    if (!value.valid() ||
        value.representation_kind() != ValueRepresentationKind::DenseTensor ||
        value.storage_layout_kind() != StorageLayoutKind::Strided) {
      throw IsolatedCpuProtocolError(
          "isolated CPU input is not a Strided DenseTensor Value");
    }
    ReadLease lease = value.buffer_handle().acquire_read();
    if (lease.size() != value.storage_size()) {
      throw IsolatedCpuProtocolError(
          "isolated CPU input Value range size is inconsistent");
    }
    IsolatedCpuCapability capability;
    capability.capability_id = next_capability_id++;
    capability.access = IsolatedCpuCapabilityAccess::ReadOnly;
    capability.byte_size = complete_capability_size(lease.size());
    add_preflight_shared_bytes(capability.byte_size, limits,
                               &aggregate_shared_bytes);

    IsolatedCpuTensorDescriptor tensor =
        to_wire_tensor_descriptor(value.dense_tensor_descriptor(),
                                  value.image_facet(), value.strided_layout());
    tensor.access = IsolatedCpuTensorAccess::InputReadOnly;
    tensor.readiness = IsolatedCpuTensorReadiness::ReadyInput;
    tensor.ownership = IsolatedCpuTensorOwnership::HostInput;
    tensor.port_identity = binding.port_identity;
    tensor.binding_identity = binding.edge_identity;
    tensor.schema_identity = binding.schema_identity;
    tensor.facet_identity = binding.facet_identity;
    tensor.layout_identity = binding.layout_identity;
    tensor.schema_version = binding.schema_version;
    tensor.layout_version = binding.layout_version;
    tensor.descriptor_digest = binding.descriptor_digest;
    tensor.logical_content_digest = binding.logical_content_digest;
    tensor.layout_digest = binding.layout_digest;
    tensor.region = binding.region;
    tensor.capability_id = capability.capability_id;
    tensor.capability_offset = kCapabilityHeaderBytes;
    tensor.capability_length = static_cast<std::uint64_t>(lease.size());
    tensor.content_binding = compute_isolated_cpu_content_binding(
        request.identity, tensor, lease.data(), lease.size());
    request.capabilities.push_back(capability);
    request.tensors.push_back(std::move(tensor));
    preflight.input_leases.push_back(std::move(lease));
  }

  for (const IsolatedCpuDenseTensorOutputPlan& plan : invocation.outputs) {
    IsolatedCpuCapability capability;
    capability.capability_id = next_capability_id++;
    capability.access = IsolatedCpuCapabilityAccess::ReadWrite;
    capability.byte_size = complete_capability_size(plan.storage_size);
    add_preflight_shared_bytes(capability.byte_size, limits,
                               &aggregate_shared_bytes);

    IsolatedCpuTensorDescriptor tensor = to_wire_tensor_descriptor(
        plan.descriptor, plan.image_facet, plan.layout);
    tensor.access = IsolatedCpuTensorAccess::OutputWriteOnly;
    tensor.readiness = IsolatedCpuTensorReadiness::WritableOutput;
    tensor.ownership = IsolatedCpuTensorOwnership::RuntimeOutput;
    tensor.port_identity = plan.port_identity;
    tensor.binding_identity = plan.plan_identity;
    tensor.schema_identity = plan.schema_identity;
    tensor.facet_identity = plan.facet_identity;
    tensor.layout_identity = plan.layout_identity;
    tensor.schema_version = plan.schema_version;
    tensor.layout_version = plan.layout_version;
    tensor.descriptor_digest = plan.descriptor_digest;
    tensor.logical_content_digest = plan.logical_content_digest;
    tensor.layout_digest = plan.layout_digest;
    tensor.region = plan.region;
    tensor.allocation_alignment = static_cast<std::uint64_t>(plan.alignment);
    tensor.capability_id = capability.capability_id;
    tensor.capability_offset = kCapabilityHeaderBytes;
    tensor.capability_length = static_cast<std::uint64_t>(plan.storage_size);
    request.capabilities.push_back(capability);
    request.tensors.push_back(std::move(tensor));
  }

  request.resources.shared_memory_bytes = aggregate_shared_bytes;
  request.resources.descriptor_count =
      static_cast<std::uint32_t>(request.tensors.size());
  request.resources.cpu_slots = 1U;
  validate_isolated_cpu_invocation_request(request, limits);
  try {
    preflight.request_packet =
        encode_isolated_cpu_invocation_request(request, limits);
  } catch (const std::length_error& error) {
    throw IsolatedCpuProtocolError(error.what());
  }
  return preflight;
}

/**
 * @brief Copies one validated Host preflight into directional capabilities.
 * @param preflight Fully validated request and retained Ready input ranges.
 * @param limits Retained endpoint hard limits.
 * @return Request plus sole local FD/mapping owners.
 * @throws IsolatedCpuProtocolError for inconsistent retained state or copied
 * input binding.
 * @throws IsolatedCpuInvocationError for shm, FD, mmap, or protection failure.
 * @throws std::bad_alloc when bounded capability-owner storage cannot allocate.
 * @note No materializer is entered until the complete request validator has
 * accepted every Host-derived plan fact. The full validator runs again after
 * materialization as a defense-in-depth boundary.
 */
PreparedHostInvocation materialize_host_invocation(
    HostInvocationPreflight preflight,
    const IsolatedCpuInvocationLimits& limits) {
  if (preflight.input_leases.size() != preflight.request.input_count ||
      preflight.request.capabilities.size() !=
          preflight.request.tensors.size()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU Host preflight inventory is inconsistent");
  }
  PreparedHostInvocation prepared;
  prepared.request = std::move(preflight.request);
  prepared.request_packet = std::move(preflight.request_packet);
  if (prepared.request_packet.empty()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU Host preflight canonical packet is absent");
  }
  prepared.capabilities.reserve(prepared.request.capabilities.size());
  for (std::size_t index = 0U; index < prepared.request.capabilities.size();
       ++index) {
    const IsolatedCpuCapability& capability =
        prepared.request.capabilities[index];
    const IsolatedCpuTensorDescriptor& tensor = prepared.request.tensors[index];
    if (tensor.capability_length > std::numeric_limits<std::size_t>::max()) {
      throw IsolatedCpuProtocolError(
          "isolated CPU Host preflight range exceeds local size");
    }
    const std::size_t payload_size =
        static_cast<std::size_t>(tensor.capability_length);
    const std::byte* source = nullptr;
    if (index < prepared.request.input_count) {
      const ReadLease& lease = preflight.input_leases[index];
      if (!lease.valid() || lease.size() != payload_size) {
        throw IsolatedCpuProtocolError(
            "isolated CPU Host input lease changed after preflight");
      }
      source = lease.data();
    }
    MappedCapability mapping = prepare_capability(
        prepared.request.identity, capability, source, payload_size);
    if (index < prepared.request.input_count) {
      const ContentDigest copied_binding = compute_isolated_cpu_content_binding(
          prepared.request.identity, tensor,
          mapping.mapping.data() + tensor.capability_offset, payload_size);
      if (!tensor.content_binding.has_value() ||
          !(copied_binding == *tensor.content_binding)) {
        throw IsolatedCpuProtocolError(
            "isolated CPU copied input binding changed after preflight");
      }
    }
    prepared.capabilities.push_back(std::move(mapping));
  }
  validate_isolated_cpu_invocation_request(prepared.request, limits);
  return prepared;
}

/**
 * @brief Authorizes resources and materializes capabilities in strict order.
 * @param invocation Host-owned invocation plan.
 * @param limits Validated protocol-v2 bounds.
 * @param authorized_runtime Retained signed runtime package.
 * @param ledger Attempt-local sole Host resource mint.
 * @param policy Validated composition resource policy.
 * @param lease Receives consumed exact authority before materialization.
 * @return Materialized request and capabilities.
 * @throws Trust, protocol, resource, Value, and materialization failures
 * unchanged.
 * @note Complete side-effect-free preflight and signed package equality precede
 * atomic token issuance. Successful token consumption precedes every
 * invocation-owned shm, descriptor, socket, mapping, fork, or exec effect.
 */
PreparedHostInvocation admit_and_materialize_host_invocation(
    const IsolatedCpuHostInvocation& invocation,
    const IsolatedCpuInvocationLimits& limits,
    const AuthorizedPluginFile& authorized_runtime, ResourceLedger& ledger,
    const PluginInvocationResourcePolicy& policy,
    std::optional<ResourceLedger::PluginResourceLease>* lease) {
  if (lease == nullptr || lease->has_value()) {
    throw std::invalid_argument(
        "Plugin invocation lease output is null or already active.");
  }
  HostInvocationPreflight preflight =
      preflight_host_invocation(invocation, limits);
  if (invocation_package_identity(invocation.identity) !=
      authorized_runtime.package_identity()) {
    throw PluginTrustError(
        PluginTrustErrorCode::PackageMismatch,
        "Plugin invocation package identity differs from signed runtime.");
  }
  const PluginResourceVector resources =
      plugin_resource_demand(preflight, policy);
  const PluginInvocationIdentityDigest identity =
      plugin_invocation_identity_digest(invocation.identity);
  ResourceLedger::PluginResourceToken token =
      ledger.issue_plugin_invocation(identity, resources);
  lease->emplace(std::move(token).consume(identity, resources));
  return materialize_host_invocation(std::move(preflight), limits);
}

/**
 * @brief Recomputes and compares every request input content binding.
 * @param request Validated request.
 * @param mappings Header-validated capability mappings.
 * @return Nothing when each immutable input remains exact.
 * @throws IsolatedCpuProtocolError for a missing or changed binding.
 * @throws Canonical digest errors unchanged.
 */
void validate_runtime_input_bindings(
    const IsolatedCpuInvocationRequest& request,
    const std::vector<MappedCapability>& mappings) {
  for (std::size_t index = 0U; index < request.input_count; ++index) {
    const IsolatedCpuTensorDescriptor& tensor = request.tensors[index];
    const MappedCapability& capability =
        find_mapped_capability(mappings, tensor.capability_id);
    const ContentDigest observed = compute_isolated_cpu_content_binding(
        request.identity, tensor,
        capability.mapping.data() + tensor.capability_offset,
        static_cast<std::size_t>(tensor.capability_length));
    if (!tensor.content_binding.has_value() ||
        !(observed == *tensor.content_binding)) {
      throw IsolatedCpuProtocolError(
          "isolated CPU input content binding changed before callback");
    }
  }
}

/**
 * @brief Copies child-owned diagnostic bytes into the protocol bound.
 * @param diagnostic Untrusted-intended process-local text.
 * @param fallback Nonempty fallback used when required and input is empty.
 * @param require_nonempty Whether response framing requires a diagnostic.
 * @return NUL-free diagnostic no larger than the protocol maximum.
 * @throws std::bad_alloc when bounded result storage cannot allocate.
 */
std::string bounded_runtime_diagnostic(const std::string& diagnostic,
                                       const char* fallback,
                                       bool require_nonempty) {
  const std::size_t nul = diagnostic.find('\0');
  const std::size_t available =
      nul == std::string::npos ? diagnostic.size() : nul;
  std::string result = diagnostic.substr(
      0U, std::min(available, kMaximumIsolatedCpuDiagnosticBytes));
  if (require_nonempty && result.empty()) {
    result = fallback;
    if (result.size() > kMaximumIsolatedCpuDiagnosticBytes) {
      result.resize(kMaximumIsolatedCpuDiagnosticBytes);
    }
  }
  return result;
}

/**
 * @brief Reconstructs callback-local pointers from validated descriptor ranges.
 * @param request Validated pointer-free request.
 * @param mappings Header- and binding-validated shared mappings.
 * @return Complete callback invocation whose pointers borrow mappings.
 * @throws IsolatedCpuProtocolError for a missing mapping or local size limit.
 * @throws std::bad_alloc when bounded callback state cannot allocate.
 */
IsolatedCpuRuntimeInvocation build_runtime_invocation(
    const IsolatedCpuInvocationRequest& request,
    const std::vector<MappedCapability>& mappings) {
  IsolatedCpuRuntimeInvocation invocation;
  invocation.identity = request.identity;
  invocation.operation = request.operation;
  invocation.operation_identity = request.operation_identity;
  invocation.implementation_identity = request.implementation_identity;
  invocation.configuration_schema_identity =
      request.configuration_schema_identity;
  invocation.parameters = request.parameters;
  invocation.configuration = request.configuration;
  invocation.inputs.reserve(request.input_count);
  invocation.outputs.reserve(request.output_count);
  for (std::size_t index = 0U; index < request.tensors.size(); ++index) {
    const IsolatedCpuTensorDescriptor& tensor = request.tensors[index];
    if (tensor.capability_length > std::numeric_limits<std::size_t>::max()) {
      throw IsolatedCpuProtocolError(
          "isolated CPU tensor range exceeds local address representation");
    }
    const MappedCapability& capability =
        find_mapped_capability(mappings, tensor.capability_id);
    std::byte* range = capability.mapping.data() + tensor.capability_offset;
    IsolatedCpuRuntimeTensor runtime_tensor;
    runtime_tensor.descriptor = tensor;
    runtime_tensor.size = static_cast<std::size_t>(tensor.capability_length);
    if (index < request.input_count) {
      runtime_tensor.input_data = range;
      invocation.inputs.push_back(std::move(runtime_tensor));
    } else {
      runtime_tensor.output_data = range;
      invocation.outputs.push_back(std::move(runtime_tensor));
    }
  }
  return invocation;
}

/**
 * @brief Executes the process-local callback and constructs a validated
 * response.
 * @param request Exact retained request.
 * @param mappings Complete runtime mappings borrowed for callback duration.
 * @param callback Nonempty process-local callback.
 * @param limits Runtime-local hard limits.
 * @return Canonical response with bindings only for successful outputs.
 * @throws Protocol, mapping, digest, or allocation failures unchanged.
 * @note Callback exceptions become PluginFailed; infrastructure exceptions do
 * not forge a plugin outcome and instead make the endpoint fail nonzero.
 */
IsolatedCpuInvocationResponse execute_runtime_callback(
    const IsolatedCpuInvocationRequest& request,
    const std::vector<MappedCapability>& mappings,
    const IsolatedCpuRuntimeCallback& callback,
    const IsolatedCpuInvocationLimits& limits) {
  const IsolatedCpuRuntimeInvocation invocation =
      build_runtime_invocation(request, mappings);
  IsolatedCpuRuntimeCallbackResult callback_result;
  try {
    callback_result = callback(invocation);
  } catch (const std::exception& error) {
    callback_result.outcome = IsolatedCpuInvocationOutcome::PluginFailed;
    callback_result.diagnostic = bounded_runtime_diagnostic(
        std::string("isolated CPU callback exception: ") + error.what(),
        "isolated CPU callback raised an exception", true);
  } catch (...) {
    callback_result.outcome = IsolatedCpuInvocationOutcome::PluginFailed;
    callback_result.diagnostic =
        "isolated CPU callback raised a non-standard exception";
  }

  IsolatedCpuInvocationResponse response;
  response.identity = request.identity;
  response.operation = request.operation;
  response.resources = request.resources;
  response.outcome = callback_result.outcome;
  switch (callback_result.outcome) {
    case IsolatedCpuInvocationOutcome::Succeeded:
      response.diagnostic.clear();
      response.outputs.reserve(request.output_count);
      for (std::size_t index = 0U; index < request.output_count; ++index) {
        IsolatedCpuTensorDescriptor output =
            request.tensors[request.input_count + index];
        const MappedCapability& capability =
            find_mapped_capability(mappings, output.capability_id);
        validate_capability_fd(capability.descriptor.get(),
                               capability.capability);
        validate_capability_header(capability.mapping, request.identity,
                                   capability.capability);
        output.readiness = IsolatedCpuTensorReadiness::ReadyOutputCandidate;
        output.ownership = IsolatedCpuTensorOwnership::HostOutputCandidate;
        output.written_offset = 0U;
        output.written_length = output.capability_length;
        output.content_binding = compute_isolated_cpu_content_binding(
            request.identity, output,
            capability.mapping.data() + output.capability_offset,
            static_cast<std::size_t>(output.capability_length));
        response.outputs.push_back(std::move(output));
      }
      break;
    case IsolatedCpuInvocationOutcome::PluginFailed:
      response.diagnostic = bounded_runtime_diagnostic(
          callback_result.diagnostic, "isolated CPU callback reported failure",
          true);
      break;
    case IsolatedCpuInvocationOutcome::Cancelled:
      response.diagnostic =
          bounded_runtime_diagnostic(callback_result.diagnostic, "", false);
      break;
    default:
      throw IsolatedCpuProtocolError(
          "isolated CPU callback returned an invalid outcome");
  }
  validate_isolated_cpu_invocation_response(request, response, limits);
  return response;
}

/**
 * @brief Revalidates every retained Host FD and capability identity after exit.
 * @param request Exact retained request.
 * @param mappings Sole Host capability owners.
 * @return Nothing when every file, access mode, size, and header remains exact.
 * @throws IsolatedCpuProtocolError or IsolatedCpuInvocationError on mismatch.
 */
void validate_host_capabilities_after_exit(
    const IsolatedCpuInvocationRequest& request,
    const std::vector<MappedCapability>& mappings) {
  if (mappings.size() != request.capabilities.size()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU Host capability inventory changed");
  }
  for (std::size_t index = 0U; index < mappings.size(); ++index) {
    if (!(mappings[index].capability == request.capabilities[index])) {
      throw IsolatedCpuProtocolError(
          "isolated CPU Host capability declaration changed");
    }
    validate_capability_fd(mappings[index].descriptor.get(),
                           mappings[index].capability);
    validate_capability_header(mappings[index].mapping, request.identity,
                               mappings[index].capability);
  }
}

/**
 * @brief Snapshots and binding-validates outputs into fresh Host-owned Values.
 * @param invocation Original Host output plans.
 * @param response Structurally validated successful response.
 * @param mappings Revalidated retained Host mappings.
 * @return Fresh immutable Values in exact output order.
 * @throws ValueBuilder validation/allocation/publication errors unchanged.
 * @throws IsolatedCpuProtocolError for local response/plan or copied-content
 * inconsistency.
 * @throws std::bad_alloc when output vectors, complete descriptor/ImageFacet
 *         copies, or immutable Value publication cannot allocate.
 * @note Binding the fresh copy closes the shared-memory check/use window. The
 * local vector provides all-or-nothing escape: partial Values are destroyed if
 * a later output cannot be constructed or validated.
 */
std::vector<Value> publish_host_outputs(
    const IsolatedCpuHostInvocation& invocation,
    const IsolatedCpuInvocationResponse& response,
    const std::vector<MappedCapability>& mappings) {
  if (response.outputs.size() != invocation.outputs.size()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU output response and Host plan counts differ");
  }
  std::vector<Value> outputs;
  outputs.reserve(invocation.outputs.size());
  for (std::size_t index = 0U; index < invocation.outputs.size(); ++index) {
    const IsolatedCpuDenseTensorOutputPlan& plan = invocation.outputs[index];
    const IsolatedCpuTensorDescriptor& descriptor = response.outputs[index];
    const MappedCapability& capability =
        find_mapped_capability(mappings, descriptor.capability_id);
    if (descriptor.capability_length != plan.storage_size) {
      throw IsolatedCpuProtocolError(
          "isolated CPU returned output size differs from Host plan");
    }
    ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
        plan.descriptor, plan.image_facet, plan.layout, plan.storage_size);
    DenseTensorValueDescriptorMetadataAccess::attach(
        &builder, dense_tensor_value_metadata(plan));
    {
      WriteLease lease = builder.acquire_write();
      if (lease.size() != plan.storage_size) {
        throw IsolatedCpuProtocolError(
            "isolated CPU fresh Host output allocation size is inconsistent");
      }
      std::memcpy(lease.data(),
                  capability.mapping.data() + descriptor.capability_offset,
                  plan.storage_size);
      const ContentDigest observed = compute_isolated_cpu_content_binding(
          invocation.identity, descriptor, lease.data(), lease.size());
      if (!descriptor.content_binding.has_value() ||
          !(observed == *descriptor.content_binding)) {
        throw IsolatedCpuProtocolError(
            "isolated CPU copied output content binding is invalid");
      }
    }
    outputs.push_back(builder.seal());
  }
  return outputs;
}

/**
 * @brief Validates Host resource accounting and child-limit composition.
 * @param policy Candidate address-space, CPU-time, and descriptor policy.
 * @return Nothing when every field is positive and representable by rlimit.
 * @throws std::invalid_argument for a zero, undersized, or unrepresentable
 * field.
 * @note The descriptor overhead must cover standard streams, control/setup,
 * optional supervision, the executable capability, and loader working space.
 */
void validate_plugin_invocation_resource_policy(
    const PluginInvocationResourcePolicy& policy) {
  constexpr std::uint64_t kMinimumDescriptorOverhead = 16U;
  const std::uint64_t native_limit_maximum =
      static_cast<std::uint64_t>(std::numeric_limits<rlim_t>::max());
  if (policy.address_space_bytes == 0U ||
      policy.address_space_bytes > native_limit_maximum) {
    throw std::invalid_argument(
        "plugin runtime address-space limit is zero or unrepresentable");
  }
  if (policy.cpu_time_seconds == 0U ||
      policy.cpu_time_seconds > native_limit_maximum) {
    throw std::invalid_argument(
        "plugin runtime CPU-time limit is zero or unrepresentable");
  }
  if (policy.descriptor_overhead < kMinimumDescriptorOverhead ||
      policy.descriptor_overhead > native_limit_maximum) {
    throw std::invalid_argument(
        "plugin runtime descriptor overhead is unsafe or unrepresentable");
  }
}

/**
 * @brief Validates and transfers one required ledger during member init.
 * @param ledger Candidate attempt-local Host authority.
 * @return Same nonnull shared owner.
 * @throws std::invalid_argument when no ledger was supplied.
 */
std::shared_ptr<ResourceLedger> require_plugin_resource_ledger(
    std::shared_ptr<ResourceLedger> ledger) {
  if (!ledger) {
    throw std::invalid_argument("plugin runtime resource ledger is null");
  }
  return ledger;
}

/**
 * @brief Validates and copies one policy during immutable member init.
 * @param policy Candidate plugin resource policy.
 * @return Same policy after complete validation.
 * @throws std::invalid_argument from policy validation unchanged.
 */
PluginInvocationResourcePolicy require_plugin_invocation_resource_policy(
    PluginInvocationResourcePolicy policy) {
  validate_plugin_invocation_resource_policy(policy);
  return policy;
}

/**
 * @brief Validates every supervisor duration before child ownership begins.
 * @param options Candidate lifecycle timing policy.
 * @return Nothing for positive, ordered, bounded, exactly representable
 * durations.
 * @throws std::invalid_argument for a non-positive, inverted, excessive, or
 * inexactly representable duration.
 * @throws std::bad_alloc when constructing a rejection diagnostic exhausts
 * memory.
 * @note Construction validates duration shape and relationships only. Every
 * later deadline derivation separately proves its captured base can accept the
 * exact converted increment before any addition occurs.
 */
void validate_plugin_runtime_supervisor_options(
    const PluginRuntimeSupervisorOptions& options) {
  static_cast<void>(
      validate_and_convert_supervisor_duration(options.startup_timeout));
  static_cast<void>(
      validate_and_convert_supervisor_duration(options.heartbeat_interval));
  static_cast<void>(
      validate_and_convert_supervisor_duration(options.heartbeat_timeout));
  static_cast<void>(
      validate_and_convert_supervisor_duration(options.invocation_timeout));
  static_cast<void>(
      validate_and_convert_supervisor_duration(options.response_timeout));
  static_cast<void>(
      validate_and_convert_supervisor_duration(options.termination_grace));
  static_cast<void>(
      validate_and_convert_supervisor_duration(options.kill_reap_timeout));
  static_cast<void>(
      validate_and_convert_supervisor_duration(options.restart_backoff));
  if (options.heartbeat_interval >= options.heartbeat_timeout) {
    throw std::invalid_argument(
        "plugin runtime heartbeat interval must be below its timeout");
  }
}

/**
 * @brief Builds the strongest factual fault from one exact wait status.
 * @param status Exact status returned by `waitpid`.
 * @param context Bounded-intended lifecycle context.
 * @return Typed natural exit, signal, or unexpected-normal-output fault.
 * @throws std::bad_alloc when diagnostic storage cannot allocate.
 */
PluginRuntimeFault plugin_runtime_fault_from_wait_status(
    int status, const std::string& context) {
  if (WIFSIGNALED(status)) {
    const int signal_number = WTERMSIG(status);
    return PluginRuntimeFault(PluginRuntimeFaultKind::ProcessSignal,
                              context + ": runtime terminated by signal " +
                                  std::to_string(signal_number),
                              status, std::nullopt, signal_number,
                              PluginRuntimeTerminationStage::None,
                              signal_number == SIGKILL);
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
    const int exit_code = WEXITSTATUS(status);
    return PluginRuntimeFault(
        PluginRuntimeFaultKind::ProcessExit,
        context + ": runtime exited with status " + std::to_string(exit_code),
        status, exit_code);
  }
  if (WIFEXITED(status)) {
    return PluginRuntimeFault(
        PluginRuntimeFaultKind::BadOutput,
        context + ": runtime exited normally without complete valid output",
        status, 0);
  }
  return PluginRuntimeFault(
      PluginRuntimeFaultKind::Channel,
      context + ": runtime produced an unsupported wait status", status);
}

/**
 * @brief Classifies definitive premature response framing EOF without a
 * scheduling grace period.
 * @param error Exact orderly EOF fact raised by the framed response receiver.
 * @param child Sole exact-PID owner for an optional nonblocking status sample.
 * @return Bad output while the runtime is still live or after clean exit,
 * stronger natural abnormal process status when already waitable, or channel
 * failure when exact status sampling itself fails.
 * @throws std::bad_alloc when diagnostic storage cannot allocate.
 * @note Classification never waits for the child to become waitable. This
 * keeps a live runtime's premature EOF deterministically `BadOutput` while
 * preserving already observable signal/nonzero-exit facts and true `waitpid`
 * failures.
 */
PluginRuntimeFault plugin_runtime_fault_from_premature_response_eof(
    const PrematureFramedPacketEof& error, SupervisedChildOwner* child) {
  if (child == nullptr) {
    return PluginRuntimeFault(
        PluginRuntimeFaultKind::Channel,
        std::string(error.what()) +
            "; plugin runtime exact child owner is absent");
  }
  try {
    if (child->poll_reap()) {
      return plugin_runtime_fault_from_wait_status(
          *child->wait_status(), "plugin runtime response framing EOF");
    }
  } catch (const IsolatedCpuInvocationError& status_error) {
    return PluginRuntimeFault(
        PluginRuntimeFaultKind::Channel,
        std::string(error.what()) +
            "; plugin runtime exact status observation failed: " +
            status_error.what());
  }
  return PluginRuntimeFault(PluginRuntimeFaultKind::BadOutput, error.what());
}

/**
 * @brief Enforces callback and heartbeat bounds at one monotonic observation.
 * @param accepted_at Observation instant; after complete event authentication
 * this is the lifecycle acceptance linearization point.
 * @param invocation_deadline Absolute callback-completion deadline.
 * @param heartbeat_deadline Current absolute maximum heartbeat-gap deadline.
 * @return Nothing when both applicable deadlines remain open.
 * @throws PluginRuntimeFault with invocation-deadline priority when it is
 * reached, otherwise heartbeat-timeout when only that deadline is reached.
 * @note The monitor uses the same priority before waiting and after event
 * authentication. Invocation deadline deliberately outranks heartbeat timeout
 * when both are reached at one observation.
 */
void enforce_invocation_lifecycle_acceptance_deadlines(
    SupervisorDeadline accepted_at, SupervisorDeadline invocation_deadline,
    SupervisorDeadline heartbeat_deadline) {
  if (accepted_at >= invocation_deadline) {
    throw PluginRuntimeFault(PluginRuntimeFaultKind::InvocationDeadline,
                             "plugin runtime invocation timed out");
  }
  if (accepted_at >= heartbeat_deadline) {
    throw PluginRuntimeFault(PluginRuntimeFaultKind::HeartbeatTimeout,
                             "plugin runtime heartbeat timed out");
  }
}

/**
 * @brief Validates one child event against the retained private session.
 * @param event Decoded child event.
 * @param nonce Exact Host-generated nonce.
 * @param identity Exact retained complete invocation identity.
 * @param heartbeat_interval_milliseconds Exact Host-selected interval.
 * @param expected_sequence Next required child sequence.
 * @return Nothing after exact comparison.
 * @throws PluginRuntimeFault for nonce, identity, or sequence mismatch.
 */
void validate_plugin_runtime_lifecycle_session(
    const PluginRuntimeLifecycleFrame& event,
    const PluginRuntimeSessionNonce& nonce,
    const IsolatedCpuInvocationIdentity& identity,
    std::uint64_t heartbeat_interval_milliseconds,
    std::uint64_t expected_sequence) {
  if (event.nonce != nonce || !(event.identity == identity) ||
      event.heartbeat_interval_milliseconds !=
          heartbeat_interval_milliseconds ||
      event.sequence != expected_sequence) {
    throw PluginRuntimeFault(
        PluginRuntimeFaultKind::LifecycleProtocol,
        "plugin runtime lifecycle session, identity, or sequence mismatch");
  }
}

/**
 * @brief Holds one armed invocation monitor until exact child exit is visible.
 * @param child Sole exact-PID owner, whose wait status remains unconsumed.
 * @param deadline Absolute invocation bound limiting the source-private hold.
 * @return Nothing when disabled or `waitid(WNOWAIT)` proves normal zero exit.
 * @throws IsolatedCpuInvocationError for an absent child, `waitid`/poll
 * failure, abnormal exit, or a hold that cannot observe exit before the
 * invocation bound.
 * @note The one-shot flag is consumed before any potentially throwing work.
 * The helper neither reaps nor signals the child and exists only to model Host
 * descheduling after request transfer while real lifecycle and response data
 * become queued. Normal production behavior performs no extra syscall.
 */
void hold_invocation_monitor_until_child_exit_for_test(
    SupervisedChildOwner* child, SupervisorDeadline deadline) {
  if (!g_next_invocation_monitor_exit_hold.exchange(
          false, std::memory_order_acq_rel)) {
    return;
  }
  if (child == nullptr || !child->active()) {
    throw IsolatedCpuInvocationError(
        "plugin runtime invocation-monitor test hold has no active child");
  }
  const pid_t pid = child->pid();
  for (;;) {
    siginfo_t information{};
    const int observed = ::waitid(P_PID, static_cast<id_t>(pid), &information,
                                  WEXITED | WNOHANG | WNOWAIT);
    if (observed == 0 && information.si_pid == pid) {
      if (information.si_code != CLD_EXITED || information.si_status != 0) {
        throw IsolatedCpuInvocationError(
            "plugin runtime invocation-monitor test child did not exit "
            "normally");
      }
      return;
    }
    if (observed < 0 && errno != EINTR) {
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime invocation-monitor test wait failed: ") +
          std::strerror(errno));
    }
    if (SupervisorClock::now() >= deadline) {
      throw IsolatedCpuInvocationError(
          "plugin runtime invocation-monitor test hold timed out");
    }
    const int timeout = std::min(poll_timeout_until(deadline), 5);
    int poll_result = -1;
    do {
      poll_result = ::poll(nullptr, 0U, timeout);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) {
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime invocation-monitor test poll failed: ") +
          std::strerror(errno));
    }
  }
}

/**
 * @brief Gives a just-closed lifecycle channel a short exact-status priority.
 * @param child Exact child owner.
 * @param enclosing_deadline Current absolute lifecycle bound.
 * @return Exact status when it becomes waitable in the bounded observation.
 * @throws IsolatedCpuInvocationError for exact wait failure.
 * @throws std::overflow_error when the short exact observation deadline cannot
 * be represented by the monotonic clock.
 * @note The caller-supplied enclosing deadline is a limit, not permission to
 * clamp an unrepresentable fresh derivation.
 */
std::optional<int> observe_child_after_channel_close(
    SupervisedChildOwner* child, SupervisorDeadline enclosing_deadline) {
  if (child == nullptr) {
    return std::nullopt;
  }
  const SupervisorDeadline observation_deadline =
      std::min(enclosing_deadline,
               checked_supervisor_deadline(SupervisorClock::now(),
                                           std::chrono::milliseconds{20}));
  if (child->wait_until(observation_deadline)) {
    return child->wait_status();
  }
  return std::nullopt;
}

/**
 * @brief Enriches a primary fault after bounded channel revocation/escalation.
 * @param fault Primary lifecycle fact.
 * @param process Exact child and invocation endpoints to retire.
 * @param options Validated termination timing policy.
 * @param pending_reap Receives deferred exact-reap completion ownership.
 * @return Never returns; throws the enriched primary or reap-pending fault.
 * @throws PluginRuntimeFault after best-effort exact cleanup.
 * @note Cleanup infrastructure failure before PID transfer appends its
 * diagnostic and preserves the primary kind. A deliberate final-bound
 * transfer instead publishes `ReapPending`, which outranks the earlier fact
 * while the supervisor remains quarantined.
 */
[[noreturn]] void throw_after_supervised_cleanup(
    const PluginRuntimeFault& fault, SupervisedSpawnedChild* process,
    const PluginRuntimeSupervisorOptions& options,
    std::shared_ptr<std::atomic<bool>>* pending_reap) {
  SupervisedTerminationResult termination;
  if (process != nullptr) {
    process->control.reset();
    process->supervision.reset();
    process->setup_status.reset();
    try {
      termination = process->child.terminate_and_reap(options);
    } catch (const std::exception& error) {
      std::optional<int> wait_status = fault.wait_status();
      if (!wait_status.has_value()) {
        wait_status = process->child.wait_status();
      }
      const PluginRuntimeTerminationStage cleanup_stage =
          process->child.termination_stage();
      const PluginRuntimeTerminationStage strongest_stage =
          static_cast<std::uint8_t>(cleanup_stage) >
                  static_cast<std::uint8_t>(fault.termination_stage())
              ? cleanup_stage
              : fault.termination_stage();
      throw PluginRuntimeFault(
          fault.kind(),
          std::string(fault.what()) +
              "; plugin runtime cleanup also failed: " + error.what(),
          wait_status, fault.exit_code(), fault.signal_number(),
          strongest_stage, fault.memory_pressure_compatible());
    }
  }
  if (termination.reap_pending) {
    if (pending_reap != nullptr) {
      *pending_reap = termination.reap_completion;
    }
    throw PluginRuntimeFault(
        PluginRuntimeFaultKind::ReapPending,
        std::string(fault.what()) +
            "; exact PID ownership moved to deferred reaper",
        std::nullopt, std::nullopt, std::nullopt, termination.stage);
  }

  std::optional<int> wait_status = fault.wait_status();
  std::optional<int> exit_code = fault.exit_code();
  std::optional<int> signal_number = fault.signal_number();
  if (!wait_status.has_value() && termination.wait_status.has_value()) {
    wait_status = termination.wait_status;
    if (WIFEXITED(*wait_status)) {
      exit_code = WEXITSTATUS(*wait_status);
    } else if (WIFSIGNALED(*wait_status)) {
      signal_number = WTERMSIG(*wait_status);
    }
  }
  const bool memory_pressure_compatible =
      fault.memory_pressure_compatible() ||
      (signal_number.has_value() && *signal_number == SIGKILL);
  throw PluginRuntimeFault(fault.kind(), fault.what(), wait_status, exit_code,
                           signal_number, termination.stage,
                           memory_pressure_compatible);
}

/**
 * @brief Waits through exec-status EOF under the startup deadline.
 * @param process Fresh child and nonblocking setup-status endpoint.
 * @param deadline Absolute startup deadline.
 * @return Nothing only after close-on-exec proves successful exec.
 * @throws PluginRuntimeFault for timeout, setup record, or natural child exit.
 * @throws IsolatedCpuInvocationError for poll/read/exact-wait failures.
 * @note Close-on-exec EOF is accepted only after a final deadline observation,
 * so scheduler delay after `poll`/`read` readiness cannot revive startup.
 */
void await_supervised_exec(SupervisedSpawnedChild* process,
                           SupervisorDeadline deadline) {
  if (process == nullptr || !process->setup_status.valid()) {
    throw PluginRuntimeFault(PluginRuntimeFaultKind::Channel,
                             "plugin runtime setup-status owner is absent");
  }
  std::array<std::byte, sizeof(int)> setup_bytes{};
  std::size_t offset = 0U;
  for (;;) {
    if (SupervisorClock::now() >= deadline) {
      throw PluginRuntimeFault(PluginRuntimeFaultKind::StartupDeadline,
                               "plugin runtime exec startup timed out");
    }
    if (process->child.poll_reap()) {
      throw plugin_runtime_fault_from_wait_status(
          *process->child.wait_status(), "plugin runtime exec startup");
    }
    struct pollfd status_poll{process->setup_status.get(),
                              static_cast<std::int16_t>(POLLIN | POLLHUP), 0};
    const int timeout = std::min(poll_timeout_until(deadline), 10);
    const int result = ::poll(&status_poll, 1U, timeout);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime exec-status poll failed: ") +
          std::strerror(errno));
    }
    if (result == 0) {
      continue;
    }
    for (;;) {
      const ssize_t count =
          ::read(process->setup_status.get(), setup_bytes.data() + offset,
                 setup_bytes.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        if (offset == setup_bytes.size()) {
          int child_error = 0;
          std::memcpy(&child_error, setup_bytes.data(), sizeof(child_error));
          throw PluginRuntimeFault(
              PluginRuntimeFaultKind::LifecycleProtocol,
              std::string("plugin runtime setup/exec failed: ") +
                  std::strerror(child_error == 0 ? EIO : child_error));
        }
        continue;
      }
      if (count == 0) {
        process->setup_status.reset();
        if (offset != 0U) {
          throw PluginRuntimeFault(
              PluginRuntimeFaultKind::LifecycleProtocol,
              "plugin runtime exec-status record was truncated");
        }
        if (SupervisorClock::now() >= deadline) {
          throw PluginRuntimeFault(PluginRuntimeFaultKind::StartupDeadline,
                                   "plugin runtime exec startup timed out");
        }
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime exec-status read failed: ") +
          std::strerror(errno));
    }
  }
}

/**
 * @brief Waits for and authenticates the first child startup event.
 * @param process Fresh child and supervision endpoint.
 * @param nonce Exact retained Host nonce.
 * @param identity Exact retained complete invocation identity.
 * @param heartbeat_interval_milliseconds Exact Host-selected interval.
 * @param deadline Absolute startup deadline.
 * @return Monotonic time of authenticated startup observation.
 * @throws PluginRuntimeFault for timeout, protocol/channel, or process fault.
 * @throws IsolatedCpuInvocationError for exact wait failure.
 * @note A decoded `RuntimeStarted` is accepted only after one post-validation
 * deadline observation; bytes queued before expiry do not extend startup.
 */
SupervisorDeadline await_authenticated_runtime_started(
    SupervisedSpawnedChild* process, const PluginRuntimeSessionNonce& nonce,
    const IsolatedCpuInvocationIdentity& identity,
    std::uint64_t heartbeat_interval_milliseconds,
    SupervisorDeadline deadline) {
  for (;;) {
    if (SupervisorClock::now() >= deadline) {
      throw PluginRuntimeFault(
          PluginRuntimeFaultKind::StartupDeadline,
          "plugin runtime authenticated startup timed out");
    }
    if (process->child.poll_reap()) {
      throw plugin_runtime_fault_from_wait_status(
          *process->child.wait_status(),
          "plugin runtime authenticated startup");
    }
    struct pollfd lifecycle_poll{
        process->supervision.get(),
        static_cast<std::int16_t>(POLLIN | POLLHUP | POLLERR), 0};
    const int timeout = std::min(poll_timeout_until(deadline), 10);
    const int result = ::poll(&lifecycle_poll, 1U, timeout);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw IsolatedCpuInvocationError(
          std::string("plugin runtime startup poll failed: ") +
          std::strerror(errno));
    }
    if (result == 0) {
      continue;
    }
    try {
      const auto event = receive_plugin_runtime_lifecycle_frame_nonblocking(
          process->supervision.get());
      if (!event.has_value()) {
        continue;
      }
      validate_plugin_runtime_lifecycle_session(
          *event, nonce, identity, heartbeat_interval_milliseconds, 1U);
      if (event->kind != PluginRuntimeLifecycleKind::RuntimeStarted) {
        throw PluginRuntimeFault(
            PluginRuntimeFaultKind::LifecycleProtocol,
            "plugin runtime first child event was not RuntimeStarted");
      }
      apply_supervised_lifecycle_acceptance_test_delay(event->kind);
      const SupervisorDeadline accepted_at = SupervisorClock::now();
      if (accepted_at >= deadline) {
        throw PluginRuntimeFault(
            PluginRuntimeFaultKind::StartupDeadline,
            "plugin runtime authenticated startup timed out");
      }
      return accepted_at;
    } catch (const IsolatedCpuProtocolError& error) {
      throw PluginRuntimeFault(PluginRuntimeFaultKind::LifecycleProtocol,
                               error.what());
    } catch (const IsolatedCpuInvocationError& error) {
      const auto status =
          observe_child_after_channel_close(&process->child, deadline);
      if (status.has_value()) {
        throw plugin_runtime_fault_from_wait_status(
            *status, "plugin runtime authenticated startup");
      }
      throw PluginRuntimeFault(PluginRuntimeFaultKind::Channel, error.what());
    }
  }
}

/**
 * @brief Monitors authenticated heartbeats until callback completion.
 * @param process Live exact child and lifecycle endpoint.
 * @param nonce Exact retained Host nonce.
 * @param identity Exact retained complete invocation identity.
 * @param heartbeat_interval_milliseconds Exact Host-selected interval.
 * @param invocation_deadline Absolute callback-completion deadline.
 * @param heartbeat_timeout Maximum gap from the latest valid event.
 * @param request_transfer_accepted_at Exact same-deadline observation that
 * accepted the complete request transfer.
 * @return Time of authenticated `InvocationCompleted` observation.
 * @throws PluginRuntimeFault for deadline, heartbeat, protocol, channel, or
 * natural process failure.
 * @throws IsolatedCpuInvocationError for exact wait failure.
 * @throws std::overflow_error when a heartbeat deadline cannot be represented
 * by the monotonic clock.
 * @note Each allowed event is accepted at one post-validation monotonic
 * observation. Invocation deadline outranks heartbeat timeout when both are
 * reached there, and an expired event cannot refresh the heartbeat gap. When
 * exact status is already reaped, the monitor drains every queued in-sequence
 * event before classifying that status as exit without completion; a valid
 * completion advances to response validation with the status still retained.
 */
SupervisorDeadline await_authenticated_invocation_completed(
    SupervisedSpawnedChild* process, const PluginRuntimeSessionNonce& nonce,
    const IsolatedCpuInvocationIdentity& identity,
    std::uint64_t heartbeat_interval_milliseconds,
    SupervisorDeadline invocation_deadline,
    std::chrono::milliseconds heartbeat_timeout,
    SupervisorDeadline request_transfer_accepted_at) {
  std::uint64_t expected_sequence = 2U;
  SupervisorDeadline heartbeat_deadline = checked_supervisor_deadline(
      request_transfer_accepted_at, heartbeat_timeout);
  for (;;) {
    const SupervisorDeadline now = SupervisorClock::now();
    enforce_invocation_lifecycle_acceptance_deadlines(now, invocation_deadline,
                                                      heartbeat_deadline);
    const bool child_reaped = process->child.poll_reap();
    const SupervisorDeadline next_deadline =
        std::min(invocation_deadline, heartbeat_deadline);
    if (!child_reaped) {
      struct pollfd lifecycle_poll{
          process->supervision.get(),
          static_cast<std::int16_t>(POLLIN | POLLHUP | POLLERR), 0};
      const int timeout = std::min(poll_timeout_until(next_deadline), 10);
      const int result = ::poll(&lifecycle_poll, 1U, timeout);
      if (result < 0) {
        if (errno == EINTR) {
          continue;
        }
        throw IsolatedCpuInvocationError(
            std::string("plugin runtime invocation poll failed: ") +
            std::strerror(errno));
      }
      if (result == 0) {
        continue;
      }
    }
    try {
      const auto event = receive_plugin_runtime_lifecycle_frame_nonblocking(
          process->supervision.get());
      if (!event.has_value()) {
        if (child_reaped) {
          throw plugin_runtime_fault_from_wait_status(
              *process->child.wait_status(), "plugin runtime invocation");
        }
        continue;
      }
      validate_plugin_runtime_lifecycle_session(*event, nonce, identity,
                                                heartbeat_interval_milliseconds,
                                                expected_sequence++);
      if (event->kind != PluginRuntimeLifecycleKind::Heartbeat &&
          event->kind != PluginRuntimeLifecycleKind::InvocationCompleted) {
        throw PluginRuntimeFault(
            PluginRuntimeFaultKind::LifecycleProtocol,
            "plugin runtime emitted an unexpected invocation event");
      }
      apply_supervised_lifecycle_acceptance_test_delay(event->kind);
      const SupervisorDeadline accepted_at = SupervisorClock::now();
      enforce_invocation_lifecycle_acceptance_deadlines(
          accepted_at, invocation_deadline, heartbeat_deadline);
      if (event->kind == PluginRuntimeLifecycleKind::Heartbeat) {
        heartbeat_deadline =
            checked_supervisor_deadline(accepted_at, heartbeat_timeout);
        continue;
      }
      return accepted_at;
    } catch (const IsolatedCpuProtocolError& error) {
      throw PluginRuntimeFault(PluginRuntimeFaultKind::LifecycleProtocol,
                               error.what());
    } catch (const IsolatedCpuInvocationError& error) {
      const auto status =
          observe_child_after_channel_close(&process->child, next_deadline);
      if (status.has_value()) {
        throw plugin_runtime_fault_from_wait_status(
            *status, "plugin runtime invocation");
      }
      throw PluginRuntimeFault(PluginRuntimeFaultKind::Channel, error.what());
    }
  }
}

/**
 * @brief Current phase used only to map internal channel/deadline exceptions.
 */
enum class SupervisedHostPhase : std::uint8_t {
  /** @brief Fork, exec-status, hello, and authenticated start. */
  Startup = 0,
  /** @brief Complete request transfer under its independent full bound. */
  RequestTransfer = 1,
  /** @brief Callback, heartbeat monitoring, and authenticated completion. */
  Invocation = 2,
  /** @brief Response, exact exit, validation, and Host publication. */
  Response = 3,
};

/**
 * @brief Maps one owned supervisor phase to its authoritative deadline fact.
 * @param phase Current closed Host lifecycle phase.
 * @return Startup, invocation, or response deadline kind for that phase.
 * @throws Nothing.
 * @note Request transfer and callback monitoring intentionally share
 * `InvocationDeadline`. The defensive fallback preserves startup fail-closed
 * behavior for an impossible invalid enum representation.
 */
PluginRuntimeFaultKind supervised_deadline_fault_kind(
    SupervisedHostPhase phase) noexcept {
  switch (phase) {
    case SupervisedHostPhase::Startup:
      return PluginRuntimeFaultKind::StartupDeadline;
    case SupervisedHostPhase::RequestTransfer:
    case SupervisedHostPhase::Invocation:
      return PluginRuntimeFaultKind::InvocationDeadline;
    case SupervisedHostPhase::Response:
      return PluginRuntimeFaultKind::ResponseDeadline;
  }
  return PluginRuntimeFaultKind::StartupDeadline;
}

/**
 * @brief Converts one owned deadline overflow into a typed cleaned-up fault.
 * @param phase Current lifecycle phase at the failed checked derivation.
 * @param error Exact private deadline-arithmetic failure.
 * @param process Exact child and invocation endpoints to retire.
 * @param options Validated termination timing policy.
 * @param pending_reap Receives quarantine completion if synchronous reap fails.
 * @return Never returns; throws the phase-typed or reap-pending fault.
 * @throws PluginRuntimeFault after exact best-effort cleanup.
 * @note This is the sole phase-mapping path for a pre-cleanup deadline
 * overflow after child ownership. `throw_after_supervised_cleanup` retains
 * cleanup diagnostics and deliberately gives `ReapPending` ownership transfer
 * priority over the phase fact.
 */
[[noreturn]] void throw_after_supervised_deadline_overflow_cleanup(
    SupervisedHostPhase phase, const SupervisorDeadlineOverflow& error,
    SupervisedSpawnedChild* process,
    const PluginRuntimeSupervisorOptions& options,
    std::shared_ptr<std::atomic<bool>>* pending_reap) {
  throw_after_supervised_cleanup(
      PluginRuntimeFault(
          supervised_deadline_fault_kind(phase),
          std::string("plugin runtime deadline arithmetic failed: ") +
              error.what()),
      process, options, pending_reap);
}

/**
 * @brief Executes one complete fresh supervised invocation.
 * @param authorized_runtime Retained signed exact runtime object.
 * @param resource_ledger Attempt-local sole resource-token mint.
 * @param resource_policy Validated admission and child-limit policy.
 * @param options Validated lifecycle timing policy.
 * @param limits Retained protocol-v2 bounds.
 * @param invocation Host-owned invocation plan.
 * @param pending_reap Receives quarantine completion if synchronous reap fails.
 * @return Complete typed callback result with fresh outputs after success.
 * @throws PluginRuntimeFault for every supervised lifecycle/process/output
 * fault.
 * @throws IsolatedCpuProtocolError for Host preflight failures before spawn.
 * @throws std::overflow_error when the pre-spawn startup deadline cannot be
 * represented by the monotonic clock.
 * @throws Value/readiness/access/allocation failures from Host preparation or
 * publication.
 * @note Every caught post-spawn fault revokes channels before bounded
 * escalation; no direct non-supervised retry exists. Complete request
 * transfer returns the exact same-deadline acceptance observation, and both
 * callback invocation and initial heartbeat-gap deadlines derive from that
 * timestamp without a later caller clock sample. Any checked lifecycle or
 * short channel-status observation overflow reached while the child is owned
 * and before cleanup begins is mapped by the current phase before that same
 * exact cleanup path runs. Cleanup and restart-backoff arithmetic retain an
 * already established primary fault; a deliberate final-bound PID transfer
 * still publishes the higher-priority `ReapPending` ownership fact.
 */
IsolatedCpuHostInvocationResult run_supervised_plugin_invocation(
    const AuthorizedPluginFile& authorized_runtime,
    ResourceLedger& resource_ledger,
    const PluginInvocationResourcePolicy& resource_policy,
    const PluginRuntimeSupervisorOptions& options,
    const IsolatedCpuInvocationLimits& limits,
    const IsolatedCpuHostInvocation& invocation,
    std::shared_ptr<std::atomic<bool>>* pending_reap) {
  const bool force_response_channel_observation_overflow =
      g_next_response_channel_observation_overflow.exchange(
          false, std::memory_order_acq_rel);
  bool response_channel_observation_overflow_ready = false;
  std::optional<ResourceLedger::PluginResourceLease> resource_lease;
  PreparedHostInvocation prepared = admit_and_materialize_host_invocation(
      invocation, limits, authorized_runtime, resource_ledger, resource_policy,
      &resource_lease);
  std::vector<int> descriptors;
  descriptors.reserve(prepared.capabilities.size());
  for (const MappedCapability& capability : prepared.capabilities) {
    descriptors.push_back(capability.descriptor.get());
  }
  const PluginRuntimeSessionNonce nonce = generate_plugin_runtime_nonce();

  SupervisedSpawnedChild process{};
  const SupervisorDeadline startup_deadline = checked_supervisor_deadline(
      SupervisorClock::now(), options.startup_timeout);
  try {
    process = spawn_supervised_runtime(
        authorized_runtime, resource_lease->resources(), resource_policy);
  } catch (const IsolatedCpuInvocationError& error) {
    throw PluginRuntimeFault(PluginRuntimeFaultKind::Channel, error.what());
  }

  SupervisedHostPhase phase = SupervisedHostPhase::Startup;
  try {
    await_supervised_exec(&process, startup_deadline);
    const PluginRuntimeLifecycleFrame hello{
        PluginRuntimeLifecycleKind::Hello, 0U, nonce, prepared.request.identity,
        static_cast<std::uint64_t>(options.heartbeat_interval.count())};
    send_plugin_runtime_lifecycle_frame_until(process.supervision.get(), hello,
                                              startup_deadline);
    static_cast<void>(await_authenticated_runtime_started(
        &process, nonce, prepared.request.identity,
        hello.heartbeat_interval_milliseconds, startup_deadline));

    phase = SupervisedHostPhase::RequestTransfer;
    const SupervisorDeadline request_transfer_deadline =
        checked_supervisor_deadline(SupervisorClock::now(),
                                    options.invocation_timeout);
    const SupervisorDeadline request_transfer_accepted_at =
        send_packet_until(process.control.get(), prepared.request_packet,
                          descriptors, request_transfer_deadline);
    const SupervisorDeadline invocation_deadline = checked_supervisor_deadline(
        request_transfer_accepted_at, options.invocation_timeout);
    phase = SupervisedHostPhase::Invocation;
    hold_invocation_monitor_until_child_exit_for_test(&process.child,
                                                      invocation_deadline);
    const SupervisorDeadline completed_at =
        await_authenticated_invocation_completed(
            &process, nonce, prepared.request.identity,
            hello.heartbeat_interval_milliseconds, invocation_deadline,
            options.heartbeat_timeout, request_transfer_accepted_at);

    phase = SupervisedHostPhase::Response;
    const SupervisorDeadline response_deadline =
        checked_supervisor_deadline(completed_at, options.response_timeout);
    if (force_response_channel_observation_overflow) {
      int regular_descriptor_flags = O_RDONLY;
#ifdef O_CLOEXEC
      regular_descriptor_flags |= O_CLOEXEC;
#endif
      const int regular_descriptor =
          ::open("/dev/null", regular_descriptor_flags);
      if (regular_descriptor < 0) {
        throw IsolatedCpuInvocationError(
            std::string("plugin runtime response-channel test replacement "
                        "failed: ") +
            std::strerror(errno));
      }
      process.control.reset(regular_descriptor);
      response_channel_observation_overflow_ready = true;
    }
    ReceivedPacket received =
        receive_packet(process.control.get(), response_deadline);
    process.control.reset();
    process.supervision.reset();
    if (!process.child.wait_until(response_deadline)) {
      throw PluginRuntimeFault(PluginRuntimeFaultKind::ResponseDeadline,
                               "plugin runtime response exit timed out");
    }
    const int wait_status = *process.child.wait_status();
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
      throw plugin_runtime_fault_from_wait_status(
          wait_status, "plugin runtime response completion");
    }
    if (!received.descriptors.empty()) {
      throw PluginRuntimeFault(
          PluginRuntimeFaultKind::BadOutput,
          "plugin runtime response unexpectedly carried descriptors",
          wait_status, 0);
    }
    if (SupervisorClock::now() >= response_deadline) {
      throw PluginRuntimeFault(PluginRuntimeFaultKind::ResponseDeadline,
                               "plugin runtime response decode timed out",
                               wait_status, 0);
    }
    const IsolatedCpuInvocationResponse response =
        decode_isolated_cpu_invocation_response(prepared.request,
                                                received.packet, limits);
    validate_host_capabilities_after_exit(prepared.request,
                                          prepared.capabilities);

    IsolatedCpuHostInvocationResult result;
    result.outcome = response.outcome;
    result.diagnostic = response.diagnostic;
    if (response.outcome == IsolatedCpuInvocationOutcome::Succeeded) {
      result.outputs =
          publish_host_outputs(invocation, response, prepared.capabilities);
    }
    if (SupervisorClock::now() >= response_deadline) {
      throw PluginRuntimeFault(PluginRuntimeFaultKind::ResponseDeadline,
                               "plugin runtime Host publication timed out",
                               wait_status, 0);
    }
    return result;
  } catch (const PluginRuntimeFault& fault) {
    throw_after_supervised_cleanup(fault, &process, options, pending_reap);
  } catch (const SupervisorDeadlineExpired&) {
    const PluginRuntimeFaultKind kind = supervised_deadline_fault_kind(phase);
    const char* message = "plugin runtime startup channel timed out";
    if (phase == SupervisedHostPhase::RequestTransfer) {
      message = "plugin runtime request transfer timed out";
    } else if (phase == SupervisedHostPhase::Invocation) {
      message = "plugin runtime invocation channel timed out";
    } else if (phase == SupervisedHostPhase::Response) {
      message = "plugin runtime response transfer timed out";
    }
    throw_after_supervised_cleanup(PluginRuntimeFault(kind, message), &process,
                                   options, pending_reap);
  } catch (const SupervisorDeadlineOverflow& error) {
    throw_after_supervised_deadline_overflow_cleanup(phase, error, &process,
                                                     options, pending_reap);
  } catch (const IsolatedCpuProtocolError& error) {
    const PluginRuntimeFaultKind kind =
        phase == SupervisedHostPhase::Response
            ? PluginRuntimeFaultKind::BadOutput
            : PluginRuntimeFaultKind::LifecycleProtocol;
    throw_after_supervised_cleanup(PluginRuntimeFault(kind, error.what()),
                                   &process, options, pending_reap);
  } catch (const PrematureFramedPacketEof& error) {
    throw_after_supervised_cleanup(
        plugin_runtime_fault_from_premature_response_eof(error, &process.child),
        &process, options, pending_reap);
  } catch (const IsolatedCpuInvocationError& error) {
    std::optional<int> status = process.child.wait_status();
    if (!status.has_value()) {
      try {
        const SupervisorDeadline observation_base =
            response_channel_observation_overflow_ready
                ? SupervisorDeadline::max()
                : SupervisorClock::now();
        status = observe_child_after_channel_close(
            &process.child,
            checked_supervisor_deadline(observation_base,
                                        std::chrono::milliseconds{20}));
      } catch (const SupervisorDeadlineOverflow& observation_error) {
        throw_after_supervised_deadline_overflow_cleanup(
            phase, observation_error, &process, options, pending_reap);
      } catch (...) {
        status = std::nullopt;
      }
    }
    if (status.has_value()) {
      throw_after_supervised_cleanup(
          plugin_runtime_fault_from_wait_status(
              *status, "plugin runtime channel failure"),
          &process, options, pending_reap);
    }
    throw_after_supervised_cleanup(
        PluginRuntimeFault(PluginRuntimeFaultKind::Channel, error.what()),
        &process, options, pending_reap);
  }
}

}  // namespace

/** @copydoc IsolatedCpuInvocationTestProbe::snapshot */
IsolatedCpuInvocationTestSnapshot
IsolatedCpuInvocationTestProbe::snapshot() noexcept {
  return IsolatedCpuInvocationTestSnapshot{
      g_host_capability_materialization_attempts.load(
          std::memory_order_relaxed),
      g_spawned_children.load(std::memory_order_relaxed),
      g_reaped_children.load(std::memory_order_relaxed),
      g_last_reaped_child.load(std::memory_order_relaxed),
      g_exact_frames_received.load(std::memory_order_relaxed),
      g_next_request_shutdown_acceptance_delay_ms.load(
          std::memory_order_acquire) > 0,
      g_next_request_transfer_post_acceptance_delay_ms.load(
          std::memory_order_acquire) > 0,
      g_next_response_channel_observation_overflow.load(
          std::memory_order_acquire),
      g_next_invocation_monitor_exit_hold.load(std::memory_order_acquire)};
}

/**
 * @copydoc IsolatedCpuInvocationTestProbe::
 * checked_supervisor_deadline_for_test
 */
std::chrono::steady_clock::time_point
IsolatedCpuInvocationTestProbe::checked_supervisor_deadline_for_test(
    std::chrono::steady_clock::time_point base,
    std::chrono::milliseconds duration) {
  return checked_supervisor_deadline(base, duration);
}

/** @copydoc IsolatedCpuInvocationTestProbe::delay_next_supervised_request_send
 */
void IsolatedCpuInvocationTestProbe::delay_next_supervised_request_send(
    std::chrono::milliseconds delay) {
  if (delay.count() < 0) {
    throw std::invalid_argument(
        "supervised request-send test delay must be nonnegative");
  }
  g_next_supervised_request_send_delay_ms.store(delay.count(),
                                                std::memory_order_release);
}

/**
 * @copydoc IsolatedCpuInvocationTestProbe::
 * delay_next_supervised_request_shutdown_acceptance
 */
void IsolatedCpuInvocationTestProbe::
    delay_next_supervised_request_shutdown_acceptance(  // NOLINT(whitespace/indent_namespace)
        std::chrono::milliseconds delay) {
  if (delay.count() < 0) {
    throw std::invalid_argument(
        "supervised request-shutdown acceptance test delay must be "
        "nonnegative");
  }
  g_next_request_shutdown_acceptance_delay_ms.store(delay.count(),
                                                    std::memory_order_release);
}

/**
 * @copydoc IsolatedCpuInvocationTestProbe::
 * delay_next_supervised_request_transfer_post_acceptance
 */
void IsolatedCpuInvocationTestProbe::
    delay_next_supervised_request_transfer_post_acceptance(  // NOLINT(whitespace/indent_namespace)
        std::chrono::milliseconds delay) {
  if (delay.count() < 0) {
    throw std::invalid_argument(
        "supervised request-transfer post-acceptance test delay must be "
        "nonnegative");
  }
  g_next_request_transfer_post_acceptance_delay_ms.store(
      delay.count(), std::memory_order_release);
}

/**
 * @copydoc IsolatedCpuInvocationTestProbe::
 * force_next_response_channel_observation_overflow
 */
void IsolatedCpuInvocationTestProbe::
    force_next_response_channel_observation_overflow(  // NOLINT(whitespace/indent_namespace)
        bool enabled) noexcept {
  g_next_response_channel_observation_overflow.store(enabled,
                                                     std::memory_order_release);
}

/**
 * @copydoc IsolatedCpuInvocationTestProbe::
 * delay_next_lifecycle_event_acceptance
 */
void IsolatedCpuInvocationTestProbe::delay_next_lifecycle_event_acceptance(
    SupervisedLifecycleTestEvent event, std::chrono::milliseconds delay) {
  if (delay.count() < 0) {
    throw std::invalid_argument(
        "supervised lifecycle-acceptance test delay must be nonnegative");
  }
  std::atomic<std::int64_t>* delay_slot = nullptr;
  switch (event) {
    case SupervisedLifecycleTestEvent::RuntimeStarted:
      delay_slot = &g_next_runtime_started_acceptance_delay_ms;
      break;
    case SupervisedLifecycleTestEvent::Heartbeat:
      delay_slot = &g_next_runtime_heartbeat_acceptance_delay_ms;
      break;
    case SupervisedLifecycleTestEvent::InvocationCompleted:
      delay_slot = &g_next_invocation_completed_acceptance_delay_ms;
      break;
    default:
      throw std::invalid_argument(
          "supervised lifecycle-acceptance test event is invalid");
  }
  delay_slot->store(delay.count(), std::memory_order_release);
}

/**
 * @copydoc IsolatedCpuInvocationTestProbe::
 * hold_next_invocation_monitor_until_child_exit
 */
void IsolatedCpuInvocationTestProbe::
    hold_next_invocation_monitor_until_child_exit(  // NOLINT(whitespace/indent_namespace)
        bool enabled) noexcept {
  g_next_invocation_monitor_exit_hold.store(enabled, std::memory_order_release);
}

/** @copydoc IsolatedCpuInvocationTestProbe::receive_one_packet */
void IsolatedCpuInvocationTestProbe::receive_one_packet(int socket) {
  static_cast<void>(receive_packet(socket));
}

/** @copydoc PluginRuntimeFault::PluginRuntimeFault */
PluginRuntimeFault::PluginRuntimeFault(
    PluginRuntimeFaultKind kind, const std::string& message,
    std::optional<int> wait_status, std::optional<int> exit_code,
    std::optional<int> signal_number,
    PluginRuntimeTerminationStage termination_stage,
    bool memory_pressure_compatible)
    : std::runtime_error(message),
      kind_(kind),
      wait_status_(wait_status),
      exit_code_(exit_code),
      signal_number_(signal_number),
      termination_stage_(termination_stage),
      memory_pressure_compatible_(memory_pressure_compatible &&
                                  signal_number.has_value() &&
                                  *signal_number == SIGKILL) {}

/**
 * @brief Address-stable serialized state for one source-private supervisor.
 * @throws std::bad_alloc when retained path state cannot allocate.
 * @note One mutex prevents overlapping child authority and protects restart
 * backoff plus deferred-reap quarantine state.
 */
class PluginRuntimeSupervisor::Impl final {
 public:
  /**
   * @brief Retains already validated immutable construction state.
   * @param runtime_executable Absolute diagnostic runtime path.
   * @param authorized_file Retained signed exact runtime capability.
   * @param ledger Attempt-local sole plugin resource authority.
   * @param invocation_resource_policy Validated admission/rlimit policy.
   * @param supervisor_options Validated lifecycle timing policy.
   * @param invocation_limits Validated protocol-v2 bounds.
   * @throws std::bad_alloc when retained path storage cannot allocate.
   */
  Impl(std::filesystem::path runtime_executable,
       AuthorizedPluginFile authorized_file,
       std::shared_ptr<ResourceLedger> ledger,
       PluginInvocationResourcePolicy invocation_resource_policy,
       PluginRuntimeSupervisorOptions supervisor_options,
       IsolatedCpuInvocationLimits invocation_limits)
      : executable(std::move(runtime_executable)),
        authorized_runtime(std::move(authorized_file)),
        resource_ledger(std::move(ledger)),
        resource_policy(invocation_resource_policy),
        options(supervisor_options),
        limits(invocation_limits) {}

  /** @brief Immutable absolute runtime path for diagnostics and argv[0]. */
  std::filesystem::path executable;
  /** @brief Immutable signed exact executable retained through every exec. */
  AuthorizedPluginFile authorized_runtime;
  /** @brief Attempt-local sole resource mint retained for every invocation. */
  std::shared_ptr<ResourceLedger> resource_ledger;
  /** @brief Immutable validated admission and child-limit policy. */
  PluginInvocationResourcePolicy resource_policy;
  /** @brief Immutable positive bounded lifecycle timing policy. */
  PluginRuntimeSupervisorOptions options;
  /** @brief Immutable protocol-v2 endpoint bounds. */
  IsolatedCpuInvocationLimits limits;
  /** @brief Serializes calls and all exact child/recovery authority. */
  std::mutex invocation_mutex;
  /** @brief Earliest launch time after a prior classified fault. */
  SupervisorDeadline next_launch_not_before = SupervisorDeadline::min();
  /** @brief Deferred exact-reap completion while the instance is quarantined.
   */
  std::shared_ptr<std::atomic<bool>> pending_reap;
};

// NOLINTBEGIN(whitespace/indent_namespace)
/**
 * @brief Validates and retains one bounded fresh-runtime supervision route.
 * @param runtime_executable Existing executable regular file.
 * @param resource_ledger Attempt-local sole resource-token mint.
 * @param resource_policy Positive admission and child-limit policy.
 * @param options Positive, ordered lifecycle bounds.
 * @param limits Protocol-v2 endpoint bounds.
 * @throws std::invalid_argument for a null resource ledger, invalid resource
 * policy, path, options, or limits.
 * @throws PluginTrustError when process trust initialization or exact-runtime
 * admission rejects the executable before any child/resource side effect.
 * @throws std::system_error when `SIGCHLD` state cannot be queried.
 * @throws std::filesystem::filesystem_error when Linux exact-object path
 * normalization fails.
 * @throws std::bad_alloc when path, trust, diagnostic, or private state cannot
 * allocate.
 * @throws Any other cached `PluginTrustPolicy::load` exception unchanged.
 * @note Construction authorizes and retains one immutable exact-runtime
 * capability and snapshot descriptor. It creates no child, session nonce,
 * invocation capability, or ledger token.
 */
PluginRuntimeSupervisor::PluginRuntimeSupervisor(
    std::filesystem::path runtime_executable,
    std::shared_ptr<ResourceLedger> resource_ledger,
    PluginInvocationResourcePolicy resource_policy,
    PluginRuntimeSupervisorOptions options,
    IsolatedCpuInvocationLimits limits) {
  if (!resource_ledger) {
    throw std::invalid_argument(
        "plugin runtime resource ledger must not be null");
  }
  validate_plugin_invocation_resource_policy(resource_policy);
  validate_plugin_runtime_supervisor_options(options);
  validate_isolated_cpu_invocation_limits(limits);
  const std::string executable = runtime_executable.string();
  if (executable.empty() || executable.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "plugin runtime executable path is empty or malformed");
  }
  AuthorizedPluginFile authorized_runtime = authorize_process_plugin(
      runtime_executable, PluginArtifactKind::IsolatedRuntime);
  runtime_executable = authorized_runtime.original_path();
  validate_sigchld_reaping_configuration();
  impl_ = std::make_unique<Impl>(
      std::move(runtime_executable), std::move(authorized_runtime),
      std::move(resource_ledger), resource_policy, options, limits);
}
// NOLINTEND

/**
 * @brief Destroys immutable/quarantine state without owning a live child.
 * @throws Nothing.
 * @note Per-call RAII retires or transfers exact PID ownership before return.
 */
PluginRuntimeSupervisor::~PluginRuntimeSupervisor() noexcept = default;

/**
 * @brief Serializes and executes one fresh supervised invocation.
 * @param invocation Host-owned identity, inputs, and exact output plans.
 * @return Complete callback outcome with fresh Values on success.
 * @throws PluginRuntimeFault for lifecycle/process/output or quarantine fault.
 * @throws std::overflow_error when the pre-spawn startup deadline cannot be
 * represented by the monotonic clock.
 * @throws IsolatedCpuProtocolError and public Value failures from Host
 * preflight/publication unchanged.
 * @note A classified failure arms only bounded restart backoff; no transport
 * fallback or child-state reuse occurs.
 */
IsolatedCpuHostInvocationResult PluginRuntimeSupervisor::invoke(
    const IsolatedCpuHostInvocation& invocation) {
  std::unique_lock<std::mutex> lock(impl_->invocation_mutex);
  if (impl_->pending_reap) {
    if (!impl_->pending_reap->load(std::memory_order_acquire)) {
      throw PluginRuntimeFault(
          PluginRuntimeFaultKind::ReapPending,
          "plugin runtime supervisor is quarantined pending exact reap");
    }
    impl_->pending_reap.reset();
  }
  const SupervisorDeadline now = SupervisorClock::now();
  if (now < impl_->next_launch_not_before) {
    std::this_thread::sleep_until(impl_->next_launch_not_before);
  }
  try {
    return run_supervised_plugin_invocation(
        impl_->authorized_runtime, *impl_->resource_ledger,
        impl_->resource_policy, impl_->options, impl_->limits, invocation,
        &impl_->pending_reap);
  } catch (const PluginRuntimeFault& fault) {
    try {
      impl_->next_launch_not_before = checked_supervisor_deadline(
          SupervisorClock::now(), impl_->options.restart_backoff);
    } catch (const SupervisorDeadlineOverflow& error) {
      throw PluginRuntimeFault(
          fault.kind(),
          std::string(fault.what()) +
              "; plugin runtime restart-backoff deadline arithmetic failed: " +
              error.what(),
          fault.wait_status(), fault.exit_code(), fault.signal_number(),
          fault.termination_stage(), fault.memory_pressure_compatible());
    }
    throw;
  }
}

/** @copydoc PluginRuntimeSupervisor::runtime_executable */
const std::filesystem::path& PluginRuntimeSupervisor::runtime_executable()
    const noexcept {
  return impl_->executable;
}

/** @copydoc PluginRuntimeSupervisor::options */
PluginRuntimeSupervisorOptions PluginRuntimeSupervisor::options()
    const noexcept {
  return impl_->options;
}

/** @copydoc PluginRuntimeSupervisor::limits */
IsolatedCpuInvocationLimits PluginRuntimeSupervisor::limits() const noexcept {
  return impl_->limits;
}

/** @copydoc PluginRuntimeSupervisor::package_identity */
PluginPackageIdentity PluginRuntimeSupervisor::package_identity()
    const noexcept {
  return impl_->authorized_runtime.package_identity();
}

// NOLINTBEGIN(whitespace/indent_namespace)
/**
 * @brief Constructs the sole supervised route selected by this executor.
 * @param runtime_executable Existing executable regular file.
 * @param resource_ledger Attempt-local sole resource-token mint.
 * @param resource_policy Positive admission and child-limit policy.
 * @param options Positive lifecycle timing policy.
 * @param limits Protocol-v2 endpoint bounds.
 * @throws std::invalid_argument for invalid path, resource authority/policy,
 * supervisor options, endpoint limits, or auto-reaping `SIGCHLD` state.
 * @throws PluginTrustError from immutable process trust initialization or
 * exact-runtime admission unchanged.
 * @throws std::system_error when `SIGCHLD` state cannot be queried.
 * @throws std::filesystem::filesystem_error from Linux exact-object path
 * normalization unchanged.
 * @throws std::bad_alloc from path, trust, diagnostic, or supervisor state
 * allocation unchanged.
 * @throws Any other cached `PluginTrustPolicy::load` exception unchanged.
 */
PluginInvocationExecutor::PluginInvocationExecutor(
    std::filesystem::path runtime_executable,
    std::shared_ptr<ResourceLedger> resource_ledger,
    PluginInvocationResourcePolicy resource_policy,
    PluginRuntimeSupervisorOptions options, IsolatedCpuInvocationLimits limits)
    : supervisor_(std::move(runtime_executable), std::move(resource_ledger),
                  resource_policy, options, limits) {}
// NOLINTEND

/**
 * @brief Invokes only the owned supervised route.
 * @param invocation Host-owned invocation plan.
 * @return Complete Host result from the supervisor.
 * @throws All supervisor, preflight, and publication failures unchanged.
 * @note No direct non-supervised adapter exists in this selection path.
 */
IsolatedCpuHostInvocationResult PluginInvocationExecutor::invoke(
    const IsolatedCpuHostInvocation& invocation) {
  return supervisor_.invoke(invocation);
}

/** @copydoc PluginInvocationExecutor::package_identity */
PluginPackageIdentity PluginInvocationExecutor::package_identity()
    const noexcept {
  return supervisor_.package_identity();
}

// NOLINTBEGIN(whitespace/indent_namespace)
/**
 * @brief Validates and retains the one-shot runtime path and protocol limits.
 * @param runtime_executable Existing executable regular file launched later
 * with an empty environment and fixed control descriptor.
 * @param resource_ledger Attempt-local sole resource-token mint.
 * @param resource_policy Positive admission and child-limit policy.
 * @param limits Protocol-v2 bounds; only the parameter limit may be zero.
 * @throws std::invalid_argument for invalid path, resource authority/policy,
 * endpoint limits, or auto-reaping `SIGCHLD` state.
 * @throws PluginTrustError when process trust initialization or exact-runtime
 * admission rejects the executable before any child/resource side effect.
 * @throws std::system_error when the process-wide SIGCHLD action cannot be
 * queried.
 * @throws std::filesystem::filesystem_error when Linux exact-object path
 * normalization fails.
 * @throws std::bad_alloc when retained path, trust, or diagnostic storage
 * cannot allocate.
 * @throws Any other cached `PluginTrustPolicy::load` exception unchanged.
 * @note Construction authorizes and retains one immutable exact-runtime
 * capability and snapshot descriptor. It creates no child, invocation-data
 * descriptor or mapping, or ledger token.
 */
NonSupervisedIsolatedCpuInvocationExecutor::
    NonSupervisedIsolatedCpuInvocationExecutor(
        std::filesystem::path runtime_executable,
        std::shared_ptr<ResourceLedger> resource_ledger,
        PluginInvocationResourcePolicy resource_policy,
        IsolatedCpuInvocationLimits limits)
    : runtime_executable_(std::move(runtime_executable)),
      resource_ledger_(
          require_plugin_resource_ledger(std::move(resource_ledger))),
      resource_policy_(
          require_plugin_invocation_resource_policy(resource_policy)),
      authorized_runtime_(authorize_process_plugin(
          runtime_executable_, PluginArtifactKind::IsolatedRuntime)),
      limits_(limits) {
  validate_isolated_cpu_invocation_limits(limits_);
  const std::string executable = runtime_executable_.string();
  if (executable.empty() || executable.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "isolated CPU runtime executable path is empty or malformed");
  }
  runtime_executable_ = authorized_runtime_.original_path();
  validate_sigchld_reaping_configuration();
}
// NOLINTEND

/**
 * @brief Runs validated Host preflight, one fresh runtime, and post-exit output
 * adoption synchronously.
 * @param invocation Host-owned identity, scalar metadata, Ready inputs, and
 * exact output plans.
 * @return Typed callback result with fresh Host Values only after success.
 * @throws IsolatedCpuProtocolError for invalid local, wire, or returned state.
 * @throws IsolatedCpuInvocationError for capability, process, channel, or exit
 * failure.
 * @throws Value/readiness/access/allocation errors from input inspection or
 * output publication.
 * @note Complete preflight and canonical encoding precede every invocation-
 * capability shm/FD/mmap/fork effect. Cleanup owns and reaps the exact child
 * without a bounded deadline.
 */
IsolatedCpuHostInvocationResult
NonSupervisedIsolatedCpuInvocationExecutor::invoke(
    const IsolatedCpuHostInvocation& invocation) const {
  std::optional<ResourceLedger::PluginResourceLease> resource_lease;
  PreparedHostInvocation prepared = admit_and_materialize_host_invocation(
      invocation, limits_, authorized_runtime_, *resource_ledger_,
      resource_policy_, &resource_lease);
  std::vector<int> descriptors;
  descriptors.reserve(prepared.capabilities.size());
  for (const MappedCapability& capability : prepared.capabilities) {
    descriptors.push_back(capability.descriptor.get());
  }

  SpawnedChild process = spawn_runtime(
      authorized_runtime_, resource_lease->resources(), resource_policy_);
  send_packet(process.control.get(), prepared.request_packet, descriptors);
  ReceivedPacket received = receive_packet(process.control.get());
  process.control.reset();
  process.child.wait_for_normal_exit();
  if (!received.descriptors.empty()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU response unexpectedly carried descriptors");
  }
  const IsolatedCpuInvocationResponse response =
      decode_isolated_cpu_invocation_response(prepared.request, received.packet,
                                              limits_);
  validate_host_capabilities_after_exit(prepared.request,
                                        prepared.capabilities);

  IsolatedCpuHostInvocationResult result;
  result.outcome = response.outcome;
  result.diagnostic = response.diagnostic;
  if (response.outcome == IsolatedCpuInvocationOutcome::Succeeded) {
    result.outputs =
        publish_host_outputs(invocation, response, prepared.capabilities);
  }
  return result;
}

/**
 * @brief Receives one EOF-terminated request, executes one callback, and sends
 * one EOF-terminated response.
 * @param control_fd Connected framed Unix stream descriptor borrowed for this
 * process lifetime.
 * @param limits Runtime-local protocol bounds.
 * @param callback Nonempty process-local CPU callback.
 * @return Zero after a valid response send, two for invalid local arguments,
 * and one for every contained request, mapping, callback-response, or channel
 * failure.
 * @throws Nothing; every exception is contained and converted to a status.
 * @note Decode and mapping occur only after the exact request frame is followed
 * by peer write-half EOF. This endpoint has no receive or callback deadline.
 */
int serve_non_supervised_isolated_cpu_invocation_once(
    int control_fd, const IsolatedCpuInvocationLimits& limits,
    const IsolatedCpuRuntimeCallback& callback) noexcept {
  try {
    if (control_fd < 0 || !callback) {
      return 2;
    }
    validate_isolated_cpu_invocation_limits(limits);
    configure_socket(control_fd);
    ReceivedPacket received = receive_packet(control_fd);
    const IsolatedCpuInvocationRequest request =
        decode_isolated_cpu_invocation_request(received.packet, limits);
    std::vector<MappedCapability> mappings =
        map_received_capabilities(request, &received.descriptors);
    validate_runtime_input_bindings(request, mappings);
    const IsolatedCpuInvocationResponse response =
        execute_runtime_callback(request, mappings, callback, limits);
    const std::vector<std::byte> response_packet =
        encode_isolated_cpu_invocation_response(request, response, limits);
    send_packet(control_fd, response_packet, {});
    return 0;
  } catch (...) {
    return 1;
  }
}

/**
 * @brief Serves one nonce-bound invocation with independent heartbeat events.
 * @param control_fd Connected #102 framed stream, normally fixed fd 3.
 * @param supervision_fd Connected lifecycle datagram socket, normally fd 5.
 * @param limits Runtime-local protocol-v2 hard bounds.
 * @param callback Nonempty process-local callback.
 * @param startup_behavior Deterministic startup behavior for maintained
 * fail-closed fixtures.
 * @param lifecycle_hook Optional process-local callback-adjacent
 * instrumentation.
 * @return Zero after one exact response, two for invalid local arguments, and
 * one for contained protocol, mapping, callback-adjacent, or channel failure.
 * @throws Nothing; every exception remains inside the fresh runtime process.
 * @note The endpoint authenticates only its private launch/session. It grants
 * no package trust, sandbox, quota, or hostile-code attestation.
 */
int serve_supervised_isolated_cpu_invocation_once(
    int control_fd, int supervision_fd,
    const IsolatedCpuInvocationLimits& limits,
    const IsolatedCpuRuntimeCallback& callback,
    PluginRuntimeEndpointStartupBehavior startup_behavior,
    const PluginRuntimeLifecycleHook& lifecycle_hook) noexcept {
  try {
    if (control_fd < 0 || supervision_fd < 0 || !callback) {
      return 2;
    }
    validate_isolated_cpu_invocation_limits(limits);
    configure_socket(control_fd);
    configure_socket(supervision_fd);
    const PluginRuntimeLifecycleFrame hello =
        receive_plugin_runtime_hello_blocking(supervision_fd);
    RuntimeHeartbeatEmitter heartbeat(supervision_fd, hello);
    if (startup_behavior ==
        PluginRuntimeEndpointStartupBehavior::SuppressStarted) {
      for (;;) {
        static_cast<void>(::pause());
      }
    }
    const bool corrupt_started_nonce =
        startup_behavior ==
        PluginRuntimeEndpointStartupBehavior::CorruptStartedNonce;
    heartbeat.send_started(corrupt_started_nonce);
    if (corrupt_started_nonce) {
      for (;;) {
        static_cast<void>(::pause());
      }
    }
    if (startup_behavior != PluginRuntimeEndpointStartupBehavior::Normal) {
      return 2;
    }
    heartbeat.start();

    ReceivedPacket received = receive_packet(control_fd);
    const IsolatedCpuInvocationRequest request =
        decode_isolated_cpu_invocation_request(received.packet, limits);
    if (!(request.identity == hello.identity)) {
      throw IsolatedCpuProtocolError(
          "plugin runtime hello and request identities differ");
    }
    std::vector<MappedCapability> mappings =
        map_received_capabilities(request, &received.descriptors);
    validate_runtime_input_bindings(request, mappings);
    const IsolatedCpuInvocationResponse response =
        execute_runtime_callback(request, mappings, callback, limits);
    const IsolatedCpuRuntimeInvocation lifecycle_invocation =
        build_runtime_invocation(request, mappings);
    if (lifecycle_hook) {
      lifecycle_hook(PluginRuntimeLifecyclePoint::BeforeInvocationCompleted,
                     lifecycle_invocation);
    }
    const std::vector<std::byte> response_packet =
        encode_isolated_cpu_invocation_response(request, response, limits);
    heartbeat.stop();
    heartbeat.send_completed();
    if (lifecycle_hook) {
      lifecycle_hook(PluginRuntimeLifecyclePoint::BeforeResponse,
                     lifecycle_invocation);
    }
    send_packet(control_fd, response_packet, {});
    return 0;
  } catch (...) {
    return 1;
  }
}

}  // namespace ps::execution
