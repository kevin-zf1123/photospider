/**
 * @file b1_output_store.cpp
 * @brief Implements process-I/O-backed crash-durable B1 artifact commits.
 */
#include "benchmark/b1/b1_output_store.hpp"

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
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#include <sys/mount.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <sys/vfs.h>
#endif
#endif

#include "photospider/data/image_view.hpp"

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
 * @brief Typed collision for atomic no-replace or plainly foreign public state.
 * @throws Nothing beyond standard runtime-error string allocation.
 */
class B1SlotExistsError final : public std::runtime_error {
 public:
  /**
   * @brief Creates the closed no-replace/no-transaction-leaf collision failure.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  B1SlotExistsError()
      : std::runtime_error("The immutable B1 occurrence slot already exists.") {
  }
};

/**
 * @brief Validates the exact frozen candidate descriptor.
 * @param image Candidate image.
 * @return Nothing for exact Ready CPU FP32 RGBA B1 state.
 * @throws std::invalid_argument for descriptor drift.
 * @throws ReadyFenceAccessError or BufferAccessError when payload access is
 * unavailable.
 */
void validate_b1_candidate_image(const Value& image) {
  const ImageView view(image);
  const DenseTensorDescriptor expected_descriptor{
      {static_cast<std::size_t>(kB1ImageEdge),
       static_cast<std::size_t>(kB1ImageEdge),
       static_cast<std::size_t>(kB1ChannelCount)},
      ElementSemantics::FloatingPoint,
      StorageEncoding{32U}};
  const ImageFacet expected_facet =
      make_zero_origin_image_facet(expected_descriptor, 1U, 0U, 2U);
  const StorageBinding binding = image.storage_binding();
  if (!(view.descriptor() == expected_descriptor) ||
      !(view.image_facet() == expected_facet) ||
      image.storage_layout_kind() != StorageLayoutKind::Strided ||
      binding.device.backend() != DeviceBackend::CPU || !binding.host_visible) {
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
 * @brief Owns one duplicated POSIX directory stream for exact entry inspection.
 * @throws std::invalid_argument for a null stream; destruction is fail-stop on
 * close failure because an unclosed stream would retain descriptor authority.
 */
class ScopedDirectoryStream final {
 public:
  /**
   * @brief Adopts one non-null stream returned by `fdopendir`.
   * @param stream Owned stream.
   * @throws std::invalid_argument when `stream` is null.
   */
  explicit ScopedDirectoryStream(DIR* stream) : stream_(stream) {
    if (stream_ == nullptr) {
      throw std::invalid_argument("B1 directory stream is null.");
    }
  }

  /** @brief Closes the owned stream; close failure terminates fail-stop. */
  ~ScopedDirectoryStream() noexcept {
    if (stream_ != nullptr && ::closedir(stream_) != 0) {
      std::terminate();
    }
  }

  /** @brief Directory-stream ownership cannot be copied. */
  ScopedDirectoryStream(const ScopedDirectoryStream&) = delete;

  /** @brief Directory-stream ownership cannot be assigned. */
  ScopedDirectoryStream& operator=(const ScopedDirectoryStream&) = delete;

  /**
   * @brief Returns the borrowed stream.
   * @return Non-null stream valid for this owner lifetime.
   * @throws Nothing.
   */
  DIR* get() const noexcept { return stream_; }

 private:
  /** @brief Owned directory stream. */
  DIR* stream_ = nullptr;
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
 * @brief Packs one logical B1 row into canonical little-endian RGBA32 bytes.
 * @param image Validated exact B1 image Value.
 * @param row_index Zero-based row smaller than `kB1ImageEdge`.
 * @param output Mutable row buffer replaced with exactly
 * `kB1PayloadRowBytes` bytes.
 * @return Nothing after every logical sample is copied.
 * @throws std::out_of_range or BufferAccessError from checked Value access.
 * @throws std::bad_alloc when row storage cannot allocate.
 * @note Source padding and physical stride never enter the durable payload.
 */
void pack_b1_payload_row(const Value& image, std::size_t row_index,
                         std::vector<std::byte>* output) {
  if (output == nullptr) {
    throw std::invalid_argument("B1 row packing requires an output buffer.");
  }
  const ImageView view(image);
  output->resize(static_cast<std::size_t>(kB1PayloadRowBytes));
  std::size_t offset = 0U;
  for (std::size_t x = 0U; x < view.width(); ++x) {
    for (std::size_t channel = 0U; channel < view.channels(); ++channel) {
      const std::byte* source = view.channel_data(x, row_index, channel);
      if (native_little_endian()) {
        std::memcpy(output->data() + offset, source, sizeof(float));
      } else {
        output->at(offset) = source[3U];
        output->at(offset + 1U) = source[2U];
        output->at(offset + 2U) = source[1U];
        output->at(offset + 3U) = source[0U];
      }
      offset += sizeof(float);
    }
  }
  if (offset != output->size()) {
    throw std::logic_error("B1 packed row length drifted.");
  }
}

/**
 * @brief Hashes one exact candidate as tight little-endian B1 payload bytes.
 * @param image Validated CPU FP32 RGBA B1 candidate.
 * @return SHA-256 matching bytes written by `write_and_validate_payload`.
 * @throws std::bad_alloc on big-endian hosts when one conversion row allocates.
 * @throws Digest or image-row validation failures unchanged.
 * @note The function performs no filesystem mutation and is used to identify a
 * same-commit public occurrence before any reconciliation barrier is attempted.
 */
B1Sha256Digest digest_b1_payload_image(const Value& image) {
  B1Sha256 hash;
  std::vector<std::byte> row;
  for (std::size_t row_index = 0U;
       row_index < static_cast<std::size_t>(kB1ImageEdge); ++row_index) {
    pack_b1_payload_row(image, row_index, &row);
    hash.update(row.data(), row.size());
  }
  return hash.finish();
}

/**
 * @brief Computes the required typed logical digest for one B1 candidate.
 * @param image Validated exact candidate image.
 * @return Available typed content identity.
 * @throws B1RevalidationError when the digest is unavailable.
 * @throws Adapter, digest, or allocation failures unchanged.
 */
ContentDigest compute_b1_logical_digest(const Value& image) {
  const ContentDigestResult logical = compute_content_digest(image);
  if (logical.state != ContentDigestState::Available ||
      !logical.digest.has_value()) {
    throw B1RevalidationError("B1 candidate logical digest is unavailable: " +
                              logical.diagnostic);
  }
  return *logical.digest;
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
 * @brief Returns a closed identifier for the filesystem behind a held root.
 * @param root_descriptor Held selected root descriptor.
 * @return `darwin-<fstypename>` or `linux-fs-<unsigned-hex-magic>`.
 * @throws std::system_error when `fstatfs` fails.
 * @throws B1RevalidationError for an empty or unsupported observation.
 * @throws std::bad_alloc when identifier construction allocates.
 * @note The value comes from the descriptor, never from retained evidence or a
 * caller-selected pathname string.
 */
std::string filesystem_type_from_descriptor(int root_descriptor) {
  struct statfs value{};
  if (::fstatfs(root_descriptor, &value) != 0) {
    throw_errno("fstatfs selected B1 output root");
  }
#if defined(__APPLE__)
  std::string result = "darwin-";
  for (const unsigned char character :
       std::string_view(value.f_fstypename,
                        std::char_traits<char>::length(value.f_fstypename))) {
    if (character >= 'A' && character <= 'Z') {
      result.push_back(static_cast<char>(character - 'A' + 'a'));
    } else if ((character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9') || character == '.' ||
               character == '_' || character == '+' || character == '-') {
      result.push_back(static_cast<char>(character));
    } else {
      result.push_back('-');
    }
  }
  if (result == "darwin-") {
    throw B1RevalidationError("B1 root filesystem type is empty.");
  }
  return result;
#elif defined(__linux__)
  std::ostringstream result;
  result << "linux-fs-" << std::hex << static_cast<std::uint64_t>(value.f_type);
  return result.str();
#else
  (void)value;
  throw B1RevalidationError(
      "B1 root filesystem type observation is unsupported.");
#endif
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
  Value image;
  /** @brief Canonical manifest constructed after payload settlement. */
  std::string manifest;
  /** @brief Verified payload digest written by the payload task. */
  B1Sha256Digest payload_digest;
  /** @brief Creation-time payload identity, once the fd has been verified. */
  std::optional<B1FileIdentity> payload_identity;
  /** @brief Verified manifest digest written by the commit task. */
  B1Sha256Digest manifest_digest;
  /** @brief Private-manifest identity while its private name exists. */
  std::optional<B1FileIdentity> private_manifest_identity;
  /** @brief Published manifest identity after its no-replace hard link. */
  std::optional<B1FileIdentity> manifest_identity;
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
  state->payload_identity = created_identity;
  B1Sha256 write_hash;
  std::vector<std::byte> row;
  for (std::size_t row_index = 0U;
       row_index < static_cast<std::size_t>(kB1ImageEdge); ++row_index) {
    pack_b1_payload_row(state->image, row_index, &row);
    write_all(owner.get(), row.data(), row.size());
    write_hash.update(row.data(), row.size());
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
 * @brief Streams and validates one exact public B1 payload through its slot fd.
 * @param slot_descriptor Held occurrence directory.
 * @param expected_digest Digest of the caller's exact candidate bytes.
 * @param expected_identity Optional identity retained by the publishing call.
 * @param observed_identity Optional output for the reopened payload identity.
 * @return Recomputed digest after exact length and identity validation.
 * @throws File, digest, or revalidation failures unchanged.
 * @note This helper is read-only and never follows a public-slot pathname.
 */
B1Sha256Digest validate_b1_payload_at(
    int slot_descriptor, const B1Sha256Digest& expected_digest,
    const B1FileIdentity* expected_identity,
    B1FileIdentity* observed_identity = nullptr) {
  const int descriptor = ::openat(slot_descriptor, kPayloadName,
                                  O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw_errno("openat B1 payload for public revalidation");
  }
  ScopedFileDescriptor owner(descriptor);
  const struct stat opened_stat =
      regular_file_stat(owner.get(), "fstat public B1 payload");
  const B1FileIdentity identity = file_identity(opened_stat);
  if (opened_stat.st_size < 0 ||
      static_cast<std::uint64_t>(opened_stat.st_size) != kB1PayloadBytes ||
      (expected_identity != nullptr && identity != *expected_identity)) {
    throw B1RevalidationError("B1 public payload length or identity drifted.");
  }

  B1Sha256 hash;
  std::array<std::byte, 65536U> buffer{};
  std::uint64_t length = 0U;
  for (;;) {
    const ssize_t count = ::read(owner.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("read B1 public payload for revalidation");
    }
    if (count == 0) {
      break;
    }
    if (length > std::numeric_limits<std::uint64_t>::max() -
                     static_cast<std::uint64_t>(count)) {
      throw B1RevalidationError("B1 public payload length overflowed.");
    }
    length += static_cast<std::uint64_t>(count);
    hash.update(buffer.data(), static_cast<std::size_t>(count));
  }
  owner.close();
  const B1Sha256Digest observed_digest = hash.finish();
  if (length != kB1PayloadBytes || observed_digest != expected_digest) {
    throw B1RevalidationError("B1 public payload bytes drifted.");
  }
  if (observed_identity != nullptr) {
    *observed_identity = identity;
  }
  return observed_digest;
}

/**
 * @brief Formats one stable filesystem identity for an immutable receipt.
 * @param identity Exact manifest device/inode identity.
 * @return Canonical `dev=<u>;ino=<u>` text.
 * @throws std::bad_alloc when stream storage cannot allocate.
 */
std::string b1_file_identity_text(const B1FileIdentity& identity) {
  std::ostringstream output;
  output << "dev=" << identity.device << ";ino=" << identity.inode;
  return output.str();
}

/**
 * @brief Opens one existing public occurrence without following substitutions.
 * @param root_descriptor Held selected output root.
 * @param slot_name Exact deterministic occurrence leaf.
 * @param named_stat No-follow result that detected the existing leaf.
 * @param slot_identity Output for the name/descriptor-matched identity.
 * @return Newly owned read-only directory descriptor.
 * @throws B1SlotExistsError when the collision is not a directory.
 * @throws File or revalidation failures for open/type/identity drift.
 */
int open_existing_public_slot(int root_descriptor, const std::string& slot_name,
                              const struct stat& named_stat,
                              B1FileIdentity* slot_identity) {
  if (slot_identity == nullptr) {
    throw std::invalid_argument("B1 public slot identity output is null.");
  }
  if (!S_ISDIR(named_stat.st_mode)) {
    throw B1SlotExistsError();
  }
  const int descriptor =
      ::openat(root_descriptor, slot_name.c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw_errno("openat existing B1 public occurrence");
  }
  ScopedFileDescriptor owner(descriptor);
  const B1FileIdentity named_identity = file_identity(named_stat);
  const B1FileIdentity opened_identity = file_identity(
      directory_stat(owner.get(), "fstat existing B1 public occurrence"));
  if (opened_identity != named_identity) {
    throw B1RevalidationError(
        "B1 public occurrence identity changed during open.");
  }
  *slot_identity = opened_identity;
  return owner.release();
}

/**
 * @brief Validates that a pending private staging anchor is exactly empty.
 * @param anchor_descriptor Held staging-anchor directory.
 * @return Nothing when only dot entries exist.
 * @throws File, allocation, or B1RevalidationError on retained residue.
 */
void validate_empty_staging_anchor(int anchor_descriptor);

/**
 * @brief Reopens an optional pending staging anchor under the held root.
 * @param root_descriptor Held selected output root.
 * @param anchor_name Exact deterministic private anchor leaf.
 * @param anchor_identity Output when the anchor exists and matches its fd.
 * @return Owned descriptor, or `-1` when no pending anchor remains.
 * @throws File or revalidation failures for type, open, identity, or residue.
 * @note A present anchor must be exactly empty after public directory rename;
 * ambiguous residue is preserved and fails closed.
 */
int open_pending_staging_anchor(int root_descriptor,
                                const std::string& anchor_name,
                                B1FileIdentity* anchor_identity) {
  if (anchor_identity == nullptr) {
    throw std::invalid_argument("B1 pending anchor identity output is null.");
  }
  struct stat named_stat{};
  if (::fstatat(root_descriptor, anchor_name.c_str(), &named_stat,
                AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return -1;
    }
    throw_errno("fstatat pending B1 staging anchor");
  }
  if (!S_ISDIR(named_stat.st_mode)) {
    throw B1RevalidationError("B1 pending staging anchor is not a directory.");
  }
  const int descriptor =
      ::openat(root_descriptor, anchor_name.c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw_errno("openat pending B1 staging anchor");
  }
  ScopedFileDescriptor owner(descriptor);
  const B1FileIdentity named_identity = file_identity(named_stat);
  const B1FileIdentity opened_identity = file_identity(
      directory_stat(owner.get(), "fstat pending B1 staging anchor"));
  if (opened_identity != named_identity) {
    throw B1RevalidationError(
        "B1 pending staging anchor identity changed during open.");
  }
  validate_empty_staging_anchor(owner.get());
  *anchor_identity = opened_identity;
  return owner.release();
}

/**
 * @brief Validates that a public occurrence contains exactly its two leaves.
 * @param slot_descriptor Held public occurrence directory.
 * @return Nothing for exactly payload plus manifest and no other entry.
 * @throws B1SlotExistsError when no transaction-looking leaf is present.
 * @throws B1RevalidationError when any payload/manifest/private-manifest leaf
 * makes the entry set incomplete, extra, or drifted.
 * @throws File/allocation failures while opening or reading the directory.
 * @note Dot entries are ignored. No entry is opened or mutated by this scan.
 */
void validate_exact_public_slot_entries(int slot_descriptor) {
  const int scan_descriptor =
      ::openat(slot_descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan_descriptor < 0) {
    throw_errno("openat B1 public occurrence for entry scan");
  }
  DIR* raw_stream = ::fdopendir(scan_descriptor);
  if (raw_stream == nullptr) {
    const int saved_errno = errno;
    static_cast<void>(::close(scan_descriptor));
    errno = saved_errno;
    throw_errno("fdopendir B1 public occurrence");
  }
  ScopedDirectoryStream stream(raw_stream);
  bool payload_present = false;
  bool manifest_present = false;
  bool transaction_residue_present = false;
  bool unknown_present = false;
  for (;;) {
    errno = 0;
    const struct dirent* entry = ::readdir(stream.get());
    if (entry == nullptr) {
      if (errno != 0) {
        throw_errno("readdir B1 public occurrence");
      }
      break;
    }
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (name == kPayloadName) {
      payload_present = true;
    } else if (name == kManifestName) {
      manifest_present = true;
    } else if (name == kPrivateManifestName) {
      transaction_residue_present = true;
    } else {
      unknown_present = true;
    }
  }
  if (!payload_present && !manifest_present && !transaction_residue_present) {
    throw B1SlotExistsError();
  }
  if (unknown_present || transaction_residue_present || !payload_present ||
      !manifest_present) {
    throw B1RevalidationError(
        "B1 public occurrence is incomplete or contains drifted entries.");
  }
}

/**
 * @brief Validates that a pending private staging anchor is exactly empty.
 * @param anchor_descriptor Held staging-anchor directory.
 * @return Nothing when only dot entries exist.
 * @throws File, allocation, or B1RevalidationError on retained residue.
 * @note Reconciliation never removes an anchor whose contents are ambiguous.
 */
void validate_empty_staging_anchor(int anchor_descriptor) {
  const int scan_descriptor =
      ::openat(anchor_descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan_descriptor < 0) {
    throw_errno("openat B1 staging anchor for entry scan");
  }
  DIR* raw_stream = ::fdopendir(scan_descriptor);
  if (raw_stream == nullptr) {
    const int saved_errno = errno;
    static_cast<void>(::close(scan_descriptor));
    errno = saved_errno;
    throw_errno("fdopendir B1 staging anchor");
  }
  ScopedDirectoryStream stream(raw_stream);
  for (;;) {
    errno = 0;
    const struct dirent* entry = ::readdir(stream.get());
    if (entry == nullptr) {
      if (errno != 0) {
        throw_errno("readdir B1 staging anchor");
      }
      return;
    }
    const std::string_view name(entry->d_name);
    if (name != "." && name != "..") {
      throw B1RevalidationError(
          "B1 pending staging anchor contains ambiguous residue.");
    }
  }
}

/**
 * @brief Writes, links, barriers, and revalidates a private-slot manifest.
 * @param state Complete task state with verified payload digest/manifest.
 * @return Nothing after both leaves and the private slot are durable and exact.
 * @throws File, durability, and revalidation failures unchanged.
 * @note Public visibility is a later atomic directory publication after this
 * accepted task settles and releases its executor charge.
 */
void commit_and_validate_manifest(B1CommitTaskState* state) {
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
  state->private_manifest_identity = private_identity;
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
  state->manifest_identity = private_identity;
  if (::unlinkat(state->slot_descriptor, kPrivateManifestName, 0) != 0) {
    throw_errno("unlinkat private B1 manifest after publication");
  }
  state->private_manifest_identity.reset();

  B1FileIdentity published_identity;
  state->manifest_digest = validate_exact_file_at(
      state->slot_descriptor, kManifestName, state->manifest, &private_identity,
      &published_identity);
  state->published_identity = b1_file_identity_text(published_identity);

  synchronize_descriptor(state->slot_descriptor, "directory");
  const B1Sha256Digest after_barrier =
      validate_exact_file_at(state->slot_descriptor, kManifestName,
                             state->manifest, &published_identity);
  if (after_barrier != state->manifest_digest) {
    throw B1RevalidationError(
        "B1 manifest digest changed after namespace barriers.");
  }
}

/**
 * @brief Creates, observes, opens, and revalidates one private directory.
 * @param parent_descriptor Held trusted parent directory.
 * @param name Single private child name.
 * @param options Source-private fault seam policy.
 * @param handoff_fault_point Optional exact post-mkdir/pre-open seam.
 * @param identity Output for the pre-open and descriptor-matched identity.
 * @return Newly owned directory descriptor.
 * @throws File, hook, or revalidation failures unchanged.
 * @note No child mutation occurs before the named pre-open identity equals the
 * opened descriptor identity. A pre-guard anchor failure preserves the
 * ambiguous current name and propagates; a slot failure after anchor-guard
 * activation terminates during fail-stop cleanup if the anchor is nonempty.
 */
int create_and_open_private_directory(
    int parent_descriptor, const std::string& name,
    const B1OutputStoreOptions& options,
    std::optional<B1OutputStoreFaultPoint> handoff_fault_point,
    B1FileIdentity* identity) {
  if (identity == nullptr) {
    throw std::invalid_argument(
        "B1 private directory identity output is null.");
  }
  if (::mkdirat(parent_descriptor, name.c_str(), S_IRWXU) != 0) {
    if (errno == EEXIST) {
      throw B1RevalidationError("B1 private staging namespace already exists.");
    }
    throw_errno("mkdirat B1 private staging directory");
  }
  struct stat named{};
  if (::fstatat(parent_descriptor, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) !=
          0 ||
      !S_ISDIR(named.st_mode)) {
    throw B1RevalidationError(
        "B1 private staging directory cannot be observed safely.");
  }
  const B1FileIdentity before_open = file_identity(named);
  if (handoff_fault_point.has_value()) {
    invoke_fault_injector(options, *handoff_fault_point);
  }
  const int descriptor =
      ::openat(parent_descriptor, name.c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (descriptor < 0) {
    throw B1RevalidationError(
        "B1 private staging directory changed before descriptor takeover.");
  }
  ScopedFileDescriptor owner(descriptor);
  const B1FileIdentity after_open = file_identity(
      directory_stat(owner.get(), "fstat B1 private staging directory"));
  if (after_open != before_open) {
    throw B1RevalidationError(
        "B1 private staging directory identity changed during takeover.");
  }
  *identity = after_open;
  return owner.release();
}

/**
 * @brief Atomically publishes one complete private directory without replace.
 * @param staging_descriptor Held private staging anchor.
 * @param staging_name Exact private child slot.
 * @param root_descriptor Held selected public root.
 * @param slot_name Exact immutable public occurrence name.
 * @return Nothing only after one atomic no-replace namespace transition.
 * @throws B1SlotExistsError when the public destination is occupied.
 * @throws B1DurabilityError when the platform primitive is unavailable.
 * @throws std::system_error for other publication failures.
 */
void publish_private_directory_no_replace(int staging_descriptor,
                                          const std::string& staging_name,
                                          int root_descriptor,
                                          const std::string& slot_name) {
  int result = -1;
#if defined(__APPLE__)
  result = ::renameatx_np(staging_descriptor, staging_name.c_str(),
                          root_descriptor, slot_name.c_str(), RENAME_EXCL);
#elif defined(__linux__)
  result = static_cast<int>(::syscall(SYS_renameat2, staging_descriptor,
                                      staging_name.c_str(), root_descriptor,
                                      slot_name.c_str(), 1U));
#else
  throw B1DurabilityError(
      "B1 atomic directory no-replace publication is unsupported.");
#endif
  if (result == 0) {
    return;
  }
  if (errno == EEXIST || errno == ENOTEMPTY) {
    throw B1SlotExistsError();
  }
  if (errno == ENOSYS || errno == EINVAL || errno == ENOTSUP ||
      errno == EOPNOTSUPP) {
    throw B1DurabilityError(
        "B1 atomic directory no-replace publication is unsupported.");
  }
  throw_errno("publish B1 occurrence directory no-replace");
}

/**
 * @brief Runs the optional cleanup seam after one exact identity check.
 * @param options Source-private cleanup policy.
 * @param operation Exact object about to be removed.
 * @return Nothing when removal should proceed.
 * @throws std::system_error for an injected errno.
 */
void invoke_cleanup_injector(const B1OutputStoreOptions& options,
                             B1OutputStoreCleanupOperation operation) {
  if (options.cleanup_injector == nullptr) {
    return;
  }
  const int injected =
      options.cleanup_injector(options.cleanup_injector_context, operation);
  if (injected != 0) {
    throw std::system_error(injected, std::generic_category(),
                            "injected B1 strict cleanup failure");
  }
}

/**
 * @brief Runs the deterministic final-recheck/pre-remove failure seam.
 * @param options Source-private cleanup policy.
 * @param operation Exact object whose name was just rechecked.
 * @return Nothing when removal should proceed.
 * @throws std::system_error for an injected errno before name removal.
 * @note Namespace mutation at this boundary violates the cooperative exclusive
 * owner contract. The seam exists to prove fail-stop before `unlinkat`, not to
 * claim an unavailable atomic identity-selected POSIX removal.
 */
void invoke_final_cleanup_injector(const B1OutputStoreOptions& options,
                                   B1OutputStoreCleanupOperation operation) {
  if (options.final_cleanup_injector == nullptr) {
    return;
  }
  const int injected = options.final_cleanup_injector(
      options.final_cleanup_injector_context, operation);
  if (injected != 0) {
    throw std::system_error(injected, std::generic_category(),
                            "injected B1 final cleanup failure");
  }
}

/**
 * @brief Strictly removes one optional exact store-owned regular leaf.
 * @param directory_descriptor Held owning directory.
 * @param leaf Exact fixed leaf name.
 * @param expected Recorded creation identity, or empty before creation.
 * @param operation Closed cleanup operation for deterministic injection.
 * @param options Source-private cleanup policy.
 * @return Nothing after checked removal and a following absence observation.
 * @throws Revalidation, system, or injected failures unchanged.
 * @note Under the cooperative exclusive owner protocol, the second identity
 * check selects the same reserved name that `unlinkat` removes. POSIX does not
 * make those calls atomic; a non-cooperating same-UID mutation after the final
 * check is outside the contract. Absence before cleanup is accepted, while a
 * replacement detected by either check causes fail-stop before removal.
 */
void strictly_remove_leaf(int directory_descriptor, const char* leaf,
                          const std::optional<B1FileIdentity>& expected,
                          B1OutputStoreCleanupOperation operation,
                          const B1OutputStoreOptions& options) {
  struct stat named{};
  if (::fstatat(directory_descriptor, leaf, &named, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return;
    }
    throw_errno("fstatat B1 strict cleanup leaf");
  }
  if (!expected.has_value() || !S_ISREG(named.st_mode) ||
      file_identity(named) != *expected) {
    throw B1RevalidationError(
        "B1 strict cleanup refused an unowned leaf replacement.");
  }
  invoke_cleanup_injector(options, operation);
  struct stat rechecked{};
  if (::fstatat(directory_descriptor, leaf, &rechecked, AT_SYMLINK_NOFOLLOW) !=
          0 ||
      !S_ISREG(rechecked.st_mode) || file_identity(rechecked) != *expected) {
    throw B1RevalidationError(
        "B1 cleanup leaf identity changed after verification.");
  }
  invoke_final_cleanup_injector(options, operation);
  if (::unlinkat(directory_descriptor, leaf, 0) != 0) {
    throw_errno("unlinkat B1 strict cleanup leaf");
  }
  if (::fstatat(directory_descriptor, leaf, &rechecked, AT_SYMLINK_NOFOLLOW) ==
          0 ||
      errno != ENOENT) {
    throw B1RevalidationError(
        "B1 strict cleanup could not prove leaf-name absence.");
  }
}

/**
 * @brief Strictly removes one exact held directory by its current name.
 * @param parent_descriptor Held current namespace parent.
 * @param directory_descriptor Held directory being removed.
 * @param name Exact current child name.
 * @param expected Creation-time directory identity.
 * @param operation Closed cleanup operation for deterministic injection.
 * @param options Source-private cleanup policy.
 * @return Nothing after checked removal and a following absence observation.
 * @throws Revalidation, system, injected, or nonempty failures unchanged.
 * @note The final identity check and name-based `unlinkat` are separate POSIX
 * operations. Their binding relies on the class's cooperative exclusive owner
 * precondition; detected drift fails before removal.
 */
void strictly_remove_directory(int parent_descriptor, int directory_descriptor,
                               const std::string& name,
                               const B1FileIdentity& expected,
                               B1OutputStoreCleanupOperation operation,
                               const B1OutputStoreOptions& options) {
  const struct stat held =
      directory_stat(directory_descriptor, "fstat B1 strict cleanup directory");
  if (file_identity(held) != expected) {
    throw B1RevalidationError("B1 held cleanup directory identity drifted.");
  }
  struct stat named{};
  if (::fstatat(parent_descriptor, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) !=
          0 ||
      !S_ISDIR(named.st_mode) || file_identity(named) != expected) {
    throw B1RevalidationError(
        "B1 strict cleanup refused a directory-name replacement.");
  }
  invoke_cleanup_injector(options, operation);
  struct stat rechecked{};
  if (::fstatat(parent_descriptor, name.c_str(), &rechecked,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(rechecked.st_mode) || file_identity(rechecked) != expected) {
    throw B1RevalidationError(
        "B1 cleanup directory identity changed after verification.");
  }
  invoke_final_cleanup_injector(options, operation);
  if (::unlinkat(parent_descriptor, name.c_str(), AT_REMOVEDIR) != 0) {
    throw_errno("unlinkat B1 strict cleanup directory");
  }
  if (::fstatat(parent_descriptor, name.c_str(), &rechecked,
                AT_SYMLINK_NOFOLLOW) == 0 ||
      errno != ENOENT) {
    throw B1RevalidationError(
        "B1 strict cleanup could not prove directory-name absence.");
  }
}

/**
 * @brief Owns private rollback, public reconciliation, and task settlement.
 *
 * @throws Nothing from destruction; cancellation/wait/cleanup invariant
 * failures terminate because descriptor lifetime cannot otherwise be proved.
 * @note The guard is created immediately after staging-anchor fd acquisition.
 * Under the cooperative exclusive namespace contract, every exit settles
 * accepted work first. Private state then removes recorded names after identity
 * checks. Atomic public rename irreversibly changes the lifecycle to pending
 * reconciliation, where destruction closes descriptors but cannot delete the
 * public occurrence or its empty source anchor. A completed receipt changes the
 * lifecycle to committed and permits exact private-anchor cleanup. A takeover
 * before guard construction preserves ambiguous residue; detected later private
 * drift or cleanup failure terminates fail-stop.
 */
class B1CommitTransactionGuard final {
 public:
  /**
   * @brief Closed occurrence lifecycle controlling destructor mutation rights.
   * @throws Nothing for value construction and comparison.
   */
  enum class SlotLifecycle : std::uint8_t {
    /** @brief No slot descriptor has been adopted yet. */
    Unadopted,
    /** @brief Slot remains private and must be rolled back on failure. */
    PrivateRollback,
    /** @brief Public rename occurred; all public cleanup authority is revoked.
     */
    PublicPendingReconcile,
    /** @brief Complete receipt was minted; only empty anchor cleanup remains.
     */
    Committed,
  };

  /**
   * @brief Adopts one newly created verified private staging anchor.
   * @param root_descriptor Held original root directory.
   * @param anchor_descriptor Held private staging anchor.
   * @param anchor_name Exact root-relative private anchor name.
   * @param anchor_identity Creation-time anchor identity.
   * @param state Mutable task state retaining exact leaf identities.
   * @param options Source-private cleanup/fault policy.
   * @throws Nothing.
   */
  B1CommitTransactionGuard(int root_descriptor, int anchor_descriptor,
                           const std::string& anchor_name,
                           B1FileIdentity anchor_identity,
                           B1CommitTaskState* state,
                           const B1OutputStoreOptions& options) noexcept
      : root_descriptor_(root_descriptor),
        anchor_descriptor_(anchor_descriptor),
        anchor_name_(anchor_name),
        anchor_identity_(anchor_identity),
        state_(state),
        options_(options) {}

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
      if (slot_lifecycle_ == SlotLifecycle::PrivateRollback) {
        if (slot_descriptor_ < 0 || state_ == nullptr) {
          std::terminate();
        }
        strictly_remove_leaf(slot_descriptor_, kPrivateManifestName,
                             state_->private_manifest_identity,
                             B1OutputStoreCleanupOperation::PrivateManifestLeaf,
                             options_);
        strictly_remove_leaf(
            slot_descriptor_, kManifestName, state_->manifest_identity,
            B1OutputStoreCleanupOperation::ManifestLeaf, options_);
        strictly_remove_leaf(
            slot_descriptor_, kPayloadName, state_->payload_identity,
            B1OutputStoreCleanupOperation::PayloadLeaf, options_);
        synchronize_descriptor(slot_descriptor_, "cleanup directory");
        strictly_remove_directory(slot_parent_descriptor_, slot_descriptor_,
                                  slot_name_, slot_identity_,
                                  B1OutputStoreCleanupOperation::SlotDirectory,
                                  options_);
        synchronize_descriptor(slot_parent_descriptor_,
                               "cleanup parent directory");
      }
      if (slot_lifecycle_ != SlotLifecycle::PublicPendingReconcile) {
        strictly_remove_directory(
            root_descriptor_, anchor_descriptor_, anchor_name_,
            anchor_identity_,
            B1OutputStoreCleanupOperation::StagingAnchorDirectory, options_);
        synchronize_descriptor(root_descriptor_, "cleanup root directory");
      }
      if (slot_descriptor_ >= 0 && ::close(slot_descriptor_) != 0) {
        std::terminate();
      }
      slot_descriptor_ = -1;
    } catch (...) {
      std::terminate();
    }
  }

  /** @brief Transaction ownership cannot be copied. */
  B1CommitTransactionGuard(const B1CommitTransactionGuard&) = delete;

  /** @brief Transaction ownership cannot be assigned. */
  B1CommitTransactionGuard& operator=(const B1CommitTransactionGuard&) = delete;

  /**
   * @brief Adopts the verified private slot fd before artifact mutation.
   * @param slot_descriptor Owned private slot directory descriptor.
   * @param slot_name Exact child name under the staging anchor.
   * @param slot_identity Creation-time slot identity.
   * @return Nothing.
   * @throws Nothing; duplicate adoption terminates as an invariant breach.
   */
  void adopt_slot(int slot_descriptor, const std::string& slot_name,
                  B1FileIdentity slot_identity) noexcept {
    if (slot_descriptor_ >= 0 || slot_descriptor < 0 ||
        slot_lifecycle_ != SlotLifecycle::Unadopted) {
      std::terminate();
    }
    slot_descriptor_ = slot_descriptor;
    slot_parent_descriptor_ = anchor_descriptor_;
    slot_name_ = slot_name;
    slot_identity_ = slot_identity;
    slot_lifecycle_ = SlotLifecycle::PrivateRollback;
  }

  /**
   * @brief Returns the held slot descriptor after successful adoption.
   * @return Nonnegative transaction-owned slot capability.
   * @throws Nothing; use before adoption is an invariant breach.
   */
  int slot_descriptor() const noexcept {
    if (slot_descriptor_ < 0) {
      std::terminate();
    }
    return slot_descriptor_;
  }

  /**
   * @brief Revokes cleanup authority after atomic public publication.
   * @return Nothing.
   * @throws Nothing; missing/private lifecycle drift terminates.
   * @note The public occurrence and empty source anchor remain intact on every
   * later failure so a same-commit retry can finish missing barriers.
   */
  void mark_slot_published() noexcept {
    if (slot_descriptor_ < 0 ||
        slot_lifecycle_ != SlotLifecycle::PrivateRollback) {
      std::terminate();
    }
    slot_lifecycle_ = SlotLifecycle::PublicPendingReconcile;
  }

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
   * @brief Marks public reconciliation committed after receipt construction.
   * @return Nothing.
   * @throws Nothing; active task or lifecycle drift terminates.
   */
  void preserve_slot() noexcept {
    if (completion_active_ ||
        slot_lifecycle_ != SlotLifecycle::PublicPendingReconcile) {
      std::terminate();
    }
    slot_lifecycle_ = SlotLifecycle::Committed;
  }

 private:
  /** @brief Held original root directory descriptor. */
  int root_descriptor_ = -1;
  /** @brief Held private staging-anchor descriptor. */
  int anchor_descriptor_ = -1;
  /** @brief Exact private staging-anchor name under the selected root. */
  const std::string& anchor_name_;
  /** @brief Creation-time private anchor identity. */
  B1FileIdentity anchor_identity_;
  /** @brief Held occurrence-slot directory descriptor after adoption. */
  int slot_descriptor_ = -1;
  /** @brief Private parent of `slot_name_` while rollback remains legal. */
  int slot_parent_descriptor_ = -1;
  /** @brief Exact private slot leaf while rollback remains legal. */
  std::string slot_name_;
  /** @brief Creation-time slot identity. */
  B1FileIdentity slot_identity_;
  /** @brief Borrowed task state valid past guard destruction. */
  B1CommitTaskState* state_ = nullptr;
  /** @brief Borrowed store options valid past guard destruction. */
  const B1OutputStoreOptions& options_;
  /** @brief Current accepted task completion, when any. */
  execution::ComputeIoCompletion completion_;
  /** @brief Whether destruction must cancel/wait `completion_`. */
  bool completion_active_ = false;
  /** @brief Current private/public mutation-right lifecycle. */
  SlotLifecycle slot_lifecycle_ = SlotLifecycle::Unadopted;
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

/**
 * @brief Shared duplicated descriptor state behind root-authority copies.
 * @throws std::bad_alloc when owned root storage cannot be constructed.
 * @note The duplicated descriptor shares the store's open-file description
 * and advisory lock. Final close failure terminates because silently losing
 * proof of authority lifetime would violate the capability contract.
 */
struct B1OutputStoreRootAuthority::State final {
  /**
   * @brief Adopts one duplicated descriptor and its frozen binding identity.
   * @param root Canonical selected path that must continue naming the fd.
   * @param device Frozen descriptor device identity.
   * @param inode Frozen descriptor inode identity.
   * @param descriptor Owned nonnegative duplicated directory descriptor.
   * @throws std::bad_alloc when `root` movement allocates unexpectedly.
   */
  State(std::filesystem::path root, std::uint64_t device, std::uint64_t inode,
        int descriptor)
      : root(std::move(root)),
        device(device),
        inode(inode),
        descriptor(descriptor) {}

  /**
   * @brief Closes the exact duplicated authority descriptor.
   * @throws Nothing; close failure terminates fail-stop.
   */
  ~State() noexcept {
#if !defined(_WIN32)
    if (descriptor >= 0 && ::close(descriptor) != 0) {
      std::terminate();
    }
#endif
  }

  /** @brief Shared authority state cannot duplicate descriptor ownership. */
  State(const State&) = delete;

  /** @brief Shared authority state cannot be reassigned. */
  State& operator=(const State&) = delete;

  /** @brief Canonical selected pathname used for live binding checks. */
  std::filesystem::path root;
  /** @brief Frozen descriptor device identity. */
  std::uint64_t device = 0U;
  /** @brief Frozen descriptor inode identity. */
  std::uint64_t inode = 0U;
  /** @brief Owned duplicated descriptor, negative only on unsupported hosts. */
  int descriptor = -1;
};

B1OutputStoreRootAuthority::B1OutputStoreRootAuthority(
    const B1OutputStoreRootAuthority&) noexcept =
    default;  // NOLINT(whitespace/indent_namespace)

B1OutputStoreRootAuthority::B1OutputStoreRootAuthority(
    B1OutputStoreRootAuthority&&) noexcept =
    default;  // NOLINT(whitespace/indent_namespace)

B1OutputStoreRootAuthority& B1OutputStoreRootAuthority::operator=(
    const B1OutputStoreRootAuthority&) noexcept = default;

B1OutputStoreRootAuthority& B1OutputStoreRootAuthority::operator=(
    B1OutputStoreRootAuthority&&) noexcept = default;

B1OutputStoreRootAuthority::~B1OutputStoreRootAuthority() noexcept = default;

B1OutputStoreRootAuthority::B1OutputStoreRootAuthority(
    std::shared_ptr<const State> state)
    : state_(std::move(state)) {
  if (!state_) {
    throw std::invalid_argument("B1 root authority state is missing.");
  }
}

B1OutputStoreRootObservation B1OutputStoreRootAuthority::observe() const {
#if defined(_WIN32)
  throw std::runtime_error(
      "B1 retained root authority is unsupported on Windows.");
#else
  if (!state_) {
    throw std::runtime_error("B1 retained root authority was moved from.");
  }
  verify_root_binding(state_->root, state_->descriptor, state_->device,
                      state_->inode);
  const struct stat root_stat =
      directory_stat(state_->descriptor, "fstat retained B1 output root");
  std::ostringstream identity;
  identity << "dev=" << static_cast<std::uint64_t>(root_stat.st_dev)
           << ";ino=" << static_cast<std::uint64_t>(root_stat.st_ino);
  return B1OutputStoreRootObservation{
      state_->root, identity.str(),
      filesystem_type_from_descriptor(state_->descriptor)};
#endif
}

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
  if (::flock(owner.get(), LOCK_EX | LOCK_NB) != 0) {
    throw_errno("acquire exclusive B1 output-root ownership");
  }
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

B1OutputStoreRootObservation B1OutputStore::observe_root_authority() const {
#if defined(_WIN32)
  throw std::runtime_error(
      "B1 root authority observation is unsupported on Windows.");
#else
  verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
  const struct stat root_stat =
      directory_stat(root_descriptor_, "fstat observed B1 output root");
  std::ostringstream identity;
  identity << "dev=" << static_cast<std::uint64_t>(root_stat.st_dev)
           << ";ino=" << static_cast<std::uint64_t>(root_stat.st_ino);
  return B1OutputStoreRootObservation{
      root_, identity.str(), filesystem_type_from_descriptor(root_descriptor_)};
#endif
}

B1OutputStoreRootAuthority B1OutputStore::retain_root_authority() const {
#if defined(_WIN32)
  throw std::runtime_error(
      "B1 retained root authority is unsupported on Windows.");
#else
  verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
  const int duplicated = ::fcntl(root_descriptor_, F_DUPFD_CLOEXEC, 0);
  if (duplicated < 0) {
    throw_errno("duplicate retained B1 output root descriptor");
  }
  ScopedFileDescriptor owner(duplicated);
  auto state = std::make_shared<B1OutputStoreRootAuthority::State>(
      root_, root_device_, root_inode_, owner.get());
  static_cast<void>(owner.release());
  return B1OutputStoreRootAuthority(std::move(state));
#endif
}

B1OutputCommitResult B1OutputStore::commit(const B1JobInstance& job,
                                           const Value& image) {
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
  try {
    verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
  } catch (const std::exception& error) {
    result.status = B1OutputCommitStatus::RootUnavailable;
    result.diagnostic = error.what();
    return result;
  }

  B1Sha256 commit_hash;
  commit_hash.update("execution-profile-output-commit-id-v1\n");
  commit_hash.update(encode_b1_job_instance(job));
  const std::string commit_id = b1_digest_hex(commit_hash.finish());
  const std::filesystem::path rooted_slot = "occurrence-" + commit_id;
  const std::string slot_name = rooted_slot.string();
  const std::string anchor_name = ".b1-staging-" + commit_id;
  struct stat existing_slot{};
  if (::fstatat(root_descriptor_, slot_name.c_str(), &existing_slot,
                AT_SYMLINK_NOFOLLOW) == 0) {
    try {
      const B1Sha256Digest expected_payload_digest =
          digest_b1_payload_image(image);
      const std::string expected_manifest =
          b1_artifact_manifest(job.job_index, expected_payload_digest);
      const B1Sha256Digest expected_manifest_digest =
          b1_sha256(expected_manifest);

      B1FileIdentity slot_identity;
      const int slot_raw = open_existing_public_slot(
          root_descriptor_, slot_name, existing_slot, &slot_identity);
      ScopedFileDescriptor slot_owner(slot_raw);
      validate_exact_public_slot_entries(slot_owner.get());

      B1FileIdentity payload_identity;
      static_cast<void>(validate_b1_payload_at(slot_owner.get(),
                                               expected_payload_digest, nullptr,
                                               &payload_identity));
      B1FileIdentity manifest_identity;
      const B1Sha256Digest observed_manifest_digest = validate_exact_file_at(
          slot_owner.get(), kManifestName, expected_manifest, nullptr,
          &manifest_identity);
      if (observed_manifest_digest != expected_manifest_digest) {
        throw B1RevalidationError(
            "B1 existing manifest digest differs from the same commit.");
      }

      B1FileIdentity anchor_identity;
      const int anchor_raw = open_pending_staging_anchor(
          root_descriptor_, anchor_name, &anchor_identity);
      std::optional<ScopedFileDescriptor> anchor_owner;
      if (anchor_raw >= 0) {
        anchor_owner.emplace(anchor_raw);
      }

      invoke_fault_injector(
          options_,
          B1OutputStoreFaultPoint::AfterSlotPublicationBeforeSourceBarrier);
      synchronize_descriptor(slot_owner.get(),
                             "published occurrence directory");
      if (anchor_owner.has_value()) {
        synchronize_descriptor(anchor_owner->get(), "pending source directory");
      }
      invoke_fault_injector(
          options_,
          B1OutputStoreFaultPoint::AfterSourceBarrierBeforeRootBarrier);
      synchronize_descriptor(root_descriptor_, "directory");
      invoke_fault_injector(
          options_, B1OutputStoreFaultPoint::BeforeFinalPublicRevalidation);

      verify_slot_binding(root_descriptor_, slot_owner.get(), slot_name,
                          slot_identity);
      verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
      validate_exact_public_slot_entries(slot_owner.get());
      static_cast<void>(validate_b1_payload_at(
          slot_owner.get(), expected_payload_digest, &payload_identity));
      const B1Sha256Digest final_manifest_digest =
          validate_exact_file_at(slot_owner.get(), kManifestName,
                                 expected_manifest, &manifest_identity);
      if (final_manifest_digest != expected_manifest_digest) {
        throw B1RevalidationError(
            "B1 manifest digest changed during public reconciliation.");
      }
      const ContentDigest logical_digest = compute_b1_logical_digest(image);
      invoke_fault_injector(options_,
                            B1OutputStoreFaultPoint::BeforeReceiptAssembly);
      B1OutputCommitReceipt receipt(B1OutputCommitReceipt::Fields{
          commit_id, root_, rooted_slot, job,
          "dense-tensor-hwc-fp32-rgba-2048x2048", logical_digest, 1U,
          kPayloadName, kManifestName, kB1PayloadBytes,
          b1_manifest_length(job.job_index), expected_payload_digest,
          expected_manifest_digest, B1OutputDurability::CrashDurable,
          B1OutputDurability::CrashDurable,
          b1_file_identity_text(manifest_identity)});

      if (anchor_owner.has_value()) {
        strictly_remove_directory(
            root_descriptor_, anchor_owner->get(), anchor_name, anchor_identity,
            B1OutputStoreCleanupOperation::StagingAnchorDirectory, options_);
        synchronize_descriptor(root_descriptor_,
                               "reconciled cleanup root directory");
      }
      result.receipt = std::move(receipt);
      result.status = B1OutputCommitStatus::Succeeded;
      return result;
    } catch (const B1SlotExistsError& error) {
      result.status = B1OutputCommitStatus::SlotExists;
      result.diagnostic = error.what();
      return result;
    } catch (const B1RootBindingError& error) {
      result.status = B1OutputCommitStatus::RootUnavailable;
      result.diagnostic = error.what();
      return result;
    } catch (const B1DurabilityError& error) {
      result.status = B1OutputCommitStatus::DurabilityUnsupported;
      result.diagnostic = error.what();
      return result;
    } catch (const std::exception& error) {
      result.status = B1OutputCommitStatus::RevalidationFailed;
      result.diagnostic = error.what();
      return result;
    }
  }
  if (errno != ENOENT) {
    result.status = B1OutputCommitStatus::RootUnavailable;
    result.diagnostic = "Cannot inspect the B1 occurrence slot: " +
                        std::string(std::strerror(errno));
    return result;
  }

  auto state = std::make_shared<B1CommitTaskState>();
  B1FileIdentity anchor_identity;
  const int anchor_raw = create_and_open_private_directory(
      root_descriptor_, anchor_name, options_,
      B1OutputStoreFaultPoint::AfterStagingAnchorMkdirBeforeOpen,
      &anchor_identity);
  ScopedFileDescriptor anchor_owner(anchor_raw);
  B1CommitTransactionGuard transaction(root_descriptor_, anchor_owner.get(),
                                       anchor_name, anchor_identity,
                                       state.get(), options_);

  const std::string staging_slot_name = "slot";
  B1FileIdentity slot_identity;
  const int slot_raw = create_and_open_private_directory(
      anchor_owner.get(), staging_slot_name, options_,
      B1OutputStoreFaultPoint::AfterStagingSlotMkdirBeforeOpen, &slot_identity);
  ScopedFileDescriptor slot_owner(slot_raw);
  transaction.adopt_slot(slot_owner.release(), staging_slot_name,
                         slot_identity);
  invoke_fault_injector(options_, B1OutputStoreFaultPoint::AfterSlotCreated);

  state->slot_descriptor = transaction.slot_descriptor();
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
               [state]() { commit_and_validate_manifest(state.get()); });
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
    verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
    const ContentDigest logical_digest = compute_b1_logical_digest(image);
    invoke_fault_injector(options_,
                          B1OutputStoreFaultPoint::BeforeSlotPublication);
    verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
    publish_private_directory_no_replace(anchor_owner.get(), staging_slot_name,
                                         root_descriptor_, slot_name);
    transaction.mark_slot_published();
    invoke_fault_injector(
        options_,
        B1OutputStoreFaultPoint::AfterSlotPublicationBeforeSourceBarrier);
    synchronize_descriptor(anchor_owner.get(), "directory");
    invoke_fault_injector(
        options_, B1OutputStoreFaultPoint::AfterSourceBarrierBeforeRootBarrier);
    synchronize_descriptor(root_descriptor_, "directory");
    invoke_fault_injector(
        options_, B1OutputStoreFaultPoint::BeforeFinalPublicRevalidation);
    verify_slot_binding(root_descriptor_, transaction.slot_descriptor(),
                        slot_name, slot_identity);
    verify_root_binding(root_, root_descriptor_, root_device_, root_inode_);
    validate_exact_public_slot_entries(transaction.slot_descriptor());
    if (!state->payload_identity.has_value() ||
        !state->manifest_identity.has_value()) {
      throw B1RevalidationError(
          "B1 published payload/manifest identity was not retained.");
    }
    static_cast<void>(validate_b1_payload_at(transaction.slot_descriptor(),
                                             state->payload_digest,
                                             &*state->payload_identity));
    const B1Sha256Digest after_publication =
        validate_exact_file_at(transaction.slot_descriptor(), kManifestName,
                               state->manifest, &*state->manifest_identity);
    if (after_publication != state->manifest_digest) {
      throw B1RevalidationError(
          "B1 manifest digest changed after directory publication.");
    }
    invoke_fault_injector(options_,
                          B1OutputStoreFaultPoint::BeforeReceiptAssembly);
    result.receipt = B1OutputCommitReceipt(B1OutputCommitReceipt::Fields{
        commit_id, root_, rooted_slot, job,
        "dense-tensor-hwc-fp32-rgba-2048x2048", logical_digest, 1U,
        kPayloadName, kManifestName, kB1PayloadBytes,
        b1_manifest_length(job.job_index), state->payload_digest,
        state->manifest_digest, B1OutputDurability::CrashDurable,
        B1OutputDurability::CrashDurable, state->published_identity});
    result.status = B1OutputCommitStatus::Succeeded;
    transaction.preserve_slot();
    return result;
  } catch (const B1SlotExistsError& error) {
    result.status = B1OutputCommitStatus::SlotExists;
    result.diagnostic = error.what();
    return result;
  } catch (const B1RootBindingError& error) {
    result.status = B1OutputCommitStatus::RootUnavailable;
    result.diagnostic = error.what();
    return result;
  } catch (const B1DurabilityError& error) {
    result.status = B1OutputCommitStatus::DurabilityUnsupported;
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
