/**
 * @file b1_output_store.cpp
 * @brief Implements process-I/O-backed crash-durable B1 artifact commits.
 */
#include "benchmark/b1_output_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "core/value_image_adapter.hpp"  // NOLINT(build/include_subdir)

namespace ps::benchmark {
namespace {

/** @brief Exact committed payload leaf. */
constexpr char kPayloadName[] = "output.rgba32le";

/** @brief Exact manifest leaf published last. */
constexpr char kManifestName[] = "manifest.txt";

/** @brief Private manifest leaf removed after no-replace publication. */
constexpr char kPrivateManifestName[] = ".manifest.private";

/**
 * @brief Typed internal failure for an unavailable durability primitive.
 * @throws Nothing beyond standard runtime-error string allocation.
 */
class B1DurabilityError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one typed durability failure.
   * @param message Complete diagnostic.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  explicit B1DurabilityError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Typed internal failure for reopened byte or identity mismatch.
 * @throws Nothing beyond standard runtime-error string allocation.
 */
class B1RevalidationError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one typed revalidation failure.
   * @param message Complete diagnostic.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  explicit B1RevalidationError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Typed internal failure when the selected root path loses fd identity.
 * @throws Nothing beyond standard runtime-error string allocation.
 */
class B1RootBindingError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one typed root-binding failure.
   * @param message Complete diagnostic.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  explicit B1RootBindingError(const std::string& message)
      : std::runtime_error(message) {}
};

/**
 * @brief Validates the exact frozen candidate descriptor.
 * @param image Candidate image.
 * @return Nothing for exact CPU FP32 RGBA B1 state.
 * @throws std::invalid_argument for descriptor drift.
 */
void validate_b1_candidate_image(const ImageBuffer& image) {
  validate_image_buffer(image);
  if (image.width != static_cast<int>(kB1ImageEdge) ||
      image.height != static_cast<int>(kB1ImageEdge) ||
      image.channels != static_cast<int>(kB1ChannelCount) ||
      image.type != DataType::FLOAT32 || image.device != Device::CPU ||
      image_buffer_row_bytes(image) != kB1PayloadRowBytes) {
    throw std::invalid_argument("B1 candidate image descriptor drifted.");
  }
}

/**
 * @brief Invokes the optional deterministic source-private test boundary.
 * @param options Store policy carrying the hook and borrowed context.
 * @param point Exact boundary reached by the production flow.
 * @return Nothing when no hook exists or the hook elects to continue.
 * @throws Any exception selected by the test hook.
 */
void invoke_fault_injector(const B1OutputStoreOptions& options,
                           B1OutputStoreFaultPoint point) {
  if (options.fault_injector != nullptr) {
    options.fault_injector(options.fault_injector_context, point);
  }
}

#if !defined(_WIN32)

/**
 * @brief Throws a system error retaining the current `errno`.
 * @param operation Exact failed operation.
 * @return Nothing.
 * @throws std::system_error always.
 */
[[noreturn]] void throw_errno(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}

/**
 * @brief Owns one POSIX descriptor through exception-safe checked close.
 * @throws Nothing for construction; destruction is fail-stop on close failure.
 */
class ScopedFileDescriptor final {
 public:
  /**
   * @brief Adopts one open descriptor.
   * @param descriptor Nonnegative descriptor.
   * @throws std::invalid_argument for a negative descriptor.
   */
  explicit ScopedFileDescriptor(int descriptor) : descriptor_(descriptor) {
    if (descriptor_ < 0) {
      throw std::invalid_argument(
          "B1 descriptor owner received a negative fd.");
    }
  }

  /**
   * @brief Closes an unreleased descriptor.
   * @throws Nothing; close failure terminates to avoid a false durability fact.
   */
  ~ScopedFileDescriptor() noexcept {
    if (descriptor_ >= 0 && ::close(descriptor_) != 0) {
      std::terminate();
    }
  }

  /** @brief Descriptor ownership cannot be copied. */
  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;

  /** @brief Descriptor ownership cannot be assigned. */
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  /**
   * @brief Returns the borrowed descriptor.
   * @return Nonnegative owned descriptor.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

  /**
   * @brief Relinquishes ownership without closing the descriptor.
   * @return Previously owned nonnegative descriptor.
   * @throws Nothing.
   */
  int release() noexcept {
    const int descriptor = descriptor_;
    descriptor_ = -1;
    return descriptor;
  }

  /**
   * @brief Performs checked close and relinquishes ownership.
   * @return Nothing.
   * @throws std::system_error when close fails.
   */
  void close() {
    const int descriptor = release();
    if (::close(descriptor) != 0) {
      throw_errno("close B1 artifact");
    }
  }

 private:
  /** @brief Owned descriptor or negative released sentinel. */
  int descriptor_ = -1;
};

/**
 * @brief Stable POSIX filesystem-object identity retained across reopen.
 * @throws Nothing for value construction and comparison.
 */
struct B1FileIdentity final {
  /** @brief Device containing the object. */
  std::uint64_t device = 0U;
  /** @brief Inode within the containing device. */
  std::uint64_t inode = 0U;

