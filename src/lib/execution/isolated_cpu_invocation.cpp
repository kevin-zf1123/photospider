/**
 * @file isolated_cpu_invocation.cpp
 * @brief Implements the non-supervised shared-memory CPU invocation vertical.
 */
#include "execution/isolated_cpu_invocation.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ps::execution {
namespace {

/** @brief Fixed close-on-exec child setup-status descriptor. */
constexpr int kIsolatedCpuRuntimeSetupDescriptor = 4;
/** @brief First descriptor closed during child exec preparation. */
constexpr int kFirstClosedChildDescriptor = 5;
/** @brief Fixed capability header width before one tensor payload range. */
constexpr std::size_t kCapabilityHeaderBytes = 40U;
/** @brief Capability header magic spelling ASCII `PSC1`. */
constexpr std::uint32_t kCapabilityHeaderMagic = 0x50534331U;
/** @brief Exact capability header structural version. */
constexpr std::uint16_t kCapabilityHeaderVersion = 1U;

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
  /** @brief Darwin kernel-exclusive descriptor ceiling; unused on Linux. */
  int darwin_exclusive_maximum = 0;
};

/** @brief Monotonic process-local suffix for collision-resistant `O_EXCL`. */
std::atomic<std::uint64_t> g_shared_memory_sequence{1U};

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

/**
 * @brief Streams one bounded frame and its ordered descriptor capabilities.
 * @param socket Connected blocking Unix stream socket.
 * @param packet Nonempty canonical request or response packet.
 * @param descriptors Ordered descriptors installed with `SCM_RIGHTS`.
 * @return Nothing after the frame and rights are completely sent.
 * @throws IsolatedCpuInvocationError for a channel or zero-progress send.
 * @throws IsolatedCpuProtocolError when local packet/descriptor bounds fail.
 * @note Descriptor ownership remains with the caller; `SCM_RIGHTS` accompanies
 * only the first nonempty stream segment and later segments carry bytes only.
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
}

/**
 * @brief Assembles one bounded stream frame and owns all installed FDs.
 * @param socket Connected blocking Unix stream socket.
 * @return Exact framed bytes and RAII-owned ancillary descriptors.
 * @throws IsolatedCpuInvocationError for EOF or a channel-system failure.
 * @throws IsolatedCpuProtocolError for truncation, malformed control data, or
 * excessive packet/descriptor counts.
 * @throws std::bad_alloc only before `recvmsg` installs descriptor rights.
 * @note Storage is fully reserved before receiving, and `SCM_RIGHTS` is valid
 * only on the first nonempty stream segment. Later segments carry bytes only.
 */
ReceivedPacket receive_packet(int socket) {
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
  while (expected_bytes == 0U || received_bytes < expected_bytes) {
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
      throw IsolatedCpuInvocationError(
          std::string("isolated CPU framed stream receive failed: ") +
          std::strerror(errno));
    }
    if (count == 0) {
      throw IsolatedCpuInvocationError(
          "isolated CPU channel closed before its framed packet completed");
    }

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

    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
      throw IsolatedCpuProtocolError(
          "isolated CPU packet or ancillary data was truncated");
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
 * @brief Closes every inherited child descriptor except exact fd 0 through 4.
 * @param plan Platform closure input prepared before fork.
 * @return Zero after complete closure, otherwise a positive setup errno.
 * @throws Nothing; only async-signal-safe operations run after fork.
 * @note Darwin scans its kernel descriptor ceiling; Linux uses the unbounded
 * raw `close_range` syscall and fails closed when unavailable.
 */
int close_child_descriptors(const ChildDescriptorClosurePlan& plan) noexcept {
#if defined(__APPLE__)
  for (int descriptor = kFirstClosedChildDescriptor;
       descriptor < plan.darwin_exclusive_maximum; ++descriptor) {
    if (::close(descriptor) == 0 || errno == EBADF) {
      continue;
    }
    return errno == 0 ? EIO : errno;
  }
  return 0;
#elif defined(__linux__)
  static_cast<void>(plan);
#if defined(SYS_close_range)
  const auto result = ::syscall(
      SYS_close_range, static_cast<unsigned int>(kFirstClosedChildDescriptor),
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
 * @return Darwin kernel ceiling or empty Linux plan.
 * @throws std::system_error when the platform query fails or is unsupported.
 */
ChildDescriptorClosurePlan prepare_child_descriptor_closure() {
#if defined(__APPLE__)
  int maximum_descriptor = 0;
  std::size_t result_size = sizeof(maximum_descriptor);
  if (::sysctlbyname("kern.maxfilesperproc", &maximum_descriptor, &result_size,
                     nullptr, 0U) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "query isolated CPU descriptor ceiling");
  }
  if (result_size != sizeof(maximum_descriptor) ||
      maximum_descriptor <= kIsolatedCpuRuntimeSetupDescriptor) {
    throw std::system_error(EIO, std::generic_category(),
                            "invalid isolated CPU descriptor ceiling");
  }
  return ChildDescriptorClosurePlan{maximum_descriptor};
#elif defined(__linux__)
  return ChildDescriptorClosurePlan{};
#else
  throw std::system_error(ENOSYS, std::generic_category(),
                          "isolated CPU descriptor closure is unsupported");
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
 * bounded deadline; #103 owns supervised escalation and hang classification.
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
 * @brief Forks and execs one fresh runtime with fd 3 and an empty environment.
 * @param runtime_executable Operability-validated executable path.
 * @return Sole child and parent socket owners after successful exec.
 * @throws IsolatedCpuInvocationError for socket, pipe, `/dev/null`, fork, or
 * child setup/exec failures.
 * @throws std::system_error from authoritative descriptor-ceiling inspection.
 * @note The child inherits no descriptor above fd 4 and no parent environment.
 */
SpawnedChild spawn_runtime(const std::filesystem::path& runtime_executable) {
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

  const ChildDescriptorClosurePlan closure = prepare_child_descriptor_closure();
  const std::string executable = runtime_executable.string();
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
                                     kFirstClosedChildDescriptor);
    if (control_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int status_copy = ::fcntl(status_write.get(), F_DUPFD_CLOEXEC,
                                    kFirstClosedChildDescriptor);
    if (status_copy < 0) {
      child_setup_failed(status_write.get(), errno);
    }
    const int null_copy = ::fcntl(null_device.get(), F_DUPFD_CLOEXEC,
                                  kFirstClosedChildDescriptor);
    if (null_copy < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::dup2(null_copy, STDIN_FILENO) < 0 ||
        ::dup2(null_copy, STDOUT_FILENO) < 0 ||
        ::dup2(null_copy, STDERR_FILENO) < 0 ||
        ::dup2(control_copy, kIsolatedCpuRuntimeControlDescriptor) < 0 ||
        ::dup2(status_copy, kIsolatedCpuRuntimeSetupDescriptor) < 0) {
      child_setup_failed(status_copy, errno);
    }
    if (::fcntl(kIsolatedCpuRuntimeControlDescriptor, F_SETFD, 0) < 0 ||
        ::fcntl(kIsolatedCpuRuntimeSetupDescriptor, F_SETFD, FD_CLOEXEC) < 0) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, errno);
    }
    const int close_error = close_child_descriptors(closure);
    if (close_error != 0) {
      child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, close_error);
    }
    char* const arguments[] = {const_cast<char*>(executable_pointer), nullptr};
    ::execve(executable_pointer, arguments, empty_environment);
    child_setup_failed(kIsolatedCpuRuntimeSetupDescriptor, errno);
  }

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
 * @brief Converts one optional public image facet into bounded wire axes.
 * @param facet Optional facet from a public Value or output plan.
 * @return Exact optional wire facet.
 * @throws IsolatedCpuProtocolError when an axis exceeds uint32 representation.
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
  return wire;
}

/**
 * @brief Converts one public whole-byte DenseTensor descriptor into wire facts.
 * @param descriptor Public logical descriptor.
 * @param image_facet Optional public image axes.
 * @param layout Public signed whole-byte layout.
 * @return Structural wire descriptor without capability/phase/binding fields.
 * @throws IsolatedCpuProtocolError for unsupported quantization, encoding, or
 * local integer representation.
 * @throws std::invalid_argument from public scalar-width validation.
 * @throws std::bad_alloc when bounded vector copies cannot allocate.
 */