  /**
   * @brief Compares exact device/inode identity.
   * @param other Candidate identity.
   * @return True only when both scalar components match.
   * @throws Nothing.
   */
  bool operator==(const B1FileIdentity& other) const noexcept {
    return device == other.device && inode == other.inode;
  }

  /**
   * @brief Detects device/inode identity drift.
   * @param other Candidate identity.
   * @return True when either scalar component differs.
   * @throws Nothing.
   */
  bool operator!=(const B1FileIdentity& other) const noexcept {
    return !(*this == other);
  }
};

/**
 * @brief Converts one successful `stat` record to stable scalar identity.
 * @param value Native stat record.
 * @return Device/inode pair.
 * @throws Nothing.
 */
B1FileIdentity file_identity(const struct stat& value) noexcept {
  return B1FileIdentity{static_cast<std::uint64_t>(value.st_dev),
                        static_cast<std::uint64_t>(value.st_ino)};
}

/**
 * @brief Reads and validates one descriptor as a regular file.
 * @param descriptor Open descriptor.
 * @param operation Diagnostic operation name for `fstat` failure.
 * @return Complete native stat record.
 * @throws std::system_error when `fstat` fails.
 * @throws B1RevalidationError when the descriptor is not a regular file.
 */
struct stat regular_file_stat(int descriptor, const char* operation) {
  struct stat value{};
  if (::fstat(descriptor, &value) != 0) {
    throw_errno(operation);
  }
  if (!S_ISREG(value.st_mode)) {
    throw B1RevalidationError("B1 artifact descriptor is not a regular file.");
  }
  return value;
}

/**
 * @brief Reads and validates one descriptor as a directory.
 * @param descriptor Open descriptor.
 * @param operation Diagnostic operation name for `fstat` failure.
 * @return Complete native stat record.
 * @throws std::system_error when `fstat` fails.
 * @throws B1RevalidationError when the descriptor is not a directory.
 */
struct stat directory_stat(int descriptor, const char* operation) {
  struct stat value{};
  if (::fstat(descriptor, &value) != 0) {
    throw_errno(operation);
  }
  if (!S_ISDIR(value.st_mode)) {
    throw B1RevalidationError("B1 artifact descriptor is not a directory.");
  }
  return value;
}

/**
 * @brief Writes a complete borrowed byte range to one descriptor.
 * @param descriptor Open writable descriptor.
 * @param data First byte, null only for zero size.
 * @param size Exact byte count.
 * @return Nothing after all bytes are written.
 * @throws std::system_error for an unrecoverable write failure.
 */
void write_all(int descriptor, const std::byte* data, std::size_t size) {
  while (size != 0U) {
    const ssize_t written =
        ::write(descriptor, data,
                std::min(size, static_cast<std::size_t>(
                                   std::numeric_limits<ssize_t>::max())));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("write B1 artifact");
    }
    if (written == 0) {
      throw B1RevalidationError("B1 artifact write made no progress.");
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
}

/**
 * @brief Synchronizes one open file or directory for crash durability.
 * @param descriptor Open file/directory descriptor.
 * @param kind Human-readable object kind for diagnostics.
 * @return Nothing after the platform barrier succeeds.
 * @throws B1DurabilityError when the barrier is unsupported.
 * @throws std::system_error for other synchronization failures.
 */
void synchronize_descriptor(int descriptor, const char* kind) {
  if (::fsync(descriptor) == 0) {
    return;
  }
  if (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {
    throw B1DurabilityError(std::string("B1 ") + kind +
                            " synchronization is unsupported.");
  }
  throw_errno("fsync B1 artifact descriptor");
}

/**
 * @brief Detects whether native scalar storage is little endian.
 * @return True when a uint16 one begins with byte one.
 * @throws Nothing.
 */
bool native_little_endian() noexcept {
  const std::uint16_t one = 1U;
  return *reinterpret_cast<const std::uint8_t*>(&one) == 1U;
}

/**
 * @brief Revalidates that the selected path still names the held root fd.
 * @param root Canonical path retained for receipt and binding checks.
 * @param root_descriptor Held directory capability used for every mutation.
 * @param expected_device Constructor-time device identity.
 * @param expected_inode Constructor-time inode identity.
 * @return Nothing while path/fd/type/identity remain exact.
 * @throws B1RootBindingError for disappearance, symlink/replacement, or drift.
 * @note The path observation is never used as mutation authority.
 */
void verify_root_binding(const std::filesystem::path& root, int root_descriptor,
                         std::uint64_t expected_device,
                         std::uint64_t expected_inode) {
  struct stat descriptor_value{};
  struct stat path_value{};
  if (::fstat(root_descriptor, &descriptor_value) != 0 ||
      ::lstat(root.c_str(), &path_value) != 0 ||
      !S_ISDIR(descriptor_value.st_mode) || !S_ISDIR(path_value.st_mode) ||
      static_cast<std::uint64_t>(descriptor_value.st_dev) != expected_device ||
      static_cast<std::uint64_t>(descriptor_value.st_ino) != expected_inode ||
      file_identity(descriptor_value) != file_identity(path_value)) {
    throw B1RootBindingError(
        "Selected B1 output root path no longer names the held directory.");
  }
}

/**
 * @brief Revalidates one root-relative slot name against its held directory fd.
 * @param root_descriptor Held original root directory.
 * @param slot_descriptor Held slot directory.
 * @param slot_name Single safe root-relative leaf.
 * @param expected Constructor-time slot device/inode identity.
 * @return Nothing while both names/capabilities still bind the same directory.
 * @throws File or revalidation errors on disappearance, type, or identity
 * drift.
 */
void verify_slot_binding(int root_descriptor, int slot_descriptor,
                         const std::string& slot_name,
                         const B1FileIdentity& expected) {
  const struct stat descriptor_value =
      directory_stat(slot_descriptor, "fstat held B1 occurrence slot");
  struct stat named_value{};
  if (::fstatat(root_descriptor, slot_name.c_str(), &named_value,
                AT_SYMLINK_NOFOLLOW) != 0) {
    throw_errno("fstatat B1 occurrence slot");
  }
  if (!S_ISDIR(named_value.st_mode) ||
      file_identity(descriptor_value) != expected ||
      file_identity(named_value) != expected) {
    throw B1RevalidationError("B1 occurrence slot identity drifted.");
  }
}

/**
 * @brief Cross-task immutable/mutable state retained through I/O settlement.
 * @throws Nothing for default construction except owned image/string storage.
 * @note `slot_descriptor` is borrowed from the synchronous commit transaction;
 * its guard always waits accepted work before descriptor destruction.
 */
struct B1CommitTaskState final {
  /** @brief Borrowed held occurrence-slot directory descriptor. */
  int slot_descriptor = -1;
  /** @brief Candidate descriptor retained through payload completion. */
  ImageBuffer image;
  /** @brief Canonical manifest constructed after payload settlement. */
  std::string manifest;
  /** @brief Verified payload digest written by the payload task. */
  B1Sha256Digest payload_digest;
  /** @brief Verified manifest digest written by the commit task. */
  B1Sha256Digest manifest_digest;
  /** @brief Stable published manifest filesystem identity. */
  std::string published_identity;
};

/**
 * @brief Streams exact tight little-endian B1 bytes through a held slot fd.
 * @param state Complete task state retaining candidate and slot capability.
 * @return Nothing after write, fsync, reopen, identity, length, and digest
 * proof.
 * @throws File, descriptor, digest, and durability failures unchanged.
 */
void write_and_validate_payload(B1CommitTaskState* state) {
  if (state == nullptr || state->slot_descriptor < 0) {
    throw std::invalid_argument("B1 payload task state is invalid.");
  }
  const int descriptor = ::openat(
      state->slot_descriptor, kPayloadName,
      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw_errno("openat B1 payload no-replace");
  }
  ScopedFileDescriptor owner(descriptor);
  const B1FileIdentity created_identity =
      file_identity(regular_file_stat(owner.get(), "fstat new B1 payload"));
  B1Sha256 write_hash;
  std::vector<std::byte> converted;
  if (!native_little_endian()) {
    converted.resize(static_cast<std::size_t>(kB1PayloadRowBytes));
  }
  for (int row_index = 0; row_index < state->image.height; ++row_index) {
    const std::byte* row = image_buffer_row_data(state->image, row_index);
    if (native_little_endian()) {
      write_all(owner.get(), row, static_cast<std::size_t>(kB1PayloadRowBytes));
      write_hash.update(row, static_cast<std::size_t>(kB1PayloadRowBytes));
      continue;
    }
    for (std::size_t offset = 0U; offset < converted.size(); offset += 4U) {
      converted[offset] = row[offset + 3U];
      converted[offset + 1U] = row[offset + 2U];
      converted[offset + 2U] = row[offset + 1U];
      converted[offset + 3U] = row[offset];
    }
    write_all(owner.get(), converted.data(), converted.size());
    write_hash.update(converted.data(), converted.size());
  }
  synchronize_descriptor(owner.get(), "file");
  const struct stat written_stat =
      regular_file_stat(owner.get(), "fstat written B1 payload");
  if (file_identity(written_stat) != created_identity ||
      written_stat.st_size < 0 ||
      static_cast<std::uint64_t>(written_stat.st_size) != kB1PayloadBytes) {
    throw B1RevalidationError("B1 written payload identity or length drifted.");
  }
  owner.close();
  const B1Sha256Digest first_digest = write_hash.finish();

  const int read_descriptor = ::openat(state->slot_descriptor, kPayloadName,
                                       O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (read_descriptor < 0) {
    throw_errno("openat reopen B1 payload");
  }
  ScopedFileDescriptor read_owner(read_descriptor);
  const struct stat reopened_stat =
      regular_file_stat(read_owner.get(), "fstat reopened B1 payload");
  if (file_identity(reopened_stat) != created_identity) {
    throw B1RevalidationError("B1 payload inode changed before revalidation.");
  }
  B1Sha256 read_hash;
  std::array<std::byte, 65536U> buffer{};
  std::uint64_t length = 0U;
  for (;;) {
    const ssize_t count =
        ::read(read_owner.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("read B1 payload for revalidation");
    }
    if (count == 0) {
      break;
    }
    if (length > std::numeric_limits<std::uint64_t>::max() -
                     static_cast<std::uint64_t>(count)) {
      throw B1RevalidationError("B1 payload length overflowed.");
    }
    length += static_cast<std::uint64_t>(count);
    read_hash.update(buffer.data(), static_cast<std::size_t>(count));
  }
  read_owner.close();
  const B1Sha256Digest second_digest = read_hash.finish();
  if (length != kB1PayloadBytes || first_digest != second_digest) {
    throw B1RevalidationError(
        "B1 payload length or digest revalidation failed.");
  }
  state->payload_digest = second_digest;
}

/**
 * @brief Reads one exact slot-relative file and validates bytes plus identity.
 * @param slot_descriptor Held occurrence-slot directory descriptor.
 * @param leaf Single fixed leaf name.
 * @param expected Exact expected bytes.
 * @param expected_identity Optional required device/inode identity.
 * @param observed_identity Optional output for the reopened file identity.
 * @return SHA-256 of the validated bytes.
 * @throws File or revalidation failures unchanged.
 */
B1Sha256Digest validate_exact_file_at(
    int slot_descriptor, const char* leaf, std::string_view expected,
    const B1FileIdentity* expected_identity,
    B1FileIdentity* observed_identity = nullptr) {
  const int descriptor =
      ::openat(slot_descriptor, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw_errno("openat B1 manifest for revalidation");
  }
  ScopedFileDescriptor owner(descriptor);
  const struct stat opened_stat =
      regular_file_stat(owner.get(), "fstat reopened B1 manifest");
  const B1FileIdentity identity = file_identity(opened_stat);
  if (expected_identity != nullptr && identity != *expected_identity) {
    throw B1RevalidationError("B1 manifest inode changed during publication.");
  }
  std::string observed(expected.size(), '\0');
  std::size_t offset = 0U;
  while (offset < observed.size()) {
    const ssize_t count =
        ::read(owner.get(), observed.data() + offset, observed.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("read B1 manifest for revalidation");
    }
    if (count == 0) {
      throw B1RevalidationError("B1 manifest was truncated.");
    }
    offset += static_cast<std::size_t>(count);
  }
  std::byte extra{};
  ssize_t trailing = 0;
  do {
    trailing = ::read(owner.get(), &extra, 1U);
  } while (trailing < 0 && errno == EINTR);
  if (trailing < 0) {
    throw_errno("read B1 manifest trailing byte");
  }
  owner.close();
  if (trailing != 0 || observed != expected) {
    throw B1RevalidationError("B1 manifest bytes drifted.");
  }
  if (observed_identity != nullptr) {
    *observed_identity = identity;
  }
  return b1_sha256(observed);
}

/**
 * @brief Writes, publishes, barriers, and revalidates a slot-relative manifest.
 * @param state Complete task state with verified payload digest/manifest.
 * @param root_descriptor Held original root directory capability.
 * @param root Canonical root spelling retained only for binding proof.
 * @param root_device Constructor-time root device.
 * @param root_inode Constructor-time root inode.
 * @param slot_name Root-relative occurrence slot leaf.
 * @param slot_identity Creation-time held slot identity.
 * @return Nothing after leaf-to-root barriers and final binding revalidation.
 * @throws File, durability, root-binding, and revalidation failures unchanged.
 */
void commit_and_validate_manifest(B1CommitTaskState* state, int root_descriptor,
                                  const std::filesystem::path& root,
                                  std::uint64_t root_device,
                                  std::uint64_t root_inode,
                                  const std::string& slot_name,
                                  const B1FileIdentity& slot_identity) {
  if (state == nullptr || state->slot_descriptor < 0) {
    throw std::invalid_argument("B1 manifest task state is invalid.");
  }
  const int descriptor = ::openat(
      state->slot_descriptor, kPrivateManifestName,
      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    throw_errno("openat private B1 manifest no-replace");
  }
  ScopedFileDescriptor owner(descriptor);
  const B1FileIdentity private_identity = file_identity(
      regular_file_stat(owner.get(), "fstat private B1 manifest"));
  write_all(owner.get(),
            reinterpret_cast<const std::byte*>(state->manifest.data()),
            state->manifest.size());
  synchronize_descriptor(owner.get(), "file");
  owner.close();

  if (::linkat(state->slot_descriptor, kPrivateManifestName,
               state->slot_descriptor, kManifestName, 0) != 0) {
    if (errno == ENOTSUP || errno == EOPNOTSUPP || errno == EPERM) {
      throw B1DurabilityError("B1 atomic link-no-replace is unsupported.");
    }
    throw_errno("linkat publish B1 manifest no-replace");
  }
  if (::unlinkat(state->slot_descriptor, kPrivateManifestName, 0) != 0) {
    throw_errno("unlinkat private B1 manifest after publication");
  }

  B1FileIdentity published_identity;
  state->manifest_digest = validate_exact_file_at(
      state->slot_descriptor, kManifestName, state->manifest, &private_identity,
      &published_identity);
  std::ostringstream identity_text;
  identity_text << "dev=" << published_identity.device
                << ";ino=" << published_identity.inode;
  state->published_identity = identity_text.str();

  synchronize_descriptor(state->slot_descriptor, "directory");
  synchronize_descriptor(root_descriptor, "directory");
  verify_slot_binding(root_descriptor, state->slot_descriptor, slot_name,
                      slot_identity);
  verify_root_binding(root, root_descriptor, root_device, root_inode);
  const B1Sha256Digest after_barrier =
      validate_exact_file_at(state->slot_descriptor, kManifestName,
                             state->manifest, &published_identity);
  if (after_barrier != state->manifest_digest) {
    throw B1RevalidationError(
        "B1 manifest digest changed after namespace barriers.");
  }
}

/**
 * @brief Removes one known slot-relative leaf while containing all failures.
 * @param slot_descriptor Held occurrence-slot directory.
 * @param leaf One fixed store-owned leaf.
 * @return Nothing; absence and cleanup failures are contained.
 * @throws Nothing.
 */
void unlink_known_leaf_noexcept(int slot_descriptor,
                                const char* leaf) noexcept {
  if (::unlinkat(slot_descriptor, leaf, 0) != 0 && errno != ENOENT) {
    return;
  }
}

/**
 * @brief Best-effort removes one exact verified occurrence slot fd-relatively.
 * @param root_descriptor Held original root directory.
 * @param slot_descriptor Held occurrence-slot directory.
 * @param slot_name Exact root-relative leaf.
 * @param expected Exact creation-time slot identity.
 * @return Nothing.
 * @throws Nothing; identity drift refuses broad or redirected cleanup.
 */
void cleanup_failed_slot(int root_descriptor, int slot_descriptor,
                         const std::string& slot_name,
                         const B1FileIdentity& expected) noexcept {
  struct stat held{};
  struct stat named{};
  if (::fstat(slot_descriptor, &held) != 0 || !S_ISDIR(held.st_mode) ||
      file_identity(held) != expected ||
      ::fstatat(root_descriptor, slot_name.c_str(), &named,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(named.st_mode) || file_identity(named) != expected) {
    return;
  }
  unlink_known_leaf_noexcept(slot_descriptor, kPrivateManifestName);
  unlink_known_leaf_noexcept(slot_descriptor, kManifestName);
  unlink_known_leaf_noexcept(slot_descriptor, kPayloadName);
  static_cast<void>(
      ::unlinkat(root_descriptor, slot_name.c_str(), AT_REMOVEDIR));
}

/**
 * @brief Owns failed-slot cleanup and at most one accepted task settlement.
 *
 * @throws Nothing from destruction; cancellation/wait/cleanup invariant
 * failures terminate because descriptor lifetime cannot otherwise be proved.
 * @note The guard is created immediately after slot fd acquisition. On every
 * exceptional or typed-failure exit it waits accepted work before unlinking.
 */
class B1CommitTransactionGuard final {
 public:
  /**
   * @brief Adopts one newly created verified occurrence slot.
   * @param root_descriptor Held original root directory.
   * @param slot_descriptor Held occurrence-slot directory.
   * @param slot_name Exact root-relative slot leaf retained by the caller.
   * @param slot_identity Creation-time slot device/inode.
   * @throws Nothing.
   */
  B1CommitTransactionGuard(int root_descriptor, int slot_descriptor,
                           const std::string& slot_name,
                           B1FileIdentity slot_identity) noexcept
      : root_descriptor_(root_descriptor),
        slot_descriptor_(slot_descriptor),
        slot_name_(slot_name),
        slot_identity_(slot_identity) {}

  /**
   * @brief Cancels/waits accepted work, then cleans an uncommitted slot.
   * @throws Nothing; unexpected synchronization failure terminates fail-stop.
   */
  ~B1CommitTransactionGuard() noexcept {
    try {
      if (completion_active_) {
        static_cast<void>(completion_.cancel());
        static_cast<void>(completion_.wait());
        completion_active_ = false;
      }
      if (cleanup_required_) {
        cleanup_failed_slot(root_descriptor_, slot_descriptor_, slot_name_,
                            slot_identity_);
      }
    } catch (...) {
      std::terminate();
    }
  }

  /** @brief Transaction ownership cannot be copied. */
  B1CommitTransactionGuard(const B1CommitTransactionGuard&) = delete;

  /** @brief Transaction ownership cannot be assigned. */
  B1CommitTransactionGuard& operator=(const B1CommitTransactionGuard&) = delete;

  /**
   * @brief Adopts one accepted completion before any subsequent allocation.
   * @param completion Active accepted-task completion.
   * @return Nothing.
   * @throws Nothing; nested active adoption terminates as an invariant breach.
   */
  void adopt(const execution::ComputeIoCompletion& completion) noexcept {
    if (completion_active_ || !completion.active()) {
      std::terminate();
    }
    completion_ = completion;
    completion_active_ = true;
  }

  /**
   * @brief Releases wait ownership immediately after terminal result return.
   * @return Nothing.
   * @throws Nothing; missing active adoption terminates.
   */
  void mark_settled() noexcept {
    if (!completion_active_) {
      std::terminate();
    }
    completion_active_ = false;
    completion_ = {};
  }

  /**
   * @brief Preserves the completed immutable slot on successful receipt return.
   * @return Nothing.
   * @throws Nothing; active task ownership terminates as an invariant breach.
   */
  void preserve_slot() noexcept {
    if (completion_active_) {
      std::terminate();
    }
    cleanup_required_ = false;
  }

 private:
  /** @brief Held original root directory descriptor. */
  int root_descriptor_ = -1;
  /** @brief Held occurrence-slot directory descriptor. */
  int slot_descriptor_ = -1;
  /** @brief Borrowed root-relative slot leaf valid past guard destruction. */
  const std::string& slot_name_;
  /** @brief Creation-time slot identity. */
  B1FileIdentity slot_identity_;
  /** @brief Current accepted task completion, when any. */
  execution::ComputeIoCompletion completion_;
  /** @brief Whether destruction must cancel/wait `completion_`. */
  bool completion_active_ = false;
  /** @brief Whether destruction must remove the occurrence slot. */
  bool cleanup_required_ = true;
};

/**
 * @brief Returns a readable diagnostic from one callback exception.
 * @param failure Original exception identity.
 * @return Exception text or stable unknown marker.
 * @throws std::bad_alloc when diagnostic allocation fails.
 */
std::string exception_diagnostic(const std::exception_ptr& failure) {
  if (failure == nullptr) {
    return "accepted B1 I/O task failed without exception identity";
  }
  try {
    std::rethrow_exception(failure);
  } catch (const std::exception& error) {
    return error.what();
  } catch (...) {
    return "accepted B1 I/O task threw a non-standard exception";
  }
}

/**
 * @brief Classifies an accepted callback failure without losing its diagnostic.
 * @param failure Original callback exception.
 * @return Root, durability, revalidation, or generic task-failure status.
 * @throws Nothing.
 */
B1OutputCommitStatus classify_task_failure(
    const std::exception_ptr& failure) noexcept {
  try {
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  } catch (const B1RootBindingError&) {
    return B1OutputCommitStatus::RootUnavailable;
  } catch (const B1DurabilityError&) {
    return B1OutputCommitStatus::DurabilityUnsupported;
  } catch (const B1RevalidationError&) {
    return B1OutputCommitStatus::RevalidationFailed;
  } catch (...) {
    return B1OutputCommitStatus::TaskFailed;
  }
  return B1OutputCommitStatus::TaskFailed;
}

#endif

}  // namespace

B1OutputStore::B1OutputStore(std::filesystem::path root,
                             execution::ComputeIoExecutor& executor,
                             B1OutputStoreOptions options)
    : root_(std::filesystem::canonical(std::move(root))),
      executor_(executor),
      options_(options) {
  if (!std::filesystem::is_directory(root_)) {
    throw std::invalid_argument("B1 output root is not an existing directory.");
  }
#if !defined(_WIN32)
  const int descriptor =
      ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw_errno("open selected B1 output root");
  }
  ScopedFileDescriptor owner(descriptor);
  const struct stat identity =
      directory_stat(owner.get(), "fstat selected B1 output root");
  root_device_ = static_cast<std::uint64_t>(identity.st_dev);
  root_inode_ = static_cast<std::uint64_t>(identity.st_ino);
  root_descriptor_ = owner.release();
#endif
}

B1OutputStore::~B1OutputStore() noexcept {
#if !defined(_WIN32)
  if (root_descriptor_ >= 0 && ::close(root_descriptor_) != 0) {
    std::terminate();
  }
  root_descriptor_ = -1;
#endif
}

B1OutputCommitResult B1OutputStore::commit(const B1JobInstance& job,
                                           const ImageBuffer& image) {
  B1OutputCommitResult result;
  try {
    validate_b1_job_instance(job);
    if (options_.requested_durability != B1OutputDurability::CrashDurable) {
      result.status = B1OutputCommitStatus::InvalidRequest;
      result.diagnostic = "B1 requires requested crash-durable output.";
      return result;
    }
    if (!options_.crash_durability_supported) {
      result.status = B1OutputCommitStatus::DurabilityUnsupported;
      result.diagnostic = "Selected B1 backend cannot prove crash durability.";
      return result;
    }
  } catch (const std::exception& error) {
    result.status = B1OutputCommitStatus::InvalidRequest;
    result.diagnostic = error.what();
    return result;
  }
  try {
    validate_b1_candidate_image(image);
  } catch (const std::exception& error) {
    result.status = B1OutputCommitStatus::InvalidImage;
    result.diagnostic = error.what();
    return result;
  }

#if defined(_WIN32)
  result.status = B1OutputCommitStatus::DurabilityUnsupported;
  result.diagnostic =
      "B1 crash-durable no-replace/barrier backend is unsupported on Windows.";
  return result;
#else
  try {
    verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
  } catch (const std::exception& error) {
    result.status = B1OutputCommitStatus::RootUnavailable;
    result.diagnostic = error.what();
    return result;
  }
  invoke_fault_injector(options_,
                        B1OutputStoreFaultPoint::AfterRootBindingVerified);

  B1Sha256 commit_hash;
  commit_hash.update("execution-profile-output-commit-id-v1\n");
  commit_hash.update(encode_b1_job_instance(job));
  const std::string commit_id = b1_digest_hex(commit_hash.finish());
  const std::filesystem::path rooted_slot = "occurrence-" + commit_id;
  const std::string slot_name = rooted_slot.string();
  if (::mkdirat(root_descriptor_, slot_name.c_str(), S_IRWXU) != 0) {
    result.status = errno == EEXIST ? B1OutputCommitStatus::SlotExists
                                    : B1OutputCommitStatus::RootUnavailable;
    result.diagnostic = errno == EEXIST
                            ? "The immutable B1 occurrence slot already exists."
                            : "Cannot create the B1 occurrence slot: " +
                                  std::string(std::strerror(errno));
    return result;
  }

  const int slot_raw =
      ::openat(root_descriptor_, slot_name.c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (slot_raw < 0) {
    const int open_error = errno;
    static_cast<void>(
        ::unlinkat(root_descriptor_, slot_name.c_str(), AT_REMOVEDIR));
    result.status = B1OutputCommitStatus::RootUnavailable;
    result.diagnostic = "Cannot retain the B1 occurrence slot descriptor: " +
                        std::string(std::strerror(open_error));
    return result;
  }
  ScopedFileDescriptor slot_owner(slot_raw);
  B1FileIdentity slot_identity;
  try {
    slot_identity = file_identity(
        directory_stat(slot_owner.get(), "fstat new B1 occurrence slot"));
  } catch (...) {
    static_cast<void>(
        ::unlinkat(root_descriptor_, slot_name.c_str(), AT_REMOVEDIR));
    throw;
  }
  B1CommitTransactionGuard transaction(root_descriptor_, slot_owner.get(),
                                       slot_name, slot_identity);
  invoke_fault_injector(options_, B1OutputStoreFaultPoint::AfterSlotCreated);

  auto state = std::make_shared<B1CommitTaskState>();
  state->slot_descriptor = slot_owner.get();
  state->image = image;

  B1ComputeIoObservation initial;
  initial.point = B1IoObservationPoint::Initial;
  initial.snapshot = executor_.snapshot();
  result.io_observations.push_back(std::move(initial));

  /**
   * @brief Retains one row-level cut after this commit's guarded task settles.
   * @return Nothing after appending the authority-free process snapshot.
   * @throws std::bad_alloc when evidence storage cannot grow.
   * @note Other process tasks may remain active; this is not an own-charge
   * fact.
   */
  const auto append_final_observation = [&]() {
    B1ComputeIoObservation final;
    final.point = B1IoObservationPoint::Final;
    final.snapshot = executor_.snapshot();
    result.io_observations.push_back(std::move(final));
  };

  /**
   * @brief Offers, guards, waits, and records one exact task attempt sequence.
   * @param identity Immutable attempt-zero task identity.
   * @param planned_bytes Exact positive task charge.
   * @param task Callback executed only by the process I/O worker.
   * @return Terminal result, or null after typed/bounded admission failure.
   * @throws Factory, hook, evidence-allocation, and synchronization errors.
   * @note Admission and settlement snapshots come from executor-authored events
   * captured at their accounting linearization points, never later snapshots.
   */
  const auto run_task = [&](const B1IoTaskIdentity& identity,
                            std::uint64_t planned_bytes,
                            const execution::ComputeIoExecutor::Task& task)
      -> std::optional<execution::ComputeIoTaskResult> {
    for (std::size_t attempt_number = 1U;
         attempt_number <= kB1CapacityAdmissionAttemptLimit; ++attempt_number) {
      const std::shared_ptr<const void> lifetime = state;
      const execution::ComputeIoSubmission submission =
          executor_.try_submit(planned_bytes, lifetime, [&, task]() {
            invoke_fault_injector(options_,
                                  B1OutputStoreFaultPoint::InsideTaskFactory);
            return execution::ComputeIoExecutor::Task([this, task]() {
              invoke_fault_injector(options_,
                                    B1OutputStoreFaultPoint::BeforeTaskWork);
              task();
            });
          });
      if (submission.accepted()) {
        transaction.adopt(submission.completion());
      }

      B1ComputeIoObservation admission;
      admission.point = submission.accepted()
                            ? B1IoObservationPoint::AcceptedAdmission
                            : B1IoObservationPoint::OfferRejected;
      admission.task = identity;
      admission.planned_bytes = planned_bytes;
      admission.admission = submission.admission_status();
      admission.admission_event = submission.admission_event();
      admission.snapshot = submission.admission_event().snapshot_after;
      result.io_observations.push_back(std::move(admission));

      if (submission.accepted()) {
        invoke_fault_injector(options_,
                              B1OutputStoreFaultPoint::AfterTaskAccepted);
        const execution::ComputeIoTaskResult completion =
            submission.completion().wait();
        transaction.mark_settled();
        B1ComputeIoObservation settlement;
        settlement.point = B1IoObservationPoint::Settlement;
        settlement.task = identity;
        settlement.planned_bytes = planned_bytes;
        settlement.admission = submission.admission_status();
        settlement.completion = completion.status();
        settlement.admission_event = submission.admission_event();
        settlement.settlement_event = completion.settlement_event();
        settlement.snapshot = completion.settlement_event().snapshot_after;
        result.io_observations.push_back(std::move(settlement));
        invoke_fault_injector(options_,
                              B1OutputStoreFaultPoint::AfterTaskSettled);
        return completion;
      }
      if (submission.admission_status() !=
              execution::ComputeIoAdmissionStatus::TaskLimit &&
          submission.admission_status() !=
              execution::ComputeIoAdmissionStatus::PlannedByteLimit) {
        return std::nullopt;
      }
      if (planned_bytes >
          submission.admission_event().snapshot_after.planned_bytes_limit) {
        return std::nullopt;
      }
      if (options_.capacity_rejection_observer != nullptr) {
        options_.capacity_rejection_observer(
            options_.capacity_rejection_observer_context, identity,
            attempt_number);
      }
      if (attempt_number == kB1CapacityAdmissionAttemptLimit) {
        return std::nullopt;
      }
      std::this_thread::yield();
    }
    return std::nullopt;
  };

  const B1IoTaskIdentity payload_identity{job, B1IoStage::PayloadStage, 0U};
  const auto payload = run_task(payload_identity, kB1PayloadBytes, [state]() {
    write_and_validate_payload(state.get());
  });
  if (!payload.has_value()) {
    result.status = B1OutputCommitStatus::AdmissionFailed;
    result.diagnostic = "B1 payload admission failed after at most " +
                        std::to_string(kB1CapacityAdmissionAttemptLimit) +
                        " deterministic attempts.";
    append_final_observation();
    return result;
  }
  if (payload->status() != execution::ComputeIoCompletionStatus::Succeeded) {
    result.status = classify_task_failure(payload->failure());
    result.diagnostic =
        payload->status() == execution::ComputeIoCompletionStatus::Cancelled
            ? "accepted B1 payload task was cancelled"
            : exception_diagnostic(payload->failure());
    append_final_observation();
    return result;
  }

  state->manifest = b1_artifact_manifest(job.job_index, state->payload_digest);
  const B1IoTaskIdentity manifest_identity{job, B1IoStage::ManifestCommit, 0U};
  const auto manifest =
      run_task(manifest_identity, b1_manifest_length(job.job_index),
               [this, state, slot_name, slot_identity]() {
                 commit_and_validate_manifest(state.get(), root_descriptor_,
                                              root_, root_device_, root_inode_,
                                              slot_name, slot_identity);
               });
  if (!manifest.has_value()) {
    result.status = B1OutputCommitStatus::AdmissionFailed;
    result.diagnostic = "B1 manifest admission failed after at most " +
                        std::to_string(kB1CapacityAdmissionAttemptLimit) +
                        " deterministic attempts.";
    append_final_observation();
    return result;
  }
  if (manifest->status() != execution::ComputeIoCompletionStatus::Succeeded) {
    result.status = classify_task_failure(manifest->failure());
    result.diagnostic =
        manifest->status() == execution::ComputeIoCompletionStatus::Cancelled
            ? "accepted B1 manifest task was cancelled"
            : exception_diagnostic(manifest->failure());
    append_final_observation();
    return result;
  }

  append_final_observation();
  try {
    invoke_fault_injector(options_,
                          B1OutputStoreFaultPoint::BeforeReceiptAssembly);
    verify_slot_binding(root_descriptor_, slot_owner.get(), slot_name,
                        slot_identity);
    verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
    const Value candidate =
        value_image_adapter::snapshot_cpu_image_value(image);
    const ContentDigestResult logical = compute_content_digest(candidate);
    if (logical.state != ContentDigestState::Available ||
        !logical.digest.has_value()) {
      throw B1RevalidationError("B1 candidate logical digest is unavailable: " +
                                logical.diagnostic);
    }
    result.receipt =
        B1OutputCommitReceipt{commit_id,
                              root_,
                              rooted_slot,
                              job,
                              "dense-tensor-hwc-fp32-rgba-2048x2048",
                              *logical.digest,
                              1U,
                              kPayloadName,
                              kManifestName,
                              kB1PayloadBytes,
                              b1_manifest_length(job.job_index),
                              state->payload_digest,
                              state->manifest_digest,
                              B1OutputDurability::CrashDurable,
                              B1OutputDurability::CrashDurable,
                              state->published_identity};
    result.status = B1OutputCommitStatus::Succeeded;
    transaction.preserve_slot();
    return result;
  } catch (const B1RootBindingError& error) {
    result.status = B1OutputCommitStatus::RootUnavailable;
    result.diagnostic = error.what();
    return result;
  } catch (const std::exception& error) {
    result.status = B1OutputCommitStatus::RevalidationFailed;
    result.diagnostic = error.what();
    return result;
  }
#endif
}

}  // namespace ps::benchmark