IsolatedCpuTensorDescriptor to_wire_tensor_descriptor(
    const DenseTensorDescriptor& descriptor,
    const std::optional<ImageFacet>& image_facet, const StridedLayout& layout) {
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
 * @brief Complete Host-side request and shared-memory owner set.
 * @throws Nothing for moves and destruction.
 */
struct PreparedHostInvocation final {
  /** @brief Validated canonical request retained for response comparison. */
  IsolatedCpuInvocationRequest request;
  /** @brief Ordered invocation-local FDs and mappings. */
  std::vector<MappedCapability> capabilities;
};

/**
 * @brief Converts and copies one high-level Host request into shared memory.
 * @param invocation Host input Values and exact output plans.
 * @param limits Retained local hard limits.
 * @return Validated request plus sole local capability owners.
 * @throws IsolatedCpuProtocolError for unsupported or inconsistent state.
 * @throws IsolatedCpuInvocationError for shared-memory preparation failure.
 * @throws Value/readiness/access/allocation errors from public Value APIs.
 * @throws std::bad_alloc when bounded request or mapping storage cannot
 * allocate.
 * @note BufferHandle and ReadLease identities remain local and are retired
 * immediately after each immutable input has been copied.
 */
PreparedHostInvocation prepare_host_invocation(
    const IsolatedCpuHostInvocation& invocation,
    const IsolatedCpuInvocationLimits& limits) {
  validate_isolated_cpu_invocation_limits(limits);
  if (invocation.outputs.empty()) {
    throw IsolatedCpuProtocolError(
        "isolated CPU invocation requires at least one output");
  }
  if (invocation.inputs.size() >
          std::numeric_limits<std::size_t>::max() - invocation.outputs.size() ||
      invocation.inputs.size() + invocation.outputs.size() >
          kMaximumIsolatedCpuDescriptors ||
      invocation.inputs.size() + invocation.outputs.size() >
          limits.maximum_descriptors ||
      invocation.inputs.size() + invocation.outputs.size() >
          limits.maximum_capabilities ||
      invocation.parameters.size() > limits.maximum_parameters) {
    throw IsolatedCpuProtocolError(
        "isolated CPU invocation tensor count exceeds its bound");
  }

  PreparedHostInvocation prepared;
  IsolatedCpuInvocationRequest& request = prepared.request;
  request.identity = invocation.identity;
  request.operation = invocation.operation;
  request.parameters = invocation.parameters;
  request.input_count = static_cast<std::uint32_t>(invocation.inputs.size());
  request.output_count = static_cast<std::uint32_t>(invocation.outputs.size());
  const std::size_t total_count =
      invocation.inputs.size() + invocation.outputs.size();
  request.capabilities.reserve(total_count);
  request.tensors.reserve(total_count);
  prepared.capabilities.reserve(total_count);

  std::uint64_t aggregate_shared_bytes = 0U;
  std::uint64_t next_capability_id = 1U;
  const auto add_shared_bytes = [&aggregate_shared_bytes,
                                 &limits](std::uint64_t byte_size) {
    if (aggregate_shared_bytes >
        std::numeric_limits<std::uint64_t>::max() - byte_size) {
      throw IsolatedCpuProtocolError(
          "isolated CPU aggregate shared-memory bytes overflow");
    }
    aggregate_shared_bytes += byte_size;
    if (aggregate_shared_bytes > limits.maximum_shared_memory_bytes ||
        aggregate_shared_bytes > kMaximumIsolatedCpuSharedBytes) {
      throw IsolatedCpuProtocolError(
          "isolated CPU aggregate shared-memory bytes exceed their bound");
    }
  };

  for (const Value& value : invocation.inputs) {
    if (!value.valid() ||
        value.representation_kind() != ValueRepresentationKind::DenseTensor ||
        value.storage_layout_kind() != StorageLayoutKind::Strided) {
      throw IsolatedCpuProtocolError(
          "isolated CPU input is not a Strided DenseTensor Value");
    }
    DenseTensorView view(value);
    ReadLease lease = value.buffer_handle().acquire_read();
    if (lease.size() != view.storage_size()) {
      throw IsolatedCpuProtocolError(
          "isolated CPU input Value range size is inconsistent");
    }
    IsolatedCpuCapability capability;
    capability.capability_id = next_capability_id++;
    capability.access = IsolatedCpuCapabilityAccess::ReadOnly;
    capability.byte_size = complete_capability_size(lease.size());

    IsolatedCpuTensorDescriptor tensor = to_wire_tensor_descriptor(
        view.descriptor(), value.image_facet(), view.layout());
    tensor.access = IsolatedCpuTensorAccess::InputReadOnly;
    tensor.readiness = IsolatedCpuTensorReadiness::ReadyInput;
    tensor.ownership = IsolatedCpuTensorOwnership::HostInput;
    tensor.capability_id = capability.capability_id;
    tensor.capability_offset = kCapabilityHeaderBytes;
    tensor.capability_length = static_cast<std::uint64_t>(lease.size());

    add_shared_bytes(capability.byte_size);
    MappedCapability mapping = prepare_capability(request.identity, capability,
                                                  lease.data(), lease.size());
    tensor.content_binding = compute_isolated_cpu_content_binding(
        request.identity, tensor,
        mapping.mapping.data() + kCapabilityHeaderBytes, lease.size());
    request.capabilities.push_back(capability);
    request.tensors.push_back(std::move(tensor));
    prepared.capabilities.push_back(std::move(mapping));
  }

  for (const IsolatedCpuDenseTensorOutputPlan& plan : invocation.outputs) {
    IsolatedCpuCapability capability;
    capability.capability_id = next_capability_id++;
    capability.access = IsolatedCpuCapabilityAccess::ReadWrite;
    capability.byte_size = complete_capability_size(plan.storage_size);

    IsolatedCpuTensorDescriptor tensor = to_wire_tensor_descriptor(
        plan.descriptor, plan.image_facet, plan.layout);
    tensor.access = IsolatedCpuTensorAccess::OutputWriteOnly;
    tensor.readiness = IsolatedCpuTensorReadiness::WritableOutput;
    tensor.ownership = IsolatedCpuTensorOwnership::RuntimeOutput;
    tensor.capability_id = capability.capability_id;
    tensor.capability_offset = kCapabilityHeaderBytes;
    tensor.capability_length = static_cast<std::uint64_t>(plan.storage_size);

    add_shared_bytes(capability.byte_size);
    MappedCapability mapping = prepare_capability(request.identity, capability,
                                                  nullptr, plan.storage_size);
    request.capabilities.push_back(capability);
    request.tensors.push_back(std::move(tensor));
    prepared.capabilities.push_back(std::move(mapping));
  }

  request.resources.shared_memory_bytes = aggregate_shared_bytes;
  request.resources.descriptor_count =
      static_cast<std::uint32_t>(request.tensors.size());
  request.resources.cpu_slots = 1U;
  validate_isolated_cpu_invocation_request(request, limits);
  return prepared;
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
  invocation.parameters = request.parameters;
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
 * @throws std::bad_alloc when output or Value storage cannot allocate.
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

}  // namespace

// NOLINTBEGIN(whitespace/indent_namespace)
NonSupervisedIsolatedCpuInvocationExecutor::
    NonSupervisedIsolatedCpuInvocationExecutor(
        std::filesystem::path runtime_executable,
        IsolatedCpuInvocationLimits limits)
    : runtime_executable_(std::move(runtime_executable)), limits_(limits) {
  validate_isolated_cpu_invocation_limits(limits_);
  const std::string executable = runtime_executable_.string();
  if (executable.empty() || executable.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "isolated CPU runtime executable path is empty or malformed");
  }
  struct stat status{};
  if (::stat(executable.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
      ::access(executable.c_str(), X_OK) != 0) {
    throw std::invalid_argument(
        "isolated CPU runtime path is not an executable regular file");
  }
  validate_sigchld_reaping_configuration();
}
// NOLINTEND

IsolatedCpuHostInvocationResult
NonSupervisedIsolatedCpuInvocationExecutor::invoke(
    const IsolatedCpuHostInvocation& invocation) const {
  PreparedHostInvocation prepared =
      prepare_host_invocation(invocation, limits_);
  const std::vector<std::byte> request_packet =
      encode_isolated_cpu_invocation_request(prepared.request, limits_);
  std::vector<int> descriptors;
  descriptors.reserve(prepared.capabilities.size());
  for (const MappedCapability& capability : prepared.capabilities) {
    descriptors.push_back(capability.descriptor.get());
  }

  SpawnedChild process = spawn_runtime(runtime_executable_);
  send_packet(process.control.get(), request_packet, descriptors);
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

}  // namespace ps::execution
